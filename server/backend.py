#!/usr/bin/env python3
"""
RescueNet - Backend Server
============================
- Subscribe ke broker MQTT lokal (topic: rescuenet/reports)
- Simpan setiap laporan korban ke database SQLite lokal
- Sediakan REST API untuk dashboard monitoring (static/index.html)

Jalankan setelah serial_bridge.py dan Mosquitto broker aktif:
    python3 backend.py

Dashboard dapat diakses di http://<ip-raspberry-pi>:5000/
"""

import json
import sqlite3
import threading
import time

from flask import Flask, jsonify, request, send_from_directory
import paho.mqtt.client as mqtt

DB_PATH = "rescuenet.db"
MQTT_HOST = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "rescuenet/reports"

app = Flask(__name__, static_folder="static", static_url_path="")

db_lock = threading.Lock()


def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS reports (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            pkt_id INTEGER,
            src_id INTEGER,
            hop INTEGER,
            lat REAL,
            lon REAL,
            has_gps INTEGER,
            kondisi TEXT,
            jumlah INTEGER,
            sos INTEGER,
            pesan TEXT,
            rssi INTEGER,
            snr REAL,
            status TEXT DEFAULT 'BARU',
            received_at REAL
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS node_status (
            node_id INTEGER PRIMARY KEY,
            last_seen REAL,
            last_rssi INTEGER
        )
    """)
    conn.commit()
    conn.close()


def save_report(d):
    with db_lock:
        conn = sqlite3.connect(DB_PATH)
        cur = conn.execute("""
            INSERT INTO reports
            (pkt_id, src_id, hop, lat, lon, has_gps, kondisi, jumlah, sos, pesan, rssi, snr, received_at)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
        """, (d["pkt_id"], d["src_id"], d["hop"], d["lat"], d["lon"], int(d["has_gps"]),
              d["kondisi"], d["jumlah"], int(d["sos"]), d["pesan"], d["rssi"], d["snr"], d["received_at"]))
        new_id = cur.lastrowid

        conn.execute("""
            INSERT INTO node_status (node_id, last_seen, last_rssi) VALUES (?,?,?)
            ON CONFLICT(node_id) DO UPDATE SET last_seen=excluded.last_seen, last_rssi=excluded.last_rssi
        """, (d["src_id"], d["received_at"], d["rssi"]))

        conn.commit()
        conn.close()
        return new_id


def on_connect(client, userdata, flags, rc):
    print(f"[backend] Terhubung ke broker MQTT (rc={rc}), subscribe topic '{MQTT_TOPIC}'")
    client.subscribe(MQTT_TOPIC)


def on_message(client, userdata, msg):
    try:
        d = json.loads(msg.payload.decode())
        new_id = save_report(d)
        tag = "SOS!!" if d["sos"] else d["kondisi"]
        print(f"[backend] Laporan #{new_id} disimpan (Node {d['src_id']}, {tag})")
    except Exception as e:
        print(f"[backend] Gagal memproses pesan MQTT: {e}")


def mqtt_worker():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, 60)
            client.loop_forever()
        except Exception as e:
            print(f"[backend] Gagal konek MQTT: {e}, coba lagi 3 detik...")
            time.sleep(3)


# ================== REST API ==================

@app.route("/")
def index():
    return send_from_directory("static", "index.html")


@app.route("/api/reports")
def get_reports():
    """Daftar semua laporan, terbaru di atas. Bisa difilter ?status=BARU"""
    status_filter = request.args.get("status")
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    if status_filter:
        rows = conn.execute(
            "SELECT * FROM reports WHERE status=? ORDER BY received_at DESC", (status_filter,)
        ).fetchall()
    else:
        rows = conn.execute("SELECT * FROM reports ORDER BY received_at DESC").fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])


@app.route("/api/reports/<int:report_id>/status", methods=["PATCH"])
def update_status(report_id):
    """Ubah status tindak lanjut: BARU -> DITANGANI -> SELESAI"""
    new_status = (request.json or {}).get("status")
    if new_status not in ("BARU", "DITANGANI", "SELESAI"):
        return jsonify({"error": "status tidak valid"}), 400
    with db_lock:
        conn = sqlite3.connect(DB_PATH)
        conn.execute("UPDATE reports SET status=? WHERE id=?", (new_status, report_id))
        conn.commit()
        conn.close()
    return jsonify({"ok": True})


@app.route("/api/nodes")
def get_nodes():
    """Status kapan terakhir tiap field node mengirim data (untuk deteksi node mati)."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    rows = conn.execute("SELECT * FROM node_status").fetchall()
    conn.close()
    now = time.time()
    result = []
    for r in rows:
        d = dict(r)
        d["seconds_ago"] = round(now - d["last_seen"], 1)
        d["online"] = d["seconds_ago"] < 300  # anggap offline jika >5 menit tak ada laporan
        result.append(d)
    return jsonify(result)


if __name__ == "__main__":
    init_db()
    t = threading.Thread(target=mqtt_worker, daemon=True)
    t.start()
    print("[backend] RescueNet backend berjalan di http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
