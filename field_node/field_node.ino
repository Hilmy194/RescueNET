/*
 * ================================================================
 * RescueNet - FIELD NODE FIRMWARE
 * ================================================================
 *
 * Board   : LILYGO T-Beam V1.2
 * MCU     : ESP32
 * PMU     : AXP2101
 * LoRa    : SX1276
 * GPS     : NEO-M8N / compatible
 *
 * Library:
 *   - LoRa by Sandeep Mistry
 *   - XPowersLib by Lewis He
 *   - U8g2 by Oliver
 *
 * Fungsi:
 *   1. WiFi Access Point
 *   2. Captive Portal
 *   3. Form laporan korban
 *   4. SOS portal
 *   5. SOS tombol fisik
 *   6. Pengiriman data melalui LoRa
 *   7. Relay/flooding paket LoRa
 *   8. Deduplication paket
 *   9. Monitoring OLED opsional
 *
 * PENTING:
 * Ganti NODE_ID untuk setiap Field Node.
 * Node 1 = NODE_ID 1
 * Node 2 = NODE_ID 2
 * dst.
 *
 * Gateway = NODE_ID 0
 * ================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include <SPI.h>
#include <LoRa.h>

#include <Wire.h>
#include <XPowersLib.h>

#include <U8g2lib.h>


// ================================================================
// KONFIGURASI NODE
// ================================================================

#define XPOWERS_CHIP_AXP2101

#define NODE_ID        1
#define GATEWAY_ID     0
#define MAX_HOP        5

#define SSID_PREFIX    "RescueNet-Node"


// ================================================================
// KONFIGURASI LORA
// T-Beam V1.2 SX1276
// ================================================================

#define LORA_SCK       5
#define LORA_MISO      19
#define LORA_MOSI      27

#define LORA_SS        18
#define LORA_RST       23
#define LORA_DIO0      26

// RescueNet menggunakan AS923
#define LORA_FREQ      923E6


// ================================================================
// PMU AXP2101
// ================================================================

#define PMU_SDA        21
#define PMU_SCL        22
#define PMU_IRQ        35

XPowersAXP2101 PMU;

bool pmuOK = false;


// ================================================================
// OLED OPSIONAL
// ================================================================

#define OLED_SDA       21
#define OLED_SCL       22

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,
    U8X8_PIN_NONE,
    OLED_SCL,
    OLED_SDA
);

bool oledOK = false;


// ================================================================
// TOMBOL
// ================================================================

// T-Beam V1.2 user button
#define SOS_BUTTON_PIN 38

// LED eksternal / indikator
#define LED_PIN 4


// ================================================================
// COUNTER
// ================================================================

uint32_t txCount = 0;
uint32_t rxCount = 0;


// ================================================================
// WIFI + CAPTIVE PORTAL
// ================================================================

DNSServer dnsServer;
WebServer server(80);

const byte DNS_PORT = 53;

IPAddress apIP(
    192,
    168,
    4,
    1
);


// ================================================================
// DEDUPLICATION CACHE
// ================================================================

#define CACHE_SIZE 25

uint16_t seenPackets[CACHE_SIZE] = {0};

int cacheIndex = 0;

uint16_t packetCounter = 0;


// ================================================================
// BACA TEGANGAN BATERAI
// ================================================================

float readBatteryVoltage()
{
    if (!pmuOK)
        return 0.0;

    return PMU.getBattVoltage() / 1000.0;
}


// ================================================================
// INIT PMU
// ================================================================

void initPMU()
{
    Serial.println();
    Serial.println("[PMU] Inisialisasi AXP2101...");

    pmuOK = PMU.begin(
        Wire,
        AXP2101_SLAVE_ADDRESS,
        PMU_SDA,
        PMU_SCL
    );

    if (!pmuOK)
    {
        Serial.println(
            "[PMU] ERROR: AXP2101 tidak ditemukan!"
        );

        Serial.println(
            "[PMU] LoRa dan GPS kemungkinan tidak mendapat daya."
        );

        return;
    }


    // ------------------------------------------------------------
    // AXP2101 T-Beam V1.2
    //
    // ALDO2 = Radio LoRa
    // ALDO3 = GPS
    // ------------------------------------------------------------


    // RADIO LORA
    PMU.setALDO2Voltage(3300);
    PMU.enableALDO2();


    // GPS
    PMU.setALDO3Voltage(3300);
    PMU.enableALDO3();


    delay(100);


    Serial.println("[PMU] AXP2101 berhasil.");

    Serial.print("[PMU] ALDO2 / LoRa : ");

    if (PMU.isEnableALDO2())
        Serial.println("ON");
    else
        Serial.println("OFF");


    Serial.print("[PMU] ALDO3 / GPS  : ");

    if (PMU.isEnableALDO3())
        Serial.println("ON");
    else
        Serial.println("OFF");


    Serial.print("[PMU] Battery      : ");
    Serial.print(readBatteryVoltage(), 2);
    Serial.println(" V");

    Serial.println();
}


// ================================================================
// OLED
// ================================================================

void updateOLED(String lastEvent)
{
    if (!oledOK)
        return;


    u8g2.clearBuffer();

    u8g2.setFont(
        u8g2_font_6x10_tf
    );


    String line1 =
        "RescueNet Node " +
        String(NODE_ID);

    String line2 =
        "TX:" +
        String(txCount) +
        " RX:" +
        String(rxCount);

    String line3 =
        "Bat:" +
        String(readBatteryVoltage(), 2) +
        "V";


    u8g2.drawStr(
        0,
        10,
        line1.c_str()
    );

    u8g2.drawStr(
        0,
        24,
        line2.c_str()
    );

    u8g2.drawStr(
        0,
        38,
        line3.c_str()
    );

    u8g2.drawStr(
        0,
        52,
        lastEvent.c_str()
    );


    u8g2.sendBuffer();
}


// ================================================================
// CACHE PACKET
// ================================================================

bool alreadySeen(uint16_t id)
{
    for (int i = 0; i < CACHE_SIZE; i++)
    {
        if (seenPackets[i] == id)
            return true;
    }

    return false;
}


void markSeen(uint16_t id)
{
    seenPackets[cacheIndex] = id;

    cacheIndex++;

    if (cacheIndex >= CACHE_SIZE)
        cacheIndex = 0;
}


// ================================================================
// HTML CAPTIVE PORTAL
// ================================================================

const char PORTAL_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta
name="viewport"
content="width=device-width,initial-scale=1">

<title>
RescueNet - Bantuan Darurat
</title>

<style>

body
{
    font-family: sans-serif;
    margin: 0;
    padding: 16px;

    background: #1a1a2e;
    color: #eeeeee;
}

.card
{
    background: #16213e;

    padding: 20px;

    border-radius: 12px;

    max-width: 420px;

    margin: auto;
}

h1
{
    color: #e94560;

    font-size: 20px;

    margin-top: 0;
}

label
{
    display: block;

    margin-top: 12px;

    font-size: 14px;
}

input,
select,
textarea
{
    width: 100%;

    padding: 10px;

    margin-top: 4px;

    border-radius: 6px;

    border: none;

    box-sizing: border-box;

    font-size: 14px;
}

button
{
    width: 100%;

    padding: 14px;

    margin-top: 16px;

    border: none;

    border-radius: 8px;

    font-weight: bold;

    font-size: 16px;
}

.btn-submit
{
    background: #0f9d58;
    color: white;
}

.btn-sos
{
    background: #e94560;
    color: white;

    margin-top: 10px;
}

#gpsStatus
{
    font-size: 12px;

    color: #aaaaaa;

    margin-top: 6px;
}

.info
{
    font-size: 13px;

    color: #aaaaaa;
}

</style>

</head>


<body>


<div class="card">


<h1>
&#128680; RescueNet - Portal Bantuan
</h1>


<p class="info">

Jaringan darurat offline.

Tidak membutuhkan internet atau pulsa.

Isi form untuk mengirim laporan ke posko SAR.

</p>


<form
action="/submit"
method="POST">


<label>
Nama / Kode Pelapor
</label>

<input
name="nama"
placeholder="Nama Anda">


<label>
Kondisi Korban
</label>


<select name="kondisi">

<option value="RINGAN">
Ringan
</option>

<option
value="SEDANG"
selected>
Sedang
</option>

<option value="BERAT">
Berat
</option>

<option value="KRITIS">
Kritis
</option>

</select>


<label>
Jumlah Korban
</label>


<input
name="jumlah"
type="number"
value="1"
min="1">


<label>
Lokasi / Patokan
</label>


<textarea
name="lokasi"
rows="2"
placeholder="Contoh: dekat masjid RW03 atau lantai 2 sekolah">
</textarea>


<label>
Pesan Tambahan
</label>


<textarea
name="pesan"
rows="2"
placeholder="Keterangan tambahan">
</textarea>


<input
type="hidden"
name="lat"
id="lat"
value="0">


<input
type="hidden"
name="lon"
id="lon"
value="0">


<div id="gpsStatus">

Mencoba mengambil lokasi smartphone...

</div>


<button
class="btn-submit"
type="submit">

Kirim Laporan

</button>


</form>


<form
action="/sos"
method="GET">


<button
class="btn-sos"
type="submit">

&#128680; KIRIM SOS DARURAT

</button>


</form>


</div>


<script>

if (navigator.geolocation)
{

    navigator.geolocation.getCurrentPosition(

        function(p)
        {

            document.getElementById('lat').value =
                p.coords.latitude;

            document.getElementById('lon').value =
                p.coords.longitude;


            document.getElementById('gpsStatus').innerText =
                'GPS aktif: ' +
                p.coords.latitude.toFixed(5) +
                ', ' +
                p.coords.longitude.toFixed(5);

        },

        function(e)
        {

            document.getElementById('gpsStatus').innerText =
                'GPS tidak tersedia. Isi lokasi manual.';

        },

        {
            timeout: 5000
        }

    );

}
else
{

    document.getElementById('gpsStatus').innerText =
        'Perangkat tidak mendukung GPS. Isi lokasi manual.';

}

</script>


</body>

</html>

)rawliteral";


// ================================================================
// ROOT
// ================================================================

void handleRoot()
{
    server.send_P(
        200,
        "text/html",
        PORTAL_HTML
    );
}


// ================================================================
// CAPTIVE PORTAL REDIRECT
// ================================================================

void handleNotFound()
{
    server.sendHeader(
        "Location",
        "http://192.168.4.1/",
        true
    );

    server.send(
        302,
        "text/plain",
        ""
    );
}


// ================================================================
// SANITIZE INPUT
// ================================================================

String sanitize(String s, int maxLen)
{
    s.replace(",", ";");

    s.replace("\n", " ");

    s.replace("\r", " ");


    if (s.length() > maxLen)
    {
        s = s.substring(
            0,
            maxLen
        );
    }


    if (s.length() == 0)
    {
        s = "-";
    }


    return s;
}


// ================================================================
// SEND RAW LORA
// ================================================================

bool sendLoRaPacket(const String &payload)
{
    Serial.println(
        "[LORA TX] " +
        payload
    );


    if (!LoRa.beginPacket())
    {
        Serial.println(
            "[LORA] beginPacket gagal."
        );

        return false;
    }


    LoRa.print(payload);


    int result =
        LoRa.endPacket();


    if (result == 0)
    {
        Serial.println(
            "[LORA] Transmisi gagal."
        );

        return false;
    }


    digitalWrite(
        LED_PIN,
        HIGH
    );

    delay(80);

    digitalWrite(
        LED_PIN,
        LOW
    );


    txCount++;


    updateOLED(
        "TX paket"
    );


    return true;
}


// ================================================================
// MEMBUAT REPORT BARU
// ================================================================

void sendReport(
    bool hasGps,
    float lat,
    float lon,
    String kondisi,
    int jumlah,
    bool sos,
    String pesan
)
{

    // ------------------------------------------------------------
    // Packet ID:
    // 4 bit NODE_ID + 12 bit counter
    // ------------------------------------------------------------

    uint16_t pktId =
        ((uint16_t)(NODE_ID & 0x0F) << 12) |
        (packetCounter++ & 0x0FFF);


    markSeen(pktId);


    kondisi = sanitize(
        kondisi,
        10
    );


    pesan = sanitize(
        pesan,
        70
    );


    if (jumlah < 1)
        jumlah = 1;


    // ------------------------------------------------------------
    // Format:
    //
    // PKT_ID
    // SRC_ID
    // HOP
    // MAX_HOP
    // LAT
    // LON
    // HAS_GPS
    // KONDISI
    // JUMLAH
    // SOS
    // PESAN
    // ------------------------------------------------------------


    String payload =
        String(pktId) + "," +

        String(NODE_ID) + "," +

        "0," +

        String(MAX_HOP) + "," +

        String(lat, 6) + "," +

        String(lon, 6) + "," +

        String(hasGps ? 1 : 0) + "," +

        kondisi + "," +

        String(jumlah) + "," +

        String(sos ? 1 : 0) + "," +

        pesan;


    sendLoRaPacket(
        payload
    );
}


// ================================================================
// HANDLE SUBMIT FORM
// ================================================================

void handleSubmit()
{

    String nama =
        server.hasArg("nama")
            ? sanitize(server.arg("nama"), 20)
            : "-";


    String latStr =
        server.hasArg("lat")
            ? server.arg("lat")
            : "0";


    String lonStr =
        server.hasArg("lon")
            ? server.arg("lon")
            : "0";


    float lat =
        latStr.toFloat();

    float lon =
        lonStr.toFloat();


    bool hasGps =
        (lat != 0.0 || lon != 0.0);


    String lokasi =
        server.hasArg("lokasi")
            ? sanitize(server.arg("lokasi"), 40)
            : "-";


    String kondisi =
        server.hasArg("kondisi")
            ? sanitize(server.arg("kondisi"), 10)
            : "SEDANG";


    int jumlah =
        server.hasArg("jumlah")
            ? server.arg("jumlah").toInt()
            : 1;


    if (jumlah < 1)
        jumlah = 1;


    String pesanRaw =
        server.hasArg("pesan")
            ? server.arg("pesan")
            : "-";


    String pesan =
        sanitize(
            nama +
            "|" +
            lokasi +
            "|" +
            pesanRaw,

            70
        );


    sendReport(
        hasGps,
        lat,
        lon,
        kondisi,
        jumlah,
        false,
        pesan
    );


    server.send(
        200,
        "text/html",

        "<html>"
        "<body style='"
        "font-family:sans-serif;"
        "background:#1a1a2e;"
        "color:#eee;"
        "text-align:center;"
        "padding-top:60px'>"

        "<h2 style='color:#0f9d58'>"
        "&#10004; Laporan terkirim"
        "</h2>"

        "<p>"
        "Laporan sudah dikirim melalui jaringan RescueNet."
        "</p>"

        "<a href='/' style='color:#e94560'>"
        "Kembali ke Portal"
        "</a>"

        "</body>"
        "</html>"
    );
}


// ================================================================
// SOS PORTAL
// ================================================================

void handleSOS()
{

    sendReport(
        false,
        0,
        0,
        "KRITIS",
        1,
        true,
        "SOS-TOMBOL-PORTAL"
    );


    server.send(
        200,
        "text/html",

        "<html>"
        "<body style='"
        "font-family:sans-serif;"
        "background:#1a1a2e;"
        "color:#eee;"
        "text-align:center;"
        "padding-top:60px'>"

        "<h2 style='color:#e94560'>"
        "&#128680; SOS terkirim!"
        "</h2>"

        "<p>"
        "Sinyal darurat telah dikirim ke jaringan RescueNet."
        "</p>"

        "<a href='/' style='color:#eee'>"
        "Kembali"
        "</a>"

        "</body>"
        "</html>"
    );
}


// ================================================================
// SPLIT CSV
// ================================================================

bool splitCSV(
    const String &s,
    String out[],
    int maxFields
)
{
    int start = 0;

    int fieldIndex = 0;


    for (
        int i = 0;
        i <= (int)s.length();
        i++
    )
    {

        if (
            i == (int)s.length() ||
            s[i] == ','
        )
        {

            if (
                fieldIndex >= maxFields
            )
            {
                return false;
            }


            // Field terakhir menerima semua sisa
            if (
                fieldIndex ==
                maxFields - 1
            )
            {

                out[fieldIndex] =
                    s.substring(start);

                fieldIndex++;

                break;
            }


            out[fieldIndex] =
                s.substring(
                    start,
                    i
                );


            fieldIndex++;


            start =
                i + 1;
        }
    }


    return
        fieldIndex == maxFields;
}


// ================================================================
// RECEIVE + RELAY
// ================================================================

void checkLoRaReceive()
{

    int packetSize =
        LoRa.parsePacket();


    if (packetSize <= 0)
        return;


    String incoming;


    while (LoRa.available())
    {
        incoming +=
            (char)LoRa.read();
    }


    // Hindari paket abnormal
    if (incoming.length() == 0)
        return;


    const int NFIELD = 11;


    String f[NFIELD];


    if (
        !splitCSV(
            incoming,
            f,
            NFIELD
        )
    )
    {

        Serial.println(
            "[DROP] Format paket tidak valid."
        );

        return;
    }


    uint16_t pktId =
        (uint16_t)f[0].toInt();


    int srcId =
        f[1].toInt();


    int hop =
        f[2].toInt();


    int maxHop =
        f[3].toInt();


    // ------------------------------------------------------------
    // Validasi
    // ------------------------------------------------------------

    if (pktId == 0)
    {
        Serial.println(
            "[DROP] Packet ID tidak valid."
        );

        return;
    }


    if (
        maxHop < 0 ||
        maxHop > 20
    )
    {
        Serial.println(
            "[DROP] MAX_HOP tidak valid."
        );

        return;
    }


    // ------------------------------------------------------------
    // Dedup
    // ------------------------------------------------------------

    if (alreadySeen(pktId))
    {
        Serial.println(
            "[DROP] Duplicate packet."
        );

        return;
    }


    markSeen(pktId);


    rxCount++;


    int rssi =
        LoRa.packetRssi();


    float snr =
        LoRa.packetSnr();


    Serial.println();

    Serial.println(
        "[RX] " +
        incoming
    );


    Serial.print(
        "[RX] Source: "
    );

    Serial.println(srcId);


    Serial.print(
        "[RX] RSSI: "
    );

    Serial.print(rssi);

    Serial.println(" dBm");


    Serial.print(
        "[RX] SNR : "
    );

    Serial.print(snr);

    Serial.println(" dB");


    updateOLED(
        "RX Node " +
        String(srcId)
    );


    // ------------------------------------------------------------
    // TTL
    // ------------------------------------------------------------

    if (
        hop + 1 > maxHop
    )
    {

        Serial.println(
            "[DROP] TTL habis."
        );

        return;
    }


    // ------------------------------------------------------------
    // Random backoff
    // supaya relay beberapa node tidak transmit bersamaan
    // ------------------------------------------------------------

    delay(
        random(
            80,
            400
        )
    );


    f[2] =
        String(
            hop + 1
        );


    String forwarded =

        f[0] + "," +

        f[1] + "," +

        f[2] + "," +

        f[3] + "," +

        f[4] + "," +

        f[5] + "," +

        f[6] + "," +

        f[7] + "," +

        f[8] + "," +

        f[9] + "," +

        f[10];


    Serial.println(
        "[RELAY] " +
        forwarded
    );


    sendLoRaPacket(
        forwarded
    );
}


// ================================================================
// INIT OLED
// ================================================================

void initOLED()
{

    Wire.beginTransmission(
        0x3C
    );


    oledOK =
        (Wire.endTransmission() == 0);


    if (!oledOK)
    {

        Serial.println(
            "[OLED] OLED tidak terdeteksi."
        );

        return;
    }


    u8g2.begin();


    u8g2.clearBuffer();


    u8g2.setFont(
        u8g2_font_6x10_tf
    );


    u8g2.drawStr(
        0,
        20,
        "RescueNet"
    );


    u8g2.drawStr(
        0,
        34,
        "Booting..."
    );


    u8g2.sendBuffer();


    Serial.println(
        "[OLED] SSD1306 terdeteksi."
    );
}


// ================================================================
// INIT WIFI
// ================================================================

void initWiFi()
{

    String ssid =
        String(SSID_PREFIX) +
        String(NODE_ID);


    WiFi.mode(
        WIFI_AP
    );


    // Konfigurasi IP dilakukan sebelum AP aktif
    WiFi.softAPConfig(
        apIP,
        apIP,
        IPAddress(
            255,
            255,
            255,
            0
        )
    );


    // Open WiFi:
    // korban tidak perlu password
    bool apOK =
        WiFi.softAP(
            ssid.c_str()
        );


    if (!apOK)
    {

        Serial.println(
            "[WIFI] Gagal membuat Access Point."
        );

        return;
    }


    Serial.print(
        "[WIFI] SSID : "
    );

    Serial.println(ssid);


    Serial.print(
        "[WIFI] IP   : "
    );

    Serial.println(
        WiFi.softAPIP()
    );


    // Semua DNS diarahkan ke captive portal
    dnsServer.start(
        DNS_PORT,
        "*",
        apIP
    );


    server.on(
        "/",
        HTTP_GET,
        handleRoot
    );


    server.on(
        "/submit",
        HTTP_POST,
        handleSubmit
    );


    server.on(
        "/sos",
        HTTP_GET,
        handleSOS
    );


    server.onNotFound(
        handleNotFound
    );


    server.begin();


    Serial.println(
        "[WEB] Captive Portal aktif."
    );
}


// ================================================================
// INIT LORA
// ================================================================

bool initLoRa()
{

    Serial.println();
    Serial.println(
        "[LORA] Inisialisasi SX1276..."
    );


    // ------------------------------------------------------------
    // PENTING:
    // T-Beam tidak memakai default VSPI ESP32.
    //
    // T-Beam:
    // SCK  = 5
    // MISO = 19
    // MOSI = 27
    // SS   = 18
    // ------------------------------------------------------------

    SPI.begin(
        LORA_SCK,
        LORA_MISO,
        LORA_MOSI,
        LORA_SS
    );


    LoRa.setSPI(
        SPI
    );


    LoRa.setPins(
        LORA_SS,
        LORA_RST,
        LORA_DIO0
    );


    if (
        !LoRa.begin(
            LORA_FREQ
        )
    )
    {

        Serial.println(
            "[LORA] ERROR: SX1276 tidak ditemukan."
        );

        Serial.println(
            "[LORA] Periksa:"
        );

        Serial.println(
            "       - tipe radio"
        );

        Serial.println(
            "       - PMU ALDO2"
        );

        Serial.println(
            "       - pin SPI"
        );

        Serial.println(
            "       - antena"
        );


        return false;
    }


    // ------------------------------------------------------------
    // Parameter RescueNet
    // ------------------------------------------------------------

    LoRa.setSpreadingFactor(
        9
    );


    LoRa.setSignalBandwidth(
        125E3
    );


    LoRa.setCodingRate4(
        5
    );


    LoRa.setSyncWord(
        0xF3
    );


    LoRa.enableCrc();


    Serial.println(
        "[LORA] SX1276 siap."
    );


    Serial.print(
        "[LORA] Frequency : "
    );

    Serial.print(
        LORA_FREQ / 1000000
    );

    Serial.println(
        " MHz"
    );


    Serial.println(
        "[LORA] SF        : 9"
    );


    Serial.println(
        "[LORA] BW        : 125 kHz"
    );


    Serial.println(
        "[LORA] CR        : 4/5"
    );


    Serial.println(
        "[LORA] Sync Word : 0xF3"
    );


    Serial.println(
        "[LORA] CRC       : ON"
    );


    return true;
}


// ================================================================
// SETUP
// ================================================================

void setup()
{

    Serial.begin(
        115200
    );


    delay(1000);


    Serial.println();
    Serial.println(
        "==================================="
    );

    Serial.println(
        "        RescueNet FIELD NODE       "
    );

    Serial.println(
        "==================================="
    );


    Serial.print(
        "NODE ID : "
    );

    Serial.println(
        NODE_ID
    );


    // ------------------------------------------------------------
    // GPIO
    // ------------------------------------------------------------

    pinMode(
        LED_PIN,
        OUTPUT
    );


    digitalWrite(
        LED_PIN,
        LOW
    );


    /*
     * GPIO38 merupakan input-only.
     * Jangan gunakan INPUT_PULLUP.
     *
     * Tombol T-Beam sudah memiliki rangkaian board.
     */
    pinMode(
        SOS_BUTTON_PIN,
        INPUT
    );


    // ------------------------------------------------------------
    // Random seed
    // ------------------------------------------------------------

    randomSeed(
        micros() +
        NODE_ID
    );


    // ------------------------------------------------------------
    // I2C
    // ------------------------------------------------------------

    Wire.begin(
        PMU_SDA,
        PMU_SCL
    );


    delay(50);


    // ------------------------------------------------------------
    // OLED
    // ------------------------------------------------------------

    initOLED();


    // ------------------------------------------------------------
    // PMU
    // ------------------------------------------------------------

    initPMU();


    if (!pmuOK)
    {

        Serial.println(
            "[WARNING] PMU gagal."
        );

        Serial.println(
            "[WARNING] LoRa kemungkinan tidak dapat bekerja."
        );
    }


    delay(100);


    // ------------------------------------------------------------
    // LoRa
    // ------------------------------------------------------------

    bool loraOK =
        initLoRa();


    if (!loraOK)
    {

        updateOLED(
            "LoRa ERROR"
        );


        while (true)
        {

            digitalWrite(
                LED_PIN,
                HIGH
            );

            delay(200);


            digitalWrite(
                LED_PIN,
                LOW
            );

            delay(200);
        }
    }


    // ------------------------------------------------------------
    // WiFi
    // ------------------------------------------------------------

    initWiFi();


    String ssid =
        String(SSID_PREFIX) +
        String(NODE_ID);


    Serial.println();
    Serial.println(
        "==================================="
    );

    Serial.println(
        "      RescueNet Node SIAP          "
    );

    Serial.println(
        "==================================="
    );


    Serial.print(
        "SSID   : "
    );

    Serial.println(
        ssid
    );


    Serial.println(
        "Portal : http://192.168.4.1"
    );


    Serial.print(
        "LoRa   : "
    );

    Serial.print(
        LORA_FREQ / 1000000
    );

    Serial.println(
        " MHz"
    );


    Serial.println(
        "==================================="
    );


    updateOLED(
        "Siap"
    );
}


// ================================================================
// LOOP
// ================================================================

void loop()
{

    // ------------------------------------------------------------
    // Captive portal
    // ------------------------------------------------------------

    dnsServer.processNextRequest();

    server.handleClient();


    // ------------------------------------------------------------
    // LoRa Receiver
    // ------------------------------------------------------------

    checkLoRaReceive();


    // ------------------------------------------------------------
    // SOS BUTTON
    // ------------------------------------------------------------

    static unsigned long lastSOS =
        0;


    const unsigned long SOS_COOLDOWN =
        4000;


    if (
        millis() > 2000 &&
        digitalRead(SOS_BUTTON_PIN) == LOW &&
        millis() - lastSOS > SOS_COOLDOWN
    )
    {

        lastSOS =
            millis();


        Serial.println(
            "[SOS] Tombol SOS fisik ditekan!"
        );


        updateOLED(
            "SOS!"
        );


        sendReport(
            false,
            0,
            0,
            "KRITIS",
            1,
            true,
            "SOS-TOMBOL-FISIK"
        );
    }


    // ------------------------------------------------------------
    // OLED REFRESH
    // ------------------------------------------------------------

    static unsigned long lastRefresh =
        0;


    if (
        millis() - lastRefresh >
        5000
    )
    {

        lastRefresh =
            millis();


        updateOLED(
            "Menunggu..."
        );
    }
}