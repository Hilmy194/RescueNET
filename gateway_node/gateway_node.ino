/*
 * RescueNet - GATEWAY NODE FIRMWARE
 * ==================================
 * Board   : LILYGO TTGO LoRa32 (V2.1_1.6) - ESP32 + SX1276 onboard + OLED SSD1306
 * Radio   : LoRa SX1276 (library: sandeepmistry/LoRa)
 * Fungsi  :
 *   1. Mendengarkan semua paket LoRa dari field node (langsung/multi-hop)
 *   2. Melakukan dedup (paket sama tidak diproses dua kali)
 *   3. Meneruskan paket valid ke Raspberry Pi via kabel USB (Serial)
 *   4. Menampilkan status gateway di OLED (jml paket diterima, RSSI terakhir)
 *
 * Gateway ini TIDAK melakukan relay balik ke jaringan LoRa (beda dengan field node),
 * karena tugasnya hanya menyerap data menuju posko.
 *
 * Hubungkan ke Raspberry Pi via kabel USB (data). Baud rate: 115200.
 *
 * !! CEK REVISI BOARD ANDA !! Pin di bawah untuk varian V2.1_1.6. Board lain
 * lihat tabel di docs/INSTRUKSI_IMPLEMENTASI.md bagian LILYGO LoRa32.
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <U8g2lib.h>

// ================== PIN LoRa ONBOARD LILYGO LoRa32 V2.1_1.6 ==================
#define LORA_SS    18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_FREQ  923E6

// ================== PIN OLED ONBOARD ==================
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RST 16
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);

#define LED_PIN 25

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

void updateOLED(String lastEvent) {
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

  u8g2.begin();
  updateOLED("Booting...");

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init GAGAL! Periksa modul/board.");
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
