#!/usr/bin/env python3
"""Truck Monitor collector for CT100. Standard library only.

POST /truck/log    newline-delimited JSON records, X-Truck-Token header
GET  /truck/api/latest    newest stored records

Records are unique on (device, boot_epoch, seq), so a node can safely resend a
batch whose reply it never received.
"""

import hmac
import json
import os
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE = Path(os.environ.get("TRUCK_LOG_DIR", "/home/ray/truck-log"))
PORT = int(os.environ.get("TRUCK_LOG_PORT", "8101"))
DB_PATH = BASE / "truck.db"
TOKEN = (BASE / "token").read_text().strip()
MAX_BODY = 256 * 1024
FIELDS = ("timestamp", "voltage", "current", "temp", "humidity", "temp_bmp", "pressure")

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
  device TEXT NOT NULL,
  boot_epoch INTEGER NOT NULL,
  seq INTEGER NOT NULL,
  timestamp INTEGER,
  voltage REAL,
  current REAL,
  temp REAL,
  humidity REAL,
  temp_bmp REAL,
  pressure REAL,
  received_at INTEGER NOT NULL,
  PRIMARY KEY (device, boot_epoch, seq)
)
"""


def connect():
    db = sqlite3.connect(DB_PATH, timeout=10)
    db.row_factory = sqlite3.Row
    return db


def store(lines):
    """Stores each parsable record, returning (added, duplicate, rejected)."""
    added = duplicate = rejected = 0
    now = int(time.time())
    with connect() as db:
        for line in lines:
            if not line.strip():
                continue
            try:
                rec = json.loads(line)
                key = (str(rec["device"]), int(rec["boot_epoch"]), int(rec["seq"]))
            except (ValueError, KeyError, TypeError):
                rejected += 1
                continue
            cur = db.execute(
                "INSERT OR IGNORE INTO readings VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                key + tuple(rec.get(f) for f in FIELDS) + (now,),
            )
            if cur.rowcount:
                added += 1
            else:
                duplicate += 1
    return added, duplicate, rejected


class Handler(BaseHTTPRequestHandler):
    server_version = "truck-log"
    sys_version = ""

    def reply(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/truck/log":
            return self.reply(404, {"error": "not found"})
        token = self.headers.get("X-Truck-Token", "")
        if not hmac.compare_digest(token.encode(), TOKEN.encode()):
            return self.reply(403, {"error": "forbidden"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            return self.reply(413, {"error": "bad length"})
        lines = self.rfile.read(length).decode("utf8", "replace").split("\n")
        added, duplicate, rejected = store(lines)
        self.reply(200, {"added": added, "duplicate": duplicate, "rejected": rejected})

    def do_GET(self):
        if self.path.split("?")[0] != "/truck/api/latest":
            return self.reply(404, {"error": "not found"})
        with connect() as db:
            rows = db.execute(
                "SELECT * FROM readings ORDER BY received_at DESC, boot_epoch DESC, seq DESC LIMIT 50"
            ).fetchall()
            total = db.execute("SELECT COUNT(*) FROM readings").fetchone()[0]
        self.reply(200, {"total": total, "readings": [dict(r) for r in rows]})

    def log_message(self, fmt, *args):
        return


def main():
    with connect() as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.execute(SCHEMA)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
