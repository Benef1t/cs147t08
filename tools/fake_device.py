"""Simulated RiseQuest box for testing the server and dashboard without hardware.

It speaks the same HMAC-signed protocol as the firmware: a boot event, a lock,
a flip-to-unlock attempt, heartbeats and live samples, plus an optional
server outage during which records are queued and later re-sent.

    python tools/fake_device.py --secret <RQ_DEVICE_SECRET> [--url http://127.0.0.1:8080]
                                [--seconds 60] [--outage 20:35] [--heartbeat 5]
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import math
import random
import time
import urllib.error
import urllib.request

G = 9.80665


class FakeBox:
    def __init__(self, url, device_id, secret, boot_id, heartbeat_s):
        self.url, self.device_id, self.secret = url.rstrip("/"), device_id, secret
        self.boot_id, self.heartbeat_s = boot_id, heartbeat_s
        self.seq, self.nonce = 0, 0
        self.queue: list[dict] = []
        self.start = time.time()
        self.locked = False
        self.flips = 0
        self.active_ms = 0
        self.face_up = True
        self.release_done: set[int] = set()

    def event(self, kind, data=None):
        self.seq += 1
        self.queue.append({"seq": self.seq, "kind": kind, "t": int(time.time()),
                           "up_ms": int((time.time() - self.start) * 1000), "data": data or {}})
        print(f"  event {kind} {data or ''}")

    def request(self, method, path, body=b""):
        self.nonce += 1
        nonce = f"{self.boot_id}-{self.nonce}"
        msg = f"{method}\n{path}\n{nonce}\n{body.decode()}".encode()
        sig = hmac.new(self.secret.encode(), msg, hashlib.sha256).hexdigest()
        req = urllib.request.Request(self.url + path, data=body if method == "POST" else None, method=method, headers={
            "Content-Type": "application/json", "X-RQ-Device": self.device_id,
            "X-RQ-Nonce": nonce, "X-RQ-Signature": sig})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def live(self):
        t = time.time()
        if self.locked and self.flips:
            # alternating face, with flip transitions
            phase = (t * 1.25) % 2
            az = G * math.cos(phase * math.pi)
            ax = G * math.sin(phase * math.pi) * 0.9
        else:
            az, ax = G, 0.0
        ax += random.gauss(0, 0.08)
        ay = random.gauss(0, 0.08)
        az += random.gauss(0, 0.08)
        tilt = math.degrees(math.acos(max(-1, min(1, az / math.sqrt(ax * ax + ay * ay + az * az)))))
        return {"ax": round(ax, 2), "ay": round(ay, 2), "az": round(az, 2), "tilt": round(tilt, 1),
                "pitch": round(math.degrees(math.atan2(-ax, math.hypot(ay, az))), 1),
                "roll": round(math.degrees(math.atan2(ay, az)), 1),
                "face": "up" if tilt < 37 else "down" if tilt > 143 else "edge",
                "progress": min(99, self.active_ms // 100) if self.flips else 0,
                "active_ms": self.active_ms, "flips": self.flips, "attempt": self.flips > 0, "paused": False,
                "shaking": False, "latch": "locked" if self.locked else "unlocked", "alarm": False,
                "imu_ok": True, "latch_cycles": 3, "queued": len(self.queue), "rssi": -58}

    def upload(self):
        batch = self.queue[:15]
        body = json.dumps({"device_id": self.device_id, "boot_id": self.boot_id, "fw": 1, "time_valid": True,
                           "config_version": 0, "live": self.live(), "records": batch}).encode()
        resp = self.request("POST", "/api/device/ingest", body)
        self.queue = [r for r in self.queue if r["seq"] > resp["ack_seq"]]
        cid = resp.get("command_id")
        if resp.get("command") == "release" and cid not in self.release_done:
            self.release_done.add(cid)
            self.event("remote_release", {"command_id": cid})
            if self.locked:
                self.locked = False
                self.event("unlock", {"source": "remote", "cycles": 4})
        return resp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--device", default="box-01")
    ap.add_argument("--secret", required=True)
    ap.add_argument("--boot", type=int, default=int(time.time()) % 100000)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--heartbeat", type=int, default=5)
    ap.add_argument("--outage", default="", help="start:end seconds during which uploads are skipped")
    args = ap.parse_args()
    out_a, out_b = (map(int, args.outage.split(":")) if args.outage else (-1, -1))

    box = FakeBox(args.url, args.device, args.secret, args.boot, args.heartbeat)
    box.event("boot", {"reason": "power_on", "fw": 1, "locked": 0, "imu": 1, "oled": 1})
    box.event("time_sync", {})
    last_hb = 0.0
    ok = fail = 0
    for i in range(args.seconds):
        t = i
        if t == 3:
            box.locked = True
            box.event("lock", {"source": "button"})
        if t == 6:
            box.event("attempt_start", {})
        if 6 <= t < 9:  # first attempt abandoned
            box.flips += 2
        if t == 9:
            box.flips, box.active_ms = 0, 0
            box.event("attempt_abandon", {"active_ms": 2900, "wall_ms": 3100, "flips": 4})
        if t == 15:
            box.event("attempt_start", {})
        if 15 <= t < 26:
            box.flips += 2
            box.active_ms = min(10000, box.active_ms + 1000)
        if t == 26 and box.locked:
            box.event("attempt_complete", {"active_ms": 10040, "wall_ms": 10600, "flips": box.flips, "rejected": 1})
            box.locked = False
            box.flips, box.active_ms = 0, 0
            box.event("unlock", {"source": "flips", "cycles": 4})
        if time.time() - last_hb >= args.heartbeat:
            last_hb = time.time()
            box.event("heartbeat", {"up": t, "heap": 182000, "rssi": -58, "q": len(box.queue), "ovf": 0,
                                    "fail": fail, "wc": 1, "lk": int(box.locked), "imu": 1})
        if out_a <= t < out_b:
            print(f"t={t:3d}s  (simulated outage, {len(box.queue)} queued)")
            fail += 1
        else:
            try:
                r = box.upload()
                ok += 1
                print(f"t={t:3d}s  ack={r['ack_seq']} queued={len(box.queue)} cfg=v{r['config_version']}")
            except (urllib.error.URLError, OSError) as e:
                fail += 1
                print(f"t={t:3d}s  upload failed: {e}")
        time.sleep(1)
    while box.queue:
        box.upload()
    print(f"done: {ok} uploads ok, {fail} skipped/failed, last seq {box.seq}")


if __name__ == "__main__":
    main()
