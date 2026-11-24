"""Laundry Machine Monitor - collector + dashboard (CS147 Group 16).

Subscribes to the device's MQTT topics on EMQX, checks every signature,
stores readings/heartbeats/events in SQLite, and serves a dashboard with
live state and 1 h / 24 h / 72 h history, gaps, and reliability stats.

Config comes from environment variables (see .env.example). Run:
    pip install -r requirements.txt
    python app.py            -> open http://localhost:8000
"""
import hashlib
import hmac
import json
import os
import sqlite3
import threading
import time

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, send_from_directory

MQTT_HOST = os.environ.get("MQTT_HOST", "")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_USER = os.environ.get("MQTT_USER", "")
MQTT_PASS = os.environ.get("MQTT_PASS", "")
HMAC_KEY = os.environ.get("HMAC_KEY", "").encode()
DB_PATH = os.environ.get("DB_PATH", "laundry.db")
WEB_PORT = int(os.environ.get("PORT", "8000"))

TOPIC_PREFIX = "iot/T16/"
READING_PERIOD_S = 2        # device sends one reading every 2 s
GAP_S = 10                  # a hole longer than this counts as a gap
OFFLINE_AFTER_S = 90        # no message for this long => dashboard shows offline

# ---------------- Database ----------------
db = sqlite3.connect(DB_PATH, check_same_thread=False)
db_lock = threading.Lock()
with db_lock:
    db.executescript("""
    CREATE TABLE IF NOT EXISTS readings (
        boot INTEGER, seq INTEGER, ts INTEGER, recv_ts REAL,
        vib REAL, state TEXT, bf INTEGER,
        PRIMARY KEY (boot, seq));
    CREATE INDEX IF NOT EXISTS readings_ts ON readings(ts);
    CREATE TABLE IF NOT EXISTS heartbeats (recv_ts REAL, boot INTEGER, data TEXT);
    CREATE TABLE IF NOT EXISTS events (ts REAL, kind TEXT, detail TEXT);
    """)
    db.commit()


def q(sql, args=()):
    with db_lock:
        return db.execute(sql, args).fetchall()


def ex(sql, args=()):
    with db_lock:
        db.execute(sql, args)
        db.commit()


def event(kind, detail, ts=None):
    ex("INSERT INTO events VALUES (?,?,?)", (ts or time.time(), kind, detail))
    print(f"[event] {kind}: {detail}")


# ---------------- Message handling ----------------
def verify(raw):
    """Return the parsed message if its HMAC is valid, else None."""
    if ',"sig":"' not in raw:
        return None
    body, sig = raw.rsplit(',"sig":"', 1)
    sig = sig[:-2]  # strip closing "}
    expected = hmac.new(HMAC_KEY, body.encode(), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, sig):
        return None
    return json.loads(raw)


def handle(topic, raw, now=None):
    now = now or time.time()
    kind = topic.removeprefix(TOPIC_PREFIX)

    # The broker's last-will "offline" message can't carry a signature.
    if kind == "status" and raw == '{"device":"T16","online":false}':
        event("offline", "device disconnected without saying goodbye (last will)", now)
        return

    msg = verify(raw)
    if msg is None:
        event("bad_signature", f"rejected message on {topic}", now)
        return

    if kind == "status":
        event("online", f"boot #{msg['boot']}, reset reason: {msg['reset']}, uptime {msg['uptime']} s", now)
    elif kind == "heartbeat":
        ex("INSERT INTO heartbeats VALUES (?,?,?)", (now, msg["boot"], json.dumps(msg)))
    elif kind == "vibration":
        ts = msg["ts"] or int(now)  # ts is 0 if the device never got NTP time
        prev = q("SELECT state FROM readings ORDER BY ts DESC, seq DESC LIMIT 1")
        with db_lock:
            cur = db.execute(
                "INSERT OR IGNORE INTO readings VALUES (?,?,?,?,?,?,?)",
                (msg["boot"], msg["seq"], ts, now, msg["vib"], msg["state"], msg["bf"]))
            db.commit()
        if cur.rowcount and prev and prev[0][0] != msg["state"]:
            event("state", f"{prev[0][0]} -> {msg['state']}", ts)


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"MQTT connected: {reason_code}")
    client.subscribe(TOPIC_PREFIX + "#", qos=1)  # (re)subscribe on every reconnect


def on_message(client, userdata, m):
    try:
        handle(m.topic, m.payload.decode())
    except Exception as e:  # never let one bad message kill the collector
        print("bad message:", e, m.payload[:200])


def start_mqtt():
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="t16-collector")
    c.username_pw_set(MQTT_USER, MQTT_PASS)
    c.tls_set()
    c.reconnect_delay_set(1, 30)
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    c.loop_start()
    return c


# ---------------- Web ----------------
app = Flask(__name__, static_folder="static")


@app.get("/")
def index():
    return send_from_directory("static", "index.html")


@app.get("/api/live")
def live():
    r = q("SELECT ts, vib, state, recv_ts, boot FROM readings ORDER BY recv_ts DESC LIMIT 1")
    hb = q("SELECT data, recv_ts FROM heartbeats ORDER BY recv_ts DESC LIMIT 1")
    since = None
    if r:  # when did the current state start?
        row = q("SELECT MAX(ts) FROM readings WHERE state != ?", (r[0][2],))
        since = row[0][0]
    last_seen = max([x for x in [r and r[0][3], hb and hb[0][1]] if x] or [0])
    return jsonify({
        "reading": dict(zip(["ts", "vib", "state", "recv_ts", "boot"], r[0])) if r else None,
        "state_since": since,
        "heartbeat": json.loads(hb[0][0]) if hb else None,
        "online": time.time() - last_seen < OFFLINE_AFTER_S,
        "last_seen": last_seen,
    })


@app.get("/api/history")
def history():
    hours = min(float(request.args.get("hours", 24)), 24 * 14)
    start = time.time() - hours * 3600
    bucket = max(READING_PERIOD_S, int(hours * 3600 / 1500))  # ~1500 points max
    points = q("""
        SELECT CAST(ts / ? AS INTEGER) * ? AS t, AVG(vib), MAX(vib),
               MAX(CASE state WHEN 'UNBALANCED' THEN 3 WHEN 'RUNNING' THEN 2
                              WHEN 'DONE' THEN 1 ELSE 0 END)
        FROM readings WHERE ts >= ? GROUP BY t ORDER BY t""", (bucket, bucket, start))
    gaps = q("""
        SELECT prev_ts, ts, ts - prev_ts FROM (
            SELECT ts, LAG(ts) OVER (ORDER BY ts) AS prev_ts FROM readings WHERE ts >= ?)
        WHERE ts - prev_ts > ? ORDER BY ts DESC""", (start, GAP_S))
    events = q("SELECT ts, kind, detail FROM events WHERE ts >= ? ORDER BY ts DESC LIMIT 200", (start,))
    return jsonify({
        "bucket": bucket,
        "points": [{"t": p[0], "avg": p[1], "max": p[2], "state": p[3]} for p in points],
        "gaps": [{"from": g[0], "to": g[1], "seconds": g[2]} for g in gaps],
        "events": [{"ts": e[0], "kind": e[1], "detail": e[2]} for e in events],
    })


@app.get("/api/stats")
def stats():
    """Reliability numbers for the 72 h write-up."""
    first, last, received, backfilled = q(
        "SELECT MIN(ts), MAX(ts), COUNT(*), SUM(bf) FROM readings")[0]
    per_boot = q("SELECT boot, MIN(seq), MAX(seq), COUNT(*) FROM readings GROUP BY boot")
    lost_in_boot = sum((hi - lo + 1) - cnt for _, lo, hi, cnt in per_boot)
    span = (last - first) if first else 0
    expected = span // READING_PERIOD_S + 1 if first else 0
    gap_s = q("""SELECT COALESCE(SUM(ts - prev_ts), 0) FROM (
                 SELECT ts, LAG(ts) OVER (ORDER BY ts) AS prev_ts FROM readings)
                 WHERE ts - prev_ts > ?""", (GAP_S,))[0][0]
    counts = dict(q("SELECT kind, COUNT(*) FROM events GROUP BY kind"))
    return jsonify({
        "first": first, "last": last, "span_s": span,
        "expected": expected, "received": received,
        "received_pct": round(100 * received / expected, 2) if expected else None,
        "backfilled": backfilled or 0,
        "missing_seq_within_boots": lost_in_boot,
        "boots": len(per_boot),
        "gap_seconds": gap_s,
        "uptime_pct": round(100 * (span - gap_s) / span, 2) if span else None,
        "offline_events": counts.get("offline", 0),
        "bad_signatures": counts.get("bad_signature", 0),
    })


if __name__ == "__main__":
    missing = [k for k in ("MQTT_HOST", "MQTT_USER", "MQTT_PASS", "HMAC_KEY") if not os.environ.get(k)]
    if missing:
        raise SystemExit(f"Set these environment variables first: {', '.join(missing)}")
    start_mqtt()
    app.run(host="0.0.0.0", port=WEB_PORT)
