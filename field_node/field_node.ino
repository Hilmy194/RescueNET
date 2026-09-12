/*
 * RescueNet - FIELD NODE FIRMWARE
 * ================================
 * Board   : LILYGO T-Beam V1.2 AXP2101 - ESP32 + SX1276 + GPS NEO-M8N + PMU AXP2101
 * Radio   : LoRa SX1276 (library: sandeepmistry/LoRa)
 * PMU     : AXP2101 (library: XPowersLib by lewisxhe) -- WAJIB, tanpa ini radio LoRa
 *           dan GPS tidak dapat suplai listrik sama sekali.
 * Display : OLED SSD1306 (opsional, hanya jika modul OLED terpasang di board Anda)
 * Fungsi  :
 *   1. Membuat WiFi Access Point + Captive Portal (portal bantuan lokal)
 *   2. Menerima input laporan korban dari smartphone via form web
 *   3. Mengirim laporan via LoRa menuju gateway (langsung / multi-hop)
 *   4. Meneruskan (relay) paket LoRa dari node lain -> flooding mesh
 *   5. Menampilkan status node di OLED (jika ada) & Serial Monitor
 *
 * PENTING: Ubah NODE_ID di bawah untuk SETIAP field node sebelum flashing!
 * Node ID harus unik: 1, 2, 3, dst. Gateway selalu ID 0.
 *
 * Tombol fisik T-Beam: RST (reset chip, bukan GPIO biasa), PWR (nyala/mati via
 * PMU, bukan GPIO biasa), USER/IO38 (satu-satunya tombol yang terhubung
 * langsung ke GPIO ESP32) -- dipakai di firmware ini sebagai tombol SOS.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <XPowersLib.h>            // Install via Library Manager: "XPowersLib" by lewisxhe
#include <U8g2lib.h>               // Install via Library Manager: "U8g2" by oliver (OLED opsional)

// ================== KONFIGURASI NODE (WAJIB DIUBAH PER PERANGKAT) ==================
#define NODE_ID        1          // <-- UBAH: ID unik node ini (1,2,3,...)
#define GATEWAY_ID     0          // ID gateway, jangan diubah
#define MAX_HOP        5          // TTL maksimum paket sebelum dibuang (cegah looping)
#define SSID_PREFIX    "RescueNet-Node"   // SSID akan jadi "RescueNet-Node1", dst.

// ================== PIN LoRa ONBOARD T-BEAM V1.2 ==================
// SPI (SCK/MISO/MOSI) memakai pin default HSPI ESP32 (5/19/27), tidak perlu didefinisikan manual.
#define LORA_SS    18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_FREQ  923E6          // 923 MHz sesuai regulasi SDPPI Indonesia (AS923)
                                   // Catatan: pastikan varian board Anda 868/915 MHz (SX1276
                                   // punya rentang 862-1020MHz jadi 923MHz masih tercakup).
                                   // Jangan pakai varian 433MHz, tidak akan bisa di-tune ke 923MHz.

// ================== PIN PMU AXP2101 (I2C) ==================
#define PMU_SDA 21
#define PMU_SCL 22
#define PMU_IRQ 35
XPowersPMU PMU;
bool pmuOK = false;

// ================== PIN OLED (OPSIONAL, via I2C bus sama dengan PMU) ==================
#define OLED_SDA 21
#define OLED_SCL 22
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
bool oledOK = false;

// ================== PIN LAINNYA ==================
#define SOS_BUTTON_PIN 38         // Tombol USER bawaan T-Beam (satu-satunya tombol ke GPIO langsung)
#define LED_PIN        4          // LED indikator (T-Beam v1.2: cek label board, umumnya GPIO4 atau GPIO14)

uint32_t txCount = 0, rxCount = 0;

float readBatteryVoltage() {
  if (!pmuOK) return 0.0;
  return PMU.getBattVoltage() / 1000.0;   // XPowersLib mengembalikan satuan mV
}

void initPMU() {
  pmuOK = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);
  if (!pmuOK) {
    Serial.println("PMU AXP2101 GAGAL diinisialisasi! LoRa/GPS tidak akan menyala.");
    return;
  }
  // Nyalakan rail daya untuk modul LoRa & GPS (sesuai referensi firmware resmi T-Beam v1.2)
  PMU.setALDO2(3300);  PMU.enableALDO2();   // suplai ke modul LoRa
  PMU.setALDO3(3300);  PMU.enableALDO3();   // suplai ke modul GPS
  Serial.println("PMU AXP2101 siap. Rail LoRa (ALDO2) & GPS (ALDO3) dinyalakan.");
}

void updateOLED(String lastEvent) {
  if (!oledOK) return;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, ("RescueNet Node " + String(NODE_ID)).c_str());
  u8g2.drawStr(0, 24, ("TX:" + String(txCount) + "  RX:" + String(rxCount)).c_str());
  u8g2.drawStr(0, 38, ("Bat: " + String(readBatteryVoltage(), 2) + "V").c_str());
  u8g2.drawStr(0, 52, lastEvent.c_str());
  u8g2.sendBuffer();
}

// ================== JARINGAN WiFi CAPTIVE PORTAL ==================
DNSServer dnsServer;
WebServer server(80);
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);

// ================== DEDUP CACHE (mencegah relay berulang / broadcast storm) ==================
#define CACHE_SIZE 25
uint16_t seenPackets[CACHE_SIZE];
int cacheIndex = 0;
uint16_t packetCounter = 0;

bool alreadySeen(uint16_t id) {
  for (int i = 0; i < CACHE_SIZE; i++) if (seenPackets[i] == id) return true;
  return false;
}
void markSeen(uint16_t id) {
  seenPackets[cacheIndex] = id;
  cacheIndex = (cacheIndex + 1) % CACHE_SIZE;
}

// ================== HALAMAN PORTAL (HTML) ==================
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RescueNet - Bantuan Darurat</title>
<style>
body{font-family:sans-serif;margin:0;padding:16px;background:#1a1a2e;color:#eee}
.card{background:#16213e;padding:20px;border-radius:12px;max-width:420px;margin:auto}
h1{color:#e94560;font-size:20px;margin-top:0}
label{display:block;margin-top:12px;font-size:14px}
input,select,textarea{width:100%;padding:10px;margin-top:4px;border-radius:6px;border:none;box-sizing:border-box;font-size:14px}
button{width:100%;padding:14px;margin-top:16px;border:none;border-radius:8px;font-weight:bold;font-size:16px}
.btn-submit{background:#0f9d58;color:white}
.btn-sos{background:#e94560;color:white;margin-top:10px}
#gpsStatus{font-size:12px;color:#aaa;margin-top:6px}
.info{font-size:13px;color:#aaa}
</style></head><body>
<div class="card">
<h1>&#128680; RescueNet - Portal Bantuan</h1>
<p class="info">Jaringan darurat offline (tanpa internet/pulsa). Isi form untuk mengirim laporan ke posko.</p>
<form action="/submit" method="POST">
<label>Nama / Kode Pelapor</label>
<input name="nama" placeholder="Nama Anda">
<label>Kondisi Korban</label>
<select name="kondisi">
<option value="RINGAN">Ringan</option>
<option value="SEDANG" selected>Sedang</option>
<option value="BERAT">Berat</option>
<option value="KRITIS">Kritis</option>
</select>
<label>Jumlah Korban</label>
<input name="jumlah" type="number" value="1" min="1">
<label>Lokasi (isi jika GPS tidak aktif: patokan/landmark)</label>
<textarea name="lokasi" rows="2" placeholder="Contoh: dekat masjid RW03, lantai 2 gedung sekolah"></textarea>
<label>Pesan Tambahan</label>
<textarea name="pesan" rows="2" placeholder="Keterangan tambahan"></textarea>
<input type="hidden" name="lat" id="lat" value="0">
<input type="hidden" name="lon" id="lon" value="0">
<div id="gpsStatus">Mencoba mengambil lokasi GPS...</div>
<button class="btn-submit" type="submit">Kirim Laporan</button>
</form>
<form action="/sos" method="GET">
<button class="btn-sos" type="submit">&#128680; KIRIM SOS DARURAT</button>
</form>
</div>
<script>
if (navigator.geolocation) {
  navigator.geolocation.getCurrentPosition(function(p){
    document.getElementById('lat').value = p.coords.latitude;
    document.getElementById('lon').value = p.coords.longitude;
    document.getElementById('gpsStatus').innerText = 'GPS aktif: ' + p.coords.latitude.toFixed(5) + ', ' + p.coords.longitude.toFixed(5);
  }, function(e){
    document.getElementById('gpsStatus').innerText = 'GPS tidak tersedia. Mohon isi kolom Lokasi secara manual di bawah.';
  }, {timeout:5000});
} else {
  document.getElementById('gpsStatus').innerText = 'Perangkat tidak mendukung GPS. Isi kolom Lokasi secara manual.';
}
</script>
</body></html>
)rawliteral";

// ================== HANDLER WEB ==================
void handleRoot() {
  server.send_P(200, "text/html", PORTAL_HTML);
}

// Redirect semua request tak dikenal ke portal (captive portal behaviour)
void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

String sanitize(String s, int maxLen) {
  s.replace(",", ";");
  s.replace("\n", " ");
  s.replace("\r", " ");
  if (s.length() > (unsigned)maxLen) s = s.substring(0, maxLen);
  if (s.length() == 0) s = "-";
  return s;
}

void sendLoRaPacket(String payload) {
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();
  digitalWrite(LED_PIN, HIGH);
  delay(80);
  digitalWrite(LED_PIN, LOW);
  txCount++;
  updateOLED("TX paket");
}

// Membuat & mengirim paket laporan baru (originasi dari node ini)
void sendReport(bool hasGps, float lat, float lon, String kondisi, int jumlah, bool sos, String pesan) {
  uint16_t pktId = ((uint16_t)NODE_ID << 12) | (packetCounter++ & 0x0FFF);
  markSeen(pktId);

  // Format CSV: PKT_ID,SRC_ID,HOP,MAX_HOP,LAT,LON,HAS_GPS,KONDISI,JUMLAH,SOS,PESAN
  String payload = String(pktId) + "," + String(NODE_ID) + ",0," + String(MAX_HOP) + "," +
                    String(lat, 6) + "," + String(lon, 6) + "," + String(hasGps ? 1 : 0) + "," +
                    kondisi + "," + String(jumlah) + "," + String(sos ? 1 : 0) + "," + pesan;

  Serial.println("[TX] " + payload);
  sendLoRaPacket(payload);
}

void handleSubmit() {
  String nama    = server.hasArg("nama")    ? sanitize(server.arg("nama"), 20)    : "-";
  String latStr  = server.hasArg("lat")     ? server.arg("lat")  : "0";
  String lonStr  = server.hasArg("lon")     ? server.arg("lon")  : "0";
  bool hasGps    = (latStr.toFloat() != 0.0 || lonStr.toFloat() != 0.0);
  String lokasi  = server.hasArg("lokasi")  ? sanitize(server.arg("lokasi"), 40)  : "";
  String kondisi = server.hasArg("kondisi") ? server.arg("kondisi") : "SEDANG";
  int jumlah     = server.hasArg("jumlah")  ? server.arg("jumlah").toInt() : 1;
  String pesanRaw = server.hasArg("pesan")  ? server.arg("pesan") : "";
  // Gabungkan nama + lokasi manual + pesan menjadi satu field pesan ringkas (batas payload LoRa)
  String pesan = sanitize(nama + "|" + lokasi + "|" + pesanRaw, 70);

  sendReport(hasGps, latStr.toFloat(), lonStr.toFloat(), kondisi, jumlah, false, pesan);

  server.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding-top:60px'>"
    "<h2 style='color:#0f9d58'>&#10004; Laporan terkirim ke posko</h2>"
    "<p>Mohon tetap tenang, bantuan sedang dikoordinasikan.</p>"
    "<a href='/' style='color:#e94560'>Kembali ke Portal</a></body></html>");
}

void handleSOS() {
  sendReport(false, 0, 0, "KRITIS", 1, true, "SOS-TOMBOL-PORTAL");
  server.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding-top:60px'>"
    "<h2 style='color:#e94560'>&#128680; Sinyal SOS terkirim!</h2>"
    "<a href='/' style='color:#eee'>Kembali</a></body></html>");
}

// ================== PARSING & FORWARDING PAKET LoRa (MESH RELAY) ==================
void splitCSV(String s, String out[], int maxFields) {
  int start = 0;
  int fieldIdx = 0;
  for (int i = 0; i <= s.length() && fieldIdx < maxFields; i++) {
    if (i == s.length() || s[i] == ',') {
      // field terakhir (pesan) boleh mengandung sisa string jika sudah field ke-(maxFields-1)
      if (fieldIdx == maxFields - 1) {
        out[fieldIdx] = s.substring(start);
        break;
      }
      out[fieldIdx] = s.substring(start, i);
      start = i + 1;
      fieldIdx++;
    }
  }
}

void checkLoRaReceive() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String incoming = "";
  while (LoRa.available()) incoming += (char)LoRa.read();

  const int NFIELD = 11;
  String f[NFIELD];
  splitCSV(incoming, f, NFIELD);

  if (f[0].length() == 0) return; // paket tidak valid/corrupt

  uint16_t pktId = f[0].toInt();
  int hop        = f[2].toInt();
  int maxHop     = f[3].toInt();

  if (alreadySeen(pktId)) return;      // sudah pernah diteruskan node ini
  markSeen(pktId);

  Serial.println("[RX] " + incoming + "  (RSSI:" + String(LoRa.packetRssi()) + ")");
  rxCount++;
  updateOLED("RX dari Node " + f[1]);

  if (hop + 1 > maxHop) {
    Serial.println("[DROP] TTL habis, paket dibuang");
    return;
  }

  // Jeda acak agar antar-node relay tidak bertabrakan (LoRa half-duplex)
  delay(random(50, 350));

  f[2] = String(hop + 1); // increment hop count
  String forwarded = f[0] + "," + f[1] + "," + f[2] + "," + f[3] + "," + f[4] + "," + f[5] + "," +
                      f[6] + "," + f[7] + "," + f[8] + "," + f[9] + "," + f[10];

  Serial.println("[RELAY] " + forwarded);
  sendLoRaPacket(forwarded);
}

// ================== SETUP & LOOP ==================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);
  randomSeed(analogRead(0) + NODE_ID + millis());

  Wire.begin(PMU_SDA, PMU_SCL);

  // Deteksi otomatis apakah OLED terpasang (alamat I2C standar SSD1306 = 0x3C)
  Wire.beginTransmission(0x3C);
  oledOK = (Wire.endTransmission() == 0);
  if (oledOK) {
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 20, "RescueNet");
    u8g2.drawStr(0, 34, "Booting...");
    u8g2.sendBuffer();
    Serial.println("OLED terdeteksi.");
  } else {
    Serial.println("OLED tidak terdeteksi (board tanpa layar) -- status hanya via Serial Monitor.");
  }

  initPMU();

  String ssid = String(SSID_PREFIX) + String(NODE_ID);
  WiFi.softAP(ssid.c_str());               // tanpa password agar korban mudah connect
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  dnsServer.start(DNS_PORT, "*", apIP);    // semua domain diarahkan ke portal (captive portal)

  server.on("/", handleRoot);
  server.on("/submit", HTTP_POST, handleSubmit);
  server.on("/sos", HTTP_GET, handleSOS);
  server.onNotFound(handleNotFound);
  server.begin();

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init GAGAL! Periksa wiring modul.");
    while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSyncWord(0xF3); // sync word khusus RescueNet, hindari interferensi LoRaWAN publik

  Serial.println("=== RescueNet Field Node " + String(NODE_ID) + " siap ===");
  Serial.println("SSID: " + ssid + "  | Portal: http://192.168.4.1");
  updateOLED("Siap. SSID:" + ssid);
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  checkLoRaReceive();

  // Tombol USER (IO38) T-Beam dipakai sebagai tombol SOS fisik.
  static unsigned long lastSOS = 0;
  if (millis() > 2000 && digitalRead(SOS_BUTTON_PIN) == LOW && millis() - lastSOS > 4000) {
    lastSOS = millis();
    sendReport(false, 0, 0, "KRITIS", 1, true, "SOS-TOMBOL-FISIK");
  }

  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh > 5000) {          // refresh OLED tiap 5 detik walau idle
    lastRefresh = millis();
    updateOLED("Menunggu laporan...");
  }
}
