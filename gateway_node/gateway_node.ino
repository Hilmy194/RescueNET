/*
 * RescueNet - GATEWAY NODE FIRMWARE
 * ==================================
 * Board   : LILYGO T-Beam V1.2 AXP2101 - ESP32 + SX1276 + PMU AXP2101
 * Radio   : LoRa SX1276 (library: sandeepmistry/LoRa)
 * PMU     : AXP2101 (library: XPowersLib by lewisxhe) -- WAJIB, tanpa ini radio
 *           LoRa tidak dapat suplai listrik sama sekali.
 * Fungsi  :
 *   1. Mendengarkan semua paket LoRa dari field node (langsung/multi-hop)
 *   2. Melakukan dedup (paket sama tidak diproses dua kali)
 *   3. Meneruskan paket valid ke Raspberry Pi via kabel USB (Serial)
 *   4. Menampilkan status gateway di OLED (jika ada) & Serial Monitor
 *
 * Gateway ini TIDAK melakukan relay balik ke jaringan LoRa (beda dengan field node),
 * karena tugasnya hanya menyerap data menuju posko.
 *
 * Hubungkan ke Raspberry Pi via kabel USB (data). Baud rate: 115200.
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <XPowersLib.h>            // Install via Library Manager: "XPowersLib" by lewisxhe
#include <U8g2lib.h>

// ================== PIN LoRa ONBOARD T-BEAM V1.2 ==================
#define LORA_SS    18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_FREQ  923E6

// ================== PIN PMU AXP2101 (I2C) ==================
#define PMU_SDA 21
#define PMU_SCL 22
XPowersPMU PMU;
bool pmuOK = false;

// ================== PIN OLED (OPSIONAL, via I2C bus sama) ==================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PMU_SCL, PMU_SDA);
bool oledOK = false;

#define LED_PIN 4

#define CACHE_SIZE 40
uint16_t seenPackets[CACHE_SIZE];
int cacheIndex = 0;
uint32_t rxCount = 0;
int lastRssi = 0;

bool alreadySeen(uint16_t id) {
  for (int i = 0; i < CACHE_SIZE; i++) if (seenPackets[i] == id) return true;
  return false;
}
void markSeen(uint16_t id) {
  seenPackets[cacheIndex] = id;
  cacheIndex = (cacheIndex + 1) % CACHE_SIZE;
}

void initPMU() {
  pmuOK = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);
  if (!pmuOK) {
    Serial.println("PMU AXP2101 GAGAL diinisialisasi! LoRa tidak akan menyala.");
    return;
  }
  PMU.setALDO2(3300); PMU.enableALDO2();   // suplai ke modul LoRa
  Serial.println("PMU AXP2101 siap. Rail LoRa (ALDO2) dinyalakan.");
}

void updateOLED(String lastEvent) {
  if (!oledOK) return;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "RescueNet GATEWAY");
  u8g2.drawStr(0, 24, ("Paket diterima: " + String(rxCount)).c_str());
  u8g2.drawStr(0, 38, ("RSSI terakhir: " + String(lastRssi) + " dBm").c_str());
  u8g2.drawStr(0, 52, lastEvent.c_str());
  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  while (!Serial) { delay(10); }

  Wire.begin(PMU_SDA, PMU_SCL);

  // Deteksi otomatis apakah OLED terpasang (alamat I2C standar SSD1306 = 0x3C)
  Wire.beginTransmission(0x3C);
  oledOK = (Wire.endTransmission() == 0);
  if (oledOK) {
    u8g2.begin();
    updateOLED("Booting...");
    Serial.println("OLED terdeteksi.");
  } else {
    Serial.println("OLED tidak terdeteksi (board tanpa layar) -- status hanya via Serial Monitor.");
  }

  initPMU();

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init GAGAL! Periksa modul/board/PMU.");
    updateOLED("LoRa GAGAL init!");
    while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSyncWord(0xF3); // HARUS SAMA dengan sync word di field_node.ino

  Serial.println("GATEWAY_READY");
  updateOLED("Siap. Menunggu paket...");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String incoming = "";
  while (LoRa.available()) incoming += (char)LoRa.read();

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  lastRssi = rssi;

  int firstComma = incoming.indexOf(',');
  if (firstComma < 0) return; // paket tidak valid

  uint16_t pktId = incoming.substring(0, firstComma).toInt();
  if (alreadySeen(pktId)) return; // sudah pernah diteruskan ke server
  markSeen(pktId);
  rxCount++;

  digitalWrite(LED_PIN, HIGH); delay(60); digitalWrite(LED_PIN, LOW);
  updateOLED("Paket diteruskan ke RPi");

  // Kirim ke Raspberry Pi dalam format: RSSI,SNR,<payload_asli_csv>
  // payload_asli_csv = PKT_ID,SRC_ID,HOP,MAX_HOP,LAT,LON,HAS_GPS,KONDISI,JUMLAH,SOS,PESAN
  Serial.print(rssi);
  Serial.print(",");
  Serial.print(snr, 1);
  Serial.print(",");
  Serial.println(incoming);
}
