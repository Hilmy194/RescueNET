#!/usr/bin/env python3
"""
RescueNet - Serial Bridge
==========================
Membaca data dari Gateway ESP32 (via USB serial) dan mem-publish ke
broker MQTT lokal (Mosquitto) yang berjalan di Raspberry Pi.

Jalankan sebelum backend.py:
    python3 serial_bridge.py

Sesuaikan SERIAL_PORT di bawah. Cek port dengan:
    ls /dev/tty*        (biasanya /dev/ttyUSB0 atau /dev/ttyACM0)
"""

import json
import time
import serial
import paho.mqtt.client as mqtt

# ================== KONFIGURASI ==================
SERIAL_PORT = "/dev/ttyUSB0"   # <-- sesuaikan dengan port gateway ESP32 Anda
BAUD_RATE   = 115200
MQTT_HOST   = "localhost"
MQTT_PORT   = 1883
MQTT_TOPIC  = "rescuenet/reports"

# Urutan field sesuai format yang dikirim gateway_node.ino
FIELDS = ["rssi", "snr", "pkt_id", "src_id", "hop", "max_hop",
          "lat", "lon", "has_gps", "kondisi", "jumlah", "sos", "pesan"]


def parse_line(line: str):
    """Ubah satu baris CSV dari gateway menjadi dict data laporan."""
    parts = line.strip().split(",", len(FIELDS) - 1)  # field terakhir (pesan) boleh apa saja
    if len(parts) < len(FIELDS):
        return None
    d = dict(zip(FIELDS, parts))
    try:
        d["rssi"] = int(d["rssi"])
        d["snr"] = float(d["snr"])
        d["pkt_id"] = int(d["pkt_id"])
        d["src_id"] = int(d["src_id"])
        d["hop"] = int(d["hop"])
        d["max_hop"] = int(d["max_hop"])
        d["lat"] = float(d["lat"])
        d["lon"] = float(d["lon"])
        d["has_gps"] = bool(int(d["has_gps"]))
        d["jumlah"] = int(d["jumlah"])
        d["sos"] = bool(int(d["sos"]))
    except (ValueError, KeyError):
        return None
    d["received_at"] = time.time()
    return d


def connect_serial():
    while True:
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            print(f"[serial_bridge] Terhubung ke {SERIAL_PORT} @ {BAUD_RATE}bps")
            return ser
        except serial.SerialException as e:
            print(f"[serial_bridge] Gagal membuka {SERIAL_PORT}: {e}. Coba lagi 3 detik...")
            time.sleep(3)


def main():
    client = mqtt.Client()
    client.connect(MQTT_HOST, MQTT_PORT, 60)
    client.loop_start()
    print(f"[serial_bridge] Terhubung ke broker MQTT {MQTT_HOST}:{MQTT_PORT}")

    ser = connect_serial()

    while True:
        try:
            raw = ser.readline().decode(errors="ignore")
            if not raw.strip():
                continue

            if raw.strip() == "GATEWAY_READY":
                print("[serial_bridge] Gateway ESP32 terdeteksi & siap.")
                continue

            data = parse_line(raw)
            if data is None:
                print(f"[serial_bridge] Baris tidak valid, diabaikan: {raw.strip()}")
                continue

            client.publish(MQTT_TOPIC, json.dumps(data))
            tag = "SOS!" if data["sos"] else data["kondisi"]
            print(f"[serial_bridge] Laporan Node {data['src_id']} "
                  f"(hop={data['hop']}, RSSI={data['rssi']}) -> {tag}")

        except serial.SerialException as e:
            print(f"[serial_bridge] Koneksi serial terputus: {e}")
            ser.close()
            ser = connect_serial()
        except KeyboardInterrupt:
            print("\n[serial_bridge] Dihentikan oleh pengguna.")
            break

    client.loop_stop()
    ser.close()


if __name__ == "__main__":
    main()
