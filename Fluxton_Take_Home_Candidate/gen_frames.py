#!/usr/bin/env python3
"""
Fluxton take-home assignment - UART capture generator.

Writes a raw byte stream (the bytes a sensor would send over UART) to a file.
Frame format (little-endian):
    SOF(0xAA) | LEN | TYPE | PAYLOAD[LEN] | CRC16 (CRC-16/CCITT-FALSE over LEN, TYPE, PAYLOAD)

TYPE 0x01 reading   : LEN 6, payload = int16 temperature (centi-degC) + uint32 sensor timestamp (ms)
TYPE 0x02 heartbeat : LEN 0, no payload

Usage:
    python3 gen_frames.py --profile noisy --seed 2002 --out noisy.bin --manifest noisy.json

Profiles: clean, noisy, burst. Any profile value can be overridden on the command line,
e.g. --readings 500 --p-corrupt 0.2
The manifest records what was generated, including how many intact frames are in the file.
Requires only the Python 3 standard library.
"""
import argparse
import json
import random
import struct

SOF = 0xAA
TYPE_READING = 0x01
TYPE_HEARTBEAT = 0x02
MAX_LEN = 64

PROFILES = {
    "clean": dict(readings=2000, hb_every=25, ts_step=100, p_garbage=0.0, p_corrupt=0.0,
                  p_truncate=0.0, p_bad_len=0.0, baud=115200),
    "noisy": dict(readings=3000, hb_every=25, ts_step=100, p_garbage=0.10, p_corrupt=0.06,
                  p_truncate=0.04, p_bad_len=0.02, baud=115200),
    "burst": dict(readings=20000, hb_every=100, ts_step=1, p_garbage=0.01, p_corrupt=0.01,
                  p_truncate=0.0, p_bad_len=0.0, baud=921600),
}


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_frame(ftype: int, payload: bytes) -> bytes:
    body = bytes([len(payload), ftype]) + payload
    return bytes([SOF]) + body + struct.pack("<H", crc16_ccitt_false(body))


def garbage(rng: random.Random) -> bytes:
    # Random noise; about 15% of the bytes are a fake start-of-frame to make resync harder.
    n = rng.randint(1, 24)
    return bytes(SOF if rng.random() < 0.15 else rng.randrange(256) for _ in range(n))


def corrupt(rng: random.Random, frame: bytes) -> bytes:
    # Flip one bit anywhere after SOF. CRC-16 catches every single-bit error.
    b = bytearray(frame)
    i = rng.randrange(1, len(b))
    b[i] ^= 1 << rng.randrange(8)
    return bytes(b)


def generate(cfg: dict, seed: int):
    rng = random.Random(seed)
    out = bytearray()
    m = dict(intact_readings=0, intact_heartbeats=0, corrupted=0, truncated=0,
             bad_length=0, garbage_runs=0, garbage_bytes=0)
    temp = 2500
    ts = 0
    tmin, tmax = None, None

    def emit_frame(frame: bytes, kind: str) -> bool:
        # A frame is either sent intact, or damaged in exactly one way.
        r = rng.random()
        if r < cfg["p_corrupt"]:
            out.extend(corrupt(rng, frame))
            m["corrupted"] += 1
        elif r < cfg["p_corrupt"] + cfg["p_truncate"]:
            out.extend(frame[:rng.randint(1, len(frame) - 1)])
            m["truncated"] += 1
        else:
            out.extend(frame)
            m[kind] += 1
            return True
        return False

    for i in range(cfg["readings"]):
        if rng.random() < cfg["p_garbage"]:
            g = garbage(rng)
            out.extend(g)
            m["garbage_runs"] += 1
            m["garbage_bytes"] += len(g)
        if rng.random() < cfg["p_bad_len"]:
            out.extend(bytes([SOF, rng.randint(MAX_LEN + 1, 255)]))
            out.extend(bytes(rng.randrange(256) for _ in range(rng.randint(0, 10))))
            m["bad_length"] += 1

        temp = max(-4000, min(8500, temp + rng.randint(-25, 25)))
        ts += cfg["ts_step"]
        if emit_frame(make_frame(TYPE_READING, struct.pack("<hI", temp, ts & 0xFFFFFFFF)),
                      "intact_readings"):
            tmin = temp if tmin is None else min(tmin, temp)
            tmax = temp if tmax is None else max(tmax, temp)

        if cfg["hb_every"] and (i + 1) % cfg["hb_every"] == 0:
            emit_frame(make_frame(TYPE_HEARTBEAT, b""), "intact_heartbeats")

    manifest = {
        "seed": seed,
        "bytes": len(out),
        "suggested_baud": cfg["baud"],
        "intact_frames": m["intact_readings"] + m["intact_heartbeats"],
        "intact_readings": m["intact_readings"],
        "intact_heartbeats": m["intact_heartbeats"],
        "injected": {k: m[k] for k in ("corrupted", "truncated", "bad_length",
                                       "garbage_runs", "garbage_bytes")},
        "temp_centi_min": tmin,
        "temp_centi_max": tmax,
    }
    return bytes(out), manifest


def main():
    ap = argparse.ArgumentParser(description="Generate a UART capture for the Fluxton take-home.")
    ap.add_argument("--profile", choices=PROFILES, default="clean")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True, help="output .bin file")
    ap.add_argument("--manifest", help="optional JSON file describing the capture")
    for key, val in PROFILES["noisy"].items():
        ap.add_argument("--" + key.replace("_", "-"), type=type(val), default=None)
    args = ap.parse_args()

    cfg = dict(PROFILES[args.profile])
    for key in cfg:
        v = getattr(args, key)
        if v is not None:
            cfg[key] = v

    data, manifest = generate(cfg, args.seed)
    manifest = {"profile": args.profile, **manifest}
    with open(args.out, "wb") as f:
        f.write(data)
    if args.manifest:
        with open(args.manifest, "w") as f:
            json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
