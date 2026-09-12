# RescueNet — Instruksi Implementasi Lengkap

Dokumen ini adalah panduan teknis untuk merealisasikan sistem RescueNet sesuai
arsitektur pada Proposal Despro 1 (Gambar 3.1, Bab 3.4). Ditujukan sebagai dasar
kerja tim untuk tahap **Desain Proyek 2**.

---

## 1. Ringkasan Arsitektur

```
[Korban/Petugas] --WiFi--> [Field Node ESP32+LoRa] --LoRa multi-hop--> [Gateway ESP32+LoRa]
                                                                              |
                                                                         USB Serial
                                                                              |
                                                                    [Raspberry Pi]
                                                                    - serial_bridge.py
                                                                    - Mosquitto (MQTT broker)
                                                                    - backend.py (Flask+SQLite)
                                                                    - Dashboard web (static/)
                                                                              |
                                                                     [Laptop/HP Operator Posko]
```

Alur data: **Portal Web (di HP korban) → ESP32 Field Node → LoRa (mesh, multi-hop
jika perlu) → ESP32 Gateway → USB Serial → Raspberry Pi (MQTT → SQLite) → Dashboard**.

---

## 2. Daftar Komponen & Wiring

Proyek ini menggunakan board **LILYGO TTGO T-Beam V1.2 (AXP2101)**, yaitu modul
ESP32 dengan chip LoRa SX1276, GPS NEO-M8N, dan chip manajemen daya (PMU)
AXP2101 terintegrasi. Berbeda dari LoRa32 biasa, board ini **wajib** menginisialisasi
PMU lewat kode sebelum radio LoRa bisa menyala — tanpa itu, `LoRa.begin()` akan
selalu gagal walau wiring/pin sudah benar.

### 2.1 Field Node (per unit)
| Komponen | Jumlah | Catatan |
|---|---|---|
| LILYGO T-Beam V1.2 AXP2101 (varian 868/915 MHz) | 1 | ESP32 + SX1276 + GPS + PMU jadi satu board |
| Antena LoRa (SMA/IPEX, sesuai varian board) | 1 | **Wajib dipasang sebelum menyalakan radio** — transmit tanpa antena bisa merusak chip RF |
| Antena GPS (biasanya sudah include, konektor terpisah dari antena LoRa) | 1 | Untuk modul GPS NEO-M8N onboard |
| Baterai Li-Po (JST 1.25mm) | 1 | Board punya charging circuit onboard (via chip AXP2101); target daya tahan ≥8 jam |
| Casing (3D print/project box) | 1 | Sediakan lubang untuk tombol USER/IO38 dan port USB-C |

### 2.2 Verifikasi Frekuensi Board
T-Beam dijual dalam beberapa varian frekuensi radio (433 / 868 / 915 MHz)
tergantung pemasangan matching circuit RF di pabrik. **Pastikan Anda membeli
varian 868 MHz atau 915 MHz** — chip SX1276 di varian ini secara fisik masih
bisa di-tune ke 923 MHz (rentang kerja SX1276 adalah 862–1020 MHz), sesuai
regulasi SDPPI Indonesia (AS923). **Varian 433 MHz tidak bisa dipakai** karena
matching circuit RF-nya berbeda secara fisik.

### 2.3 Tombol Fisik T-Beam (bukan seperti board ESP32 biasa)
T-Beam punya 3 tombol dengan fungsi berbeda dari board ESP32 pada umumnya:

| Tombol | Terhubung ke | Fungsi | Dipakai di firmware untuk |
|---|---|---|---|
| **RST** | EN/CHIP_PU ESP32 | Reset keras chip | (tidak dipakai eksplisit) |
| **USER / IO38** | GPIO38 langsung | Satu-satunya tombol ke GPIO biasa | **Tombol SOS** |
| **PWR** | Chip AXP2101 | Nyala/mati board dari baterai | (tidak dipakai di kode) |

Tidak ada tombol yang terhubung ke GPIO0 (BOOT), jadi proses **upload firmware
mengandalkan auto-reset otomatis** dari chip USB-serial — biasanya tidak perlu
menahan tombol apa pun saat upload.

### 2.4 Pin Onboard T-Beam V1.2 AXP2101 (sudah diatur di firmware)
| Fungsi | Pin |
|---|---|
| LoRa SCK/MISO/MOSI | 5 / 19 / 27 (default HSPI, otomatis) |
| LoRa NSS/CS | GPIO 18 |
| LoRa RESET | GPIO 23 |
| LoRa DIO0 | GPIO 26 |
| PMU AXP2101 SDA / SCL | GPIO 21 / 22 |
| PMU IRQ | GPIO 35 (tidak dipakai eksplisit di firmware ini) |
| Tombol USER (SOS) | GPIO 38 |
| OLED (jika ada, opsional) | Bus I2C sama dengan PMU (21/22), dideteksi otomatis saat boot |

### 2.5 Gateway Node
Board fisik sama persis dengan field node (LILYGO T-Beam), tapi firmware
berbeda — **tanpa** WiFi captive portal dan GPS, hanya mendengarkan LoRa dan
meneruskan ke Raspberry Pi. Terhubung ke Raspberry Pi via kabel USB (data,
bukan hanya kabel charging — banyak kabel murah cuma bawa jalur power).

### 2.6 Server (Raspberry Pi)
- Raspberry Pi 4 (atau laptop sebagai pengganti untuk skenario tanpa Raspberry Pi)
- microSD 32GB (OS: Raspberry Pi OS Lite/Desktop)
- Power bank/adaptor 5V 3A

---

## 3. Setup Firmware (Arduino IDE)

1. Install **Arduino IDE** + board package **ESP32** (Boards Manager → cari "esp32" oleh Espressif).
2. Pilih board di menu Tools → Board → ESP32 Arduino: **"T-Beam"** jika muncul di daftar,
   atau **"ESP32 Dev Module"** sebagai fallback (pin tetap diatur manual di kode jadi tetap berfungsi).
3. Install 3 library via Library Manager:
   - **LoRa** by Sandeep Mistry
   - **XPowersLib** by lewisxhe (**wajib** — untuk inisialisasi PMU AXP2101, tanpa ini LoRa tidak menyala)
   - **U8g2** by oliver (untuk OLED, opsional — kode akan otomatis mendeteksi ada/tidaknya OLED)
4. Buka `field_node/field_node.ino`.
   - **WAJIB**: ubah `#define NODE_ID` menjadi angka unik untuk setiap unit
     (1, 2, 3, dst — jangan ada yang sama).
   - Pasang antena LoRa sebelum menyalakan board (jangan transmit tanpa antena).
   - Upload ke tiap board field node satu per satu.
5. Buka `gateway_node/gateway_node.ino`, upload ke board yang akan jadi gateway
   (tidak perlu ubah NODE_ID, gateway selalu ID 0).
6. Verifikasi via Serial Monitor (115200 baud) — harus muncul urutan log:
   ```
   OLED terdeteksi.                                  (atau "tidak terdeteksi" jika tanpa layar, aman)
   PMU AXP2101 siap. Rail LoRa (ALDO2) & GPS (ALDO3) dinyalakan.
   === RescueNet Field Node X siap ===
   SSID: RescueNet-NodeX | Portal: http://192.168.4.1
   ```
   Kalau muncul "PMU AXP2101 GAGAL diinisialisasi!" → cek kembali board (kemungkinan
   board Anda bukan varian AXP2101, atau ada masalah di jalur I2C).
   Kalau muncul "LoRa init GAGAL!" setelah PMU sukses → kemungkinan antena belum
   terpasang atau modul LoRa fisik bermasalah.

**Penting — Sync Word LoRa**: semua node (field + gateway) harus memakai
`LoRa.setSyncWord(0xF3)` yang sama, jangan diubah berbeda antar unit, atau
mereka tidak akan bisa saling mendengar.

---

## 4. Setup Raspberry Pi (Server)

```bash
# 1. Update sistem
sudo apt update && sudo apt upgrade -y

# 2. Install Mosquitto (MQTT broker lokal)
sudo apt install -y mosquitto mosquitto-clients

# 3. Salin konfigurasi RescueNet
sudo cp server/mosquitto_rescuenet.conf /etc/mosquitto/conf.d/rescuenet.conf
sudo systemctl restart mosquitto
sudo systemctl enable mosquitto   # otomatis start saat boot

# 4. Install Python & dependencies
sudo apt install -y python3-pip
cd rescuenet/server
pip3 install -r requirements.txt

# 5. Cari port USB gateway ESP32
ls /dev/tty*
# biasanya /dev/ttyUSB0 (chip CP2102/CH340) — sesuaikan SERIAL_PORT di serial_bridge.py

# 6. Jalankan (di 2 terminal terpisah, atau pakai systemd/screen/tmux)
python3 serial_bridge.py     # terminal 1
python3 backend.py            # terminal 2
```

Dashboard bisa diakses di `http://<ip-raspberry-pi>:5000` dari perangkat mana
pun yang tersambung ke jaringan lokal Raspberry Pi (WiFi hotspot RPi atau
kabel LAN ke gateway).

### 4.1 Menjalankan otomatis saat boot (opsional, direkomendasikan untuk lapangan)
Buat dua file systemd service:

```ini
# /etc/systemd/system/rescuenet-serial.service
[Unit]
Description=RescueNet Serial Bridge
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/pi/rescuenet/server/serial_bridge.py
WorkingDirectory=/home/pi/rescuenet/server
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/rescuenet-backend.service
[Unit]
Description=RescueNet Backend
After=network.target rescuenet-serial.service

[Service]
ExecStart=/usr/bin/python3 /home/pi/rescuenet/server/backend.py
WorkingDirectory=/home/pi/rescuenet/server
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now rescuenet-serial rescuenet-backend
```

### 4.2 Catatan: Penyesuaian dari Batasan Teknis di Proposal
Beralih ke LILYGO T-Beam AXP2101 mengubah beberapa asumsi teknis di Bab 2.4 proposal:
- **Antena**: proposal menyebut "antena harus terpasang permanen" dengan
  perhitungan panjang kawat 7,72 cm (antena monopole custom). Pada T-Beam,
  antena terpasang via konektor **SMA atau u.FL/IPEX**, bukan kawat solder
  langsung. Tetap penting antena terpasang kencang (pakai lem panas di
  sambungan konektor) agar tidak lepas saat dibawa di lapangan — tapi bukan
  lagi solder permanen.
- **Manajemen daya**: T-Beam punya chip PMU (AXP2101) yang mengatur suplai
  listrik ke tiap modul (LoRa, GPS) secara terpisah — ini **wajib diinisialisasi
  lewat kode** (lihat `initPMU()` di firmware), berbeda dari board ESP32+LoRa
  polos yang listriknya langsung tersambung tanpa switching.
- **GPS built-in**: T-Beam sudah punya modul GPS NEO-M8N onboard dan firmware
  field node sekarang memakai GPS node sebagai sumber koordinat laporan.
  Ini lebih cocok untuk mode offline/captive portal karena browser HP sering
  memblokir geolocation pada halaman `http://192.168.4.1` yang tidak HTTPS.
  Lokasi yang tampil di dashboard adalah posisi field node tempat korban
  tersambung/mengirim laporan.
- **RAB**: harga per-unit T-Beam (ESP32+LoRa+GPS+PMU jadi satu) berbeda dari
  ESP32 DevKit + modul LoRa RA-02 terpisah. Perbarui Tabel 5.2 di proposal
  dengan harga T-Beam AXP2101 aktual dari supplier Anda.
- **Casing**: pastikan desain 3D print/box menyediakan lubang untuk tombol
  USER/IO38 (dipakai sebagai SOS di firmware ini), tombol PWR, dan port USB-C.

---

## 5. Protokol Data (Format Paket LoRa)

Format CSV ringkas (menghindari overhead JSON, sesuai batasan payload LoRa 255 byte,
lihat Bab 3.4.3 proposal):

```
PKT_ID,SRC_ID,HOP,MAX_HOP,LAT,LON,HAS_GPS,KONDISI,JUMLAH,SOS,PESAN
```

| Field | Arti |
|---|---|
| PKT_ID | ID unik paket (untuk dedup saat relay) |
| SRC_ID | ID node asal pelapor |
| HOP | Jumlah hop yang sudah dilalui (bertambah tiap relay) |
| MAX_HOP | TTL maksimum (default 5) — mencegah looping paket selamanya |
| LAT, LON | Koordinat GPS field node/T-Beam (0,0 jika belum fix) |
| HAS_GPS | 1 jika GPS node sudah fix, 0 jika belum tersedia |
| KONDISI | RINGAN / SEDANG / BERAT / KRITIS |
| JUMLAH | Jumlah korban dalam laporan tersebut |
| SOS | 1 jika dikirim lewat tombol SOS |
| PESAN | Nama pelapor + lokasi manual + catatan (digabung, maks ±70 karakter) |

Gateway menambahkan `RSSI,SNR,` di depan sebelum diteruskan ke Raspberry Pi via serial.

### Mekanisme Mesh (Flooding dengan Dedup + TTL)
1. Field node membuat paket baru → broadcast via LoRa.
2. Setiap node lain yang mendengar paket akan:
   - Mengecek apakah `PKT_ID` sudah pernah dilihat (cache 25 entri terakhir) → jika ya, buang.
   - Mengecek `HOP+1 > MAX_HOP` → jika ya, buang (TTL habis).
   - Jika lolos: tunggu jeda acak 50–350 ms (menghindari tabrakan sesama relay),
     lalu naikkan HOP dan broadcast ulang.
3. Gateway mendengarkan semua broadcast; jika `PKT_ID` belum pernah diteruskan
   ke server, kirim ke Raspberry Pi via serial.

Ini adalah pendekatan **flooding mesh** — sederhana untuk diimplementasikan
dan diuji dalam skala prototipe (2–5 node), sesuai batasan proyek. Untuk skala
lebih besar di masa depan, bisa dikembangkan ke algoritma routing yang lebih
efisien (mis. berbasis metrik RSSI seperti pendekatan OLSR/ELP pada studi WiMesh
di proposal Bab 2.1).

---

## 6. Prosedur Pengujian (sesuai Bab 5.2.2 Proposal)

### 6.1 Pengujian Komunikasi Antar Node
- Nyalakan 1 field node + 1 gateway berdekatan (±5 m). Kirim laporan dari
  portal → cek muncul di dashboard.

### 6.2 Pengujian Multi-Hop
- Tempatkan field node A di luar jangkauan langsung gateway, tapi field node B
  (relay) berada di antara keduanya dan dalam jangkauan LoRa dari A dan gateway.
- Kirim laporan dari A → verifikasi paket diteruskan B → sampai ke gateway
  (lihat log Serial Monitor field node B: harus muncul baris `[RELAY]`).

### 6.3 Pengukuran QoS (PDR, RSSI, delay)
- Kirim N paket uji dari satu node (mis. 100x), hitung:
  `PDR = (paket diterima gateway / paket dikirim) × 100%`
- Ambil nilai RSSI dari kolom RSSI di dashboard/database untuk tiap jarak uji.
- Ukur delay dari `received_at` (server) dikurangi waktu pengiriman (dicatat manual
  atau via timestamp Serial Monitor saat `[TX]`).
- Target proposal: PDR ≥ 80% pada jarak 500 m (semi-terbuka), RSSI ≥ -120 dBm,
  delay ≤ 5 detik per paket.

### 6.4 Pengujian Ketahanan Sistem
- Matikan salah satu relay node saat pengiriman sedang berlangsung → pastikan
  jaringan tetap bisa mengirim data lewat jalur lain (jika topologi mendukung)
  atau paket gagal terkirim tercatat dengan jelas (untuk dianalisis).

### 6.5 Pengujian Dashboard
- Verifikasi update laporan baru muncul di tabel dalam <10 detik (polling 4 detik
  di `app.js` sudah memenuhi target ini).
- Verifikasi status laporan bisa diubah (BARU → DITANGANI → SELESAI) dan tersimpan.

---

## 7. Struktur File Proyek

```
rescuenet/
├── field_node/
│   └── field_node.ino          # Firmware ESP32 field node (portal + LoRa TX/relay)
├── gateway_node/
│   └── gateway_node.ino        # Firmware ESP32 gateway (LoRa RX -> Serial)
├── server/
│   ├── serial_bridge.py        # Baca serial gateway -> publish MQTT
│   ├── backend.py              # Flask + SQLite + REST API
│   ├── requirements.txt
│   ├── mosquitto_rescuenet.conf
│   └── static/
│       ├── index.html          # Dashboard monitoring
│       ├── style.css
│       └── app.js
└── docs/
    └── INSTRUKSI_IMPLEMENTASI.md   # Dokumen ini
```

---

## 8. Pemetaan ke Kebutuhan Proposal

| Kebutuhan Fungsional (Bab 3.2.1) | Implementasi |
|---|---|
| Portal bantuan lokal via smartphone | `field_node.ino` — WiFi AP + captive portal + form HTML |
| Input data korban (nama, lokasi, kondisi, jumlah, pesan) | Form `/submit` di field node |
| Komunikasi multi-hop antar field node | Fungsi `checkLoRaReceive()` (flooding + dedup + TTL) |
| Gateway terima & teruskan ke server | `gateway_node.ino` → `serial_bridge.py` |
| Server simpan ke database lokal | `backend.py` → SQLite (`rescuenet.db`) |
| Dashboard tampilkan laporan real-time | `static/index.html` + `app.js` (polling 4 detik) |
| Tombol SOS | Endpoint `/sos` + tombol fisik `SOS_BUTTON_PIN` |
| Sistem jalan tanpa internet publik | Semua komunikasi lokal: WiFi AP, LoRa, Mosquitto lokal, Flask lokal |

---

## 9. Rencana Lanjutan untuk Desain Proyek 2

1. **Kalibrasi lapangan**: uji jangkauan LoRa nyata di area terbuka & semi-terbuka,
   sesuaikan `setSpreadingFactor`/`setSignalBandwidth` untuk trade-off jangkauan vs kecepatan.
2. **Manajemen daya**: ukur konsumsi arus tiap mode (idle, TX, RX) untuk validasi target 8 jam.
3. **Keamanan dasar**: aktifkan autentikasi dashboard (login admin sederhana) dan
   autentikasi MQTT (lihat catatan di `mosquitto_rescuenet.conf`).
4. **Visualisasi peta**: jika tersedia waktu, integrasikan tile peta offline
   (mis. MBTiles + tileserver lokal) menggantikan scatter plot SVG sederhana.
5. **Reliabilitas pengiriman**: pertimbangkan mekanisme ACK sederhana dari gateway
   agar field node tahu laporan sudah sampai (retry otomatis jika tidak ada ACK).
