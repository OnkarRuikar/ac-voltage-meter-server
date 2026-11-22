import json
import time
import math
import sqlite3
import threading
import urllib.request
from pathlib import Path
from datetime import datetime, timezone, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
from contextlib import contextmanager

# ---------------- CONFIGURATION ----------------

MCU_URL = "http://192.168.0.240/v"

POLL_INTERVAL = 2
MCU_TIMEOUT = 10

RETENTION_SECONDS = 7 * 24 * 60 * 60

HOST = "0.0.0.0"
PORT = 8888

DB_PATH = Path(__file__).with_name("voltage.db")
                                                            
# ---------------- SHARED STATUS ----------------

status_lock = threading.Lock()

status = {
    "connected": False,
    "last_poll": None,
    "last_success": None,
    "error": "Waiting for first poll"
}


# ---------------- DATABASE ----------------

@contextmanager
def get_db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row

    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()

def init_db():
    with get_db() as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=NORMAL")
        db.execute("""
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp INTEGER NOT NULL,
                voltage REAL NOT NULL
            )
        """)

        db.execute("""
            CREATE INDEX IF NOT EXISTS idx_timestamp
            ON readings(timestamp)
        """)

        db.commit()

    print("Database initialized:", DB_PATH)


def save_reading(timestamp, voltage):
    with get_db() as db:
        db.execute(
            "INSERT INTO readings (timestamp, voltage) VALUES (?, ?)",
            (timestamp, voltage)
        )


def cleanup_old_records():
    cutoff = int(time.time() * 1000) - RETENTION_SECONDS * 1000

    with get_db() as db:
        db.execute(
            "DELETE FROM readings WHERE timestamp < ?",
            (cutoff,)
        )


# ---------------- MCU POLLING ----------------

def read_mcu():
    request = urllib.request.Request(
        MCU_URL,
        headers={"User-Agent": "PiVoltageMonitor"}
    )

    with urllib.request.urlopen(
        request,
        timeout=MCU_TIMEOUT
    ) as response:

        if response.status != 200:
            raise RuntimeError(f"HTTP status {response.status}")

        value = response.read(128).decode("utf-8").strip()

        voltage = float(value)

        if not math.isfinite(voltage):
            raise ValueError("Invalid voltage value")

        return voltage


def polling_worker():
    print("Starting MCU polling...")

    poll_count = 0
    next_poll = time.monotonic()

    while True:
        next_poll += POLL_INTERVAL

        timestamp = int(time.time() * 1000)

        try:
            voltage = read_mcu()
            connected = True
            error = None

        except Exception as e:
            voltage = 0.0
            connected = False
            error = str(e)

        try:
            # Store every poll, including errors as 0V
            save_reading(timestamp, voltage)

            with status_lock:
                status["connected"] = connected
                status["last_poll"] = timestamp
                status["error"] = error

                if connected:
                    status["last_success"] = timestamp

            poll_count += 1

            # Remove expired records every 10 minutes
            if poll_count >= 300:
                cleanup_old_records()
                poll_count = 0

            if connected:
                print(f"[MCU] {voltage} V")
            else:
                print(f"[MCU] DISCONNECTED: {error}")

        except Exception as e:
            print(f"[DATABASE ERROR] {e}")

        # Maintain approximately 2-second polling interval
        delay = next_poll - time.monotonic()

        if delay > 0:
            time.sleep(delay)
        else:
            # Avoid rapid catch-up polling
            next_poll = time.monotonic()


# ---------------- DATA HELPERS ----------------

def format_datetime(timestamp):
    ist_tz = timezone(timedelta(hours=5, minutes=30))
    return datetime.fromtimestamp(
        timestamp / 1000,
        tz=ist_tz
    ).isoformat()


def get_latest():
    with get_db() as db:
        row = db.execute("""
            SELECT timestamp, voltage
            FROM readings
            ORDER BY id DESC
            LIMIT 1
        """).fetchone()

    if row is None:
        return None

    timestamp = row["timestamp"]

    return {
        "voltage": row["voltage"],
        "timestamp": timestamp,
        "datetime": format_datetime(timestamp)
    }


def get_history(since=None):
    with get_db() as db:

        if since is None:
            rows = db.execute("""
                SELECT timestamp, voltage
                FROM readings
                ORDER BY timestamp ASC
            """).fetchall()

        else:
            rows = db.execute("""
                SELECT timestamp, voltage
                FROM readings
                WHERE timestamp >= ?
                ORDER BY timestamp ASC
            """, (since,)).fetchall()

    return {
        "count": len(rows),
        "readings": [
            {
                "timestamp": row["timestamp"],
                "voltage": row["voltage"]
            }
            for row in rows
        ]
    }


# ---------------- HTTP SERVER ----------------

class APIHandler(BaseHTTPRequestHandler):

    def send_json(self, data, status_code=200):
        body = json.dumps(data, allow_nan=False).encode("utf-8")

        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):

        path = urlsplit(self.path).path.rstrip("/")

        try:

            if path == "/voltage":
                result = get_latest()
                self.send_json(result)

            elif path == "/24hr":
                since = int(time.time() * 1000) - 86400 * 1000
                self.send_json(get_history(since))

            elif path == "/allhistory":
                self.send_json(get_history())

            elif path == "/status":
                with status_lock:
                    result = status.copy()

                self.send_json(result)

            else:
                self.send_json({
                    "error": "Endpoint not found"
                }, 404)

        except Exception as e:
            self.send_json({
                "error": str(e)
            }, 500)

    def log_message(self, fmt, *args):
        print("[HTTP]", fmt % args)


# ---------------- MAIN ----------------

if __name__ == "__main__":

    init_db()

    poller = threading.Thread(
        target=polling_worker,
        daemon=True
    )
    poller.start()

    server = ThreadingHTTPServer(
        (HOST, PORT),
        APIHandler
    )

    print(f"HTTP API listening on port {PORT}")

    try:
        server.serve_forever()

    except KeyboardInterrupt:
        print("Stopping server...")

    finally:
        server.server_close()
