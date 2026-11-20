"""RiseQuest server: receives device uploads, stores them in SQLite and serves
the web dashboard. Runs on the team laptop.

    python app.py                 # serve (http://0.0.0.0:8080)
    python app.py hash-password   # make RQ_ADMIN_PASSWORD_HASH for .env
"""
from __future__ import annotations

import functools
import getpass
import hashlib
import hmac
import json
import os
import secrets
import sqlite3
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

from dotenv import load_dotenv
from flask import Flask, abort, g, jsonify, redirect, render_template, request, send_file, session, url_for
from werkzeug.security import check_password_hash, generate_password_hash

BASE = Path(__file__).resolve().parent
load_dotenv(BASE / ".env")

DEVICE_ID = os.environ.get("RQ_DEVICE_ID", "box-01")
DEVICE_SECRET = os.environ.get("RQ_DEVICE_SECRET", "")
ADMIN_HASH = os.environ.get("RQ_ADMIN_PASSWORD_HASH", "")
DB_PATH = Path(os.environ.get("RQ_DB_PATH", BASE / "riseq.db"))
FIRMWARE_DIR = BASE / "firmware"
ONLINE_AFTER_S = 5

DEFAULT_SETTINGS = {
    "alarm_enabled": False,
    "alarm_hour": 7,
    "alarm_minute": 0,
    "timezone": "PST8PDT,M3.2.0,M11.1.0",
    "required_s": 10,
    "pause_ms": 2500,
    "min_flips": 6,
    "face_cos_pct": 80,
    "heartbeat_s": 60,
}
# (min, max) for numeric settings; required_s can never go below the 10 s rule.
LIMITS = {
    "alarm_hour": (0, 23),
    "alarm_minute": (0, 59),
    "required_s": (10, 120),
    "pause_ms": (1000, 5000),
    "min_flips": (2, 60),
    "face_cos_pct": (60, 95),
    "heartbeat_s": (10, 600),
}

app = Flask(__name__)
app.config.update(
    SECRET_KEY=os.environ.get("RQ_SECRET_KEY") or secrets.token_hex(32),
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Strict",
    MAX_CONTENT_LENGTH=64 * 1024,
)

# ---------------------------------------------------------------- database
SCHEMA = """
CREATE TABLE IF NOT EXISTS records (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL,
    boot_id INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    kind TEXT NOT NULL,
    t INTEGER,                 -- device wall time (epoch s), NULL if unknown
    up_ms INTEGER,
    received_at REAL NOT NULL,
    data TEXT NOT NULL,
    UNIQUE (device_id, boot_id, seq)
);
CREATE INDEX IF NOT EXISTS records_kind ON records (kind, received_at);
CREATE TABLE IF NOT EXISTS live (
    device_id TEXT PRIMARY KEY,
    received_at REAL NOT NULL,
    boot_id INTEGER,
    fw INTEGER,
    data TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS uploads (       -- one row per accepted upload, for gap analysis
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL,
    boot_id INTEGER NOT NULL,
    received_at REAL NOT NULL,
    n_records INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    version INTEGER NOT NULL,
    data TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS commands (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL,
    command TEXT NOT NULL,
    created_at REAL NOT NULL,
    done_at REAL
);
"""


def db() -> sqlite3.Connection:
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH, timeout=10)
        g.db.row_factory = sqlite3.Row
        g.db.execute("PRAGMA journal_mode=WAL")
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.executescript(SCHEMA)
    if not conn.execute("SELECT 1 FROM settings WHERE id = 1").fetchone():
        conn.execute("INSERT INTO settings (id, version, data) VALUES (1, 1, ?)", (json.dumps(DEFAULT_SETTINGS),))
    conn.commit()
    conn.close()


def load_settings() -> tuple[int, dict]:
    row = db().execute("SELECT version, data FROM settings WHERE id = 1").fetchone()
    return row["version"], {**DEFAULT_SETTINGS, **json.loads(row["data"])}


def firmware_manifest() -> dict | None:
    path = FIRMWARE_DIR / "manifest.json"
    if not path.exists() or not (FIRMWARE_DIR / "firmware.bin").exists():
        return None
    return json.loads(path.read_text())


# ---------------------------------------------------------------- device auth
def device_auth(view):
    """HMAC-SHA256 over method, path, nonce and body with the shared device secret."""

    @functools.wraps(view)
    def wrapper(*args, **kwargs):
        if not DEVICE_SECRET:
            abort(503, "RQ_DEVICE_SECRET not configured")
        dev = request.headers.get("X-RQ-Device", "")
        nonce = request.headers.get("X-RQ-Nonce", "")
        sig = request.headers.get("X-RQ-Signature", "")
        body = request.get_data(cache=True, as_text=True)
        msg = f"{request.method}\n{request.path}\n{nonce}\n{body}".encode()
        expected = hmac.new(DEVICE_SECRET.encode(), msg, hashlib.sha256).hexdigest()
        if dev != DEVICE_ID or not nonce or not hmac.compare_digest(sig, expected):
            abort(401)
        return view(*args, **kwargs)

    return wrapper


# ---------------------------------------------------------------- device API
@app.post("/api/device/ingest")
@device_auth
def ingest():
    payload = request.get_json(force=True, silent=True)
    if not isinstance(payload, dict):
        abort(400)
    now = time.time()
    boot_id = int(payload.get("boot_id", 0))
    conn = db()

    ack = 0
    records = payload.get("records") or []
    for r in records:
        seq = int(r["seq"])
        data = r.get("data") if isinstance(r.get("data"), dict) else {}
        conn.execute(
            "INSERT OR IGNORE INTO records (device_id, boot_id, seq, kind, t, up_ms, received_at, data) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (DEVICE_ID, boot_id, seq, str(r.get("kind", "?"))[:20], r.get("t"), r.get("up_ms"), now, json.dumps(data)),
        )
        ack = max(ack, seq)
        if r.get("kind") == "remote_release" and "command_id" in data:
            conn.execute("UPDATE commands SET done_at = ? WHERE id = ? AND done_at IS NULL", (now, data["command_id"]))

    if isinstance(payload.get("live"), dict):
        conn.execute(
            "INSERT INTO live (device_id, received_at, boot_id, fw, data) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET received_at=excluded.received_at, boot_id=excluded.boot_id, "
            "fw=excluded.fw, data=excluded.data",
            (DEVICE_ID, now, boot_id, payload.get("fw"), json.dumps(payload["live"])),
        )
    conn.execute(
        "INSERT INTO uploads (device_id, boot_id, received_at, n_records) VALUES (?, ?, ?, ?)",
        (DEVICE_ID, boot_id, now, len(records)),
    )
    conn.commit()

    version, _ = load_settings()
    cmd = conn.execute(
        "SELECT id, command FROM commands WHERE device_id = ? AND done_at IS NULL ORDER BY id LIMIT 1", (DEVICE_ID,)
    ).fetchone()
    manifest = firmware_manifest()
    resp = {
        "ack_seq": ack,
        "server_time": int(now),
        "config_version": version,
        "fw_latest": manifest["version"] if manifest else 0,
    }
    if cmd:
        resp.update(command=cmd["command"], command_id=cmd["id"])
    return jsonify(resp)


@app.get("/api/device/config")
@device_auth
def device_config():
    version, s = load_settings()
    return jsonify({"version": version, **s})


@app.get("/api/device/firmware/manifest")
@device_auth
def firmware_manifest_view():
    m = firmware_manifest()
    if not m:
        abort(404)
    return jsonify(m)


@app.get("/api/device/firmware/image")
@device_auth
def firmware_image():
    if not firmware_manifest():
        abort(404)
    return send_file(FIRMWARE_DIR / "firmware.bin", mimetype="application/octet-stream")


# ---------------------------------------------------------------- dashboard auth
def login_required(view):
    @functools.wraps(view)
    def wrapper(*args, **kwargs):
        if not session.get("user"):
            if request.path.startswith("/api/"):
                abort(401)
            return redirect(url_for("login", next=request.path))
        if request.method == "POST" and request.headers.get("X-CSRF-Token") != session.get("csrf"):
            abort(403)
        return view(*args, **kwargs)

    return wrapper


@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        if request.form.get("csrf") != session.get("login_csrf"):
            abort(403)
        if ADMIN_HASH and check_password_hash(ADMIN_HASH, request.form.get("password", "")):
            session.clear()
            session["user"] = "admin"
            session["csrf"] = secrets.token_urlsafe(24)
            return redirect(url_for("index"))
        time.sleep(1)  # slow down guessing
        error = "Wrong password" if ADMIN_HASH else "RQ_ADMIN_PASSWORD_HASH is not set in server/.env"
    session["login_csrf"] = secrets.token_urlsafe(24)
    return render_template("login.html", csrf=session["login_csrf"], error=error)


@app.post("/logout")
@login_required
def logout():
    session.clear()
    return jsonify(ok=True)


@app.get("/")
@login_required
def index():
    return render_template("index.html", csrf=session["csrf"], device_id=DEVICE_ID)


# ---------------------------------------------------------------- dashboard API
def event_time(row) -> float:
    return row["t"] if row["t"] else row["received_at"]


@app.get("/api/live")
@login_required
def api_live():
    conn = db()
    row = conn.execute("SELECT * FROM live WHERE device_id = ?", (DEVICE_ID,)).fetchone()
    hb = conn.execute(
        "SELECT * FROM records WHERE device_id = ? AND kind = 'heartbeat' ORDER BY received_at DESC, id DESC LIMIT 1",
        (DEVICE_ID,),
    ).fetchone()
    now = time.time()
    out = {"device_id": DEVICE_ID, "server_time": now, "online": False, "live": None, "heartbeat": None}
    if row:
        age = now - row["received_at"]
        out.update(
            online=age < ONLINE_AFTER_S, age_s=round(age, 1), boot_id=row["boot_id"], fw=row["fw"],
            live=json.loads(row["data"]),
        )
    if hb:
        out["heartbeat"] = {"at": event_time(hb), **json.loads(hb["data"])}
    return jsonify(out)


@app.get("/api/events")
@login_required
def api_events():
    limit = min(int(request.args.get("limit", 50)), 500)
    include_hb = request.args.get("heartbeats") == "1"
    q = "SELECT * FROM records WHERE device_id = ?"
    if not include_hb:
        q += " AND kind != 'heartbeat'"
    rows = db().execute(q + " ORDER BY received_at DESC, id DESC LIMIT ?", (DEVICE_ID, limit)).fetchall()
    return jsonify([
        {"at": event_time(r), "kind": r["kind"], "boot_id": r["boot_id"], "seq": r["seq"],
         "time_from_device": r["t"] is not None, "data": json.loads(r["data"])}
        for r in rows
    ])


@app.get("/api/history")
@login_required
def api_history():
    days = min(int(request.args.get("days", 7)), 90)
    since = time.time() - days * 86400
    rows = db().execute(
        "SELECT * FROM records WHERE device_id = ? AND kind IN "
        "('attempt_start','attempt_complete','attempt_abandon','lock','unlock','alarm_start') "
        "AND COALESCE(t, received_at) >= ? ORDER BY COALESCE(t, received_at), boot_id, seq",
        (DEVICE_ID, since),
    ).fetchall()

    attempts, daily = [], defaultdict(lambda: {"completed": 0, "abandoned": 0, "attempts": 0, "locks": 0})
    for r in rows:
        at = event_time(r)
        day = datetime.fromtimestamp(at).strftime("%Y-%m-%d")
        d = json.loads(r["data"])
        if r["kind"] == "attempt_start":
            daily[day]["attempts"] += 1
        elif r["kind"] == "lock":
            daily[day]["locks"] += 1
        elif r["kind"] in ("attempt_complete", "attempt_abandon"):
            ok = r["kind"] == "attempt_complete"
            daily[day]["completed" if ok else "abandoned"] += 1
            attempts.append({
                "at": at, "outcome": "completed" if ok else "abandoned",
                "active_s": round(d.get("active_ms", 0) / 1000, 1), "wall_s": round(d.get("wall_ms", 0) / 1000, 1),
                "flips": d.get("flips", 0), "rejected": d.get("rejected", 0),
            })
    # fill empty days so the chart has a continuous axis
    out_days = []
    for i in range(days - 1, -1, -1):
        day = datetime.fromtimestamp(time.time() - i * 86400).strftime("%Y-%m-%d")
        out_days.append({"day": day, **daily.get(day, {"completed": 0, "abandoned": 0, "attempts": 0, "locks": 0})})
    attempts.reverse()
    return jsonify({"days": out_days, "attempts": attempts[:100]})


@app.get("/api/reliability")
@login_required
def api_reliability():
    """Per boot: expected vs received records, reset reasons, queue overflow and heartbeat gaps."""
    conn = db()
    _, s = load_settings()
    hb_interval = s["heartbeat_s"]
    boots = conn.execute(
        "SELECT boot_id, COUNT(*) AS received, MIN(seq) AS min_seq, MAX(seq) AS max_seq, "
        "MIN(received_at) AS first_at, MAX(received_at) AS last_at "
        "FROM records WHERE device_id = ? GROUP BY boot_id ORDER BY boot_id DESC LIMIT 50",
        (DEVICE_ID,),
    ).fetchall()
    result = []
    for b in boots:
        boot_id = b["boot_id"]
        boot_ev = conn.execute(
            "SELECT data FROM records WHERE device_id = ? AND boot_id = ? AND kind = 'boot'", (DEVICE_ID, boot_id)
        ).fetchone()
        hbs = conn.execute(
            "SELECT t, received_at, data FROM records WHERE device_id = ? AND boot_id = ? AND kind = 'heartbeat' "
            "ORDER BY seq", (DEVICE_ID, boot_id)
        ).fetchall()
        seqs = [r["seq"] for r in conn.execute(
            "SELECT seq FROM records WHERE device_id = ? AND boot_id = ? ORDER BY seq", (DEVICE_ID, boot_id))]
        expected = b["max_seq"]  # seq starts at 1 every boot
        missing_ranges, prev = [], 0
        for sq in seqs:
            if sq > prev + 1:
                missing_ranges.append([prev + 1, sq - 1])
            prev = sq

        overflow = upload_fail = 0
        gaps = []
        prev_t = None
        for h in hbs:
            d = json.loads(h["data"])
            overflow = max(overflow, d.get("ovf", 0))
            upload_fail = max(upload_fail, d.get("fail", 0))
            t = h["t"] or h["received_at"]
            if prev_t and t - prev_t > 2.5 * hb_interval:
                gaps.append({"from": prev_t, "to": t, "seconds": round(t - prev_t)})
            prev_t = t
        # Upload gaps show Wi-Fi/server outages even when heartbeats were queued and back-filled.
        ups = [r["received_at"] for r in conn.execute(
            "SELECT received_at FROM uploads WHERE device_id = ? AND boot_id = ? ORDER BY id", (DEVICE_ID, boot_id))]
        outages = []
        for a, c in zip(ups, ups[1:]):
            if c - a > 30:
                outages.append({"from": a, "to": c, "seconds": round(c - a)})

        bd = json.loads(boot_ev["data"]) if boot_ev else {}
        last_hb = json.loads(hbs[-1]["data"]) if hbs else {}
        result.append({
            "boot_id": boot_id,
            "reset_reason": bd.get("reason", "unknown (boot record not received)"),
            "fw": bd.get("fw"),
            "first_at": b["first_at"], "last_at": b["last_at"],
            "uptime_s": last_hb.get("up"),
            "expected": expected, "received": b["received"], "missing": expected - b["received"],
            "missing_ranges": missing_ranges[:20],
            "queue_overflow": overflow,
            "upload_failures": upload_fail,
            "heartbeats": len(hbs),
            "heartbeat_gaps": gaps[:20],
            "upload_outages": outages[:20],
        })
    return jsonify({"heartbeat_s": hb_interval, "boots": result})


@app.route("/api/settings", methods=["GET", "POST"])
@login_required
def api_settings():
    version, current = load_settings()
    if request.method == "GET":
        return jsonify({"version": version, **current})
    body = request.get_json(force=True, silent=True) or {}
    new = dict(current)
    errors = {}
    for key, default in DEFAULT_SETTINGS.items():
        if key not in body:
            continue
        val = body[key]
        if isinstance(default, bool):
            new[key] = bool(val)
        elif isinstance(default, int):
            try:
                val = int(val)
            except (TypeError, ValueError):
                errors[key] = "must be a number"
                continue
            lo, hi = LIMITS[key]
            if not lo <= val <= hi:
                errors[key] = f"must be between {lo} and {hi}"
                continue
            new[key] = val
        else:
            val = str(val).strip()
            if not 2 <= len(val) <= 47:
                errors[key] = "invalid"
                continue
            new[key] = val
    if errors:
        return jsonify(errors=errors), 400
    if new != current:
        version += 1
        db().execute("UPDATE settings SET version = ?, data = ? WHERE id = 1", (version, json.dumps(new)))
        db().commit()
    return jsonify({"version": version, **new})


@app.post("/api/commands/release")
@login_required
def api_release():
    conn = db()
    conn.execute(
        "INSERT INTO commands (device_id, command, created_at) VALUES (?, 'release', ?)", (DEVICE_ID, time.time())
    )
    conn.commit()
    return jsonify(ok=True)


# ---------------------------------------------------------------- entry point
def main():
    if len(sys.argv) > 1 and sys.argv[1] == "hash-password":
        pw = getpass.getpass("Dashboard password: ")
        print(f"RQ_ADMIN_PASSWORD_HASH={generate_password_hash(pw)}")
        return
    init_db()
    host = os.environ.get("RQ_HOST", "0.0.0.0")
    port = int(os.environ.get("RQ_PORT", 8080))
    if not DEVICE_SECRET or not ADMIN_HASH:
        print("WARNING: set RQ_DEVICE_SECRET and RQ_ADMIN_PASSWORD_HASH in server/.env", file=sys.stderr)
    print(f"RiseQuest server on http://{host}:{port}  (db: {DB_PATH})")
    try:
        from waitress import serve

        serve(app, host=host, port=port, threads=8)
    except ImportError:
        app.run(host=host, port=port)


if __name__ == "__main__":
    main()
