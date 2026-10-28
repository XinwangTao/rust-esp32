#!/usr/bin/env python3
"""PC-side UDP receiver for ESP32-S3 INMP441 acceptance testing.

Parses the 20-byte AUD0 header emitted by rust_udp_send_audio() in
components/tarot_core/src/lib.rs and reports packet/loss statistics.
Optionally dumps raw s16le PCM payload for playback or inspection:

    ffplay -f s16le -ar 16000 -ch_layout mono dump.pcm
"""
import argparse
import socket
import struct
import sys
import time

# magic(4) version(1) flags(1) header_len(2) sequence(4) sample_rate(4) sample_count(2) payload_len(2)
# little-endian, packed — mirrors #[repr(C, packed)] AudioHeader in lib.rs (20 bytes)
HEADER = struct.Struct("<4sBBHIIHH")
MAGIC = b"AUD0"
VERSION = 1
EXPECTED_RATE = 16000


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bind", default="0.0.0.0", help="local bind IP (default 0.0.0.0)")
    p.add_argument("--port", type=int, default=5000, help="UDP port (default 5000)")
    p.add_argument("--seconds", type=float, default=0.0, help="stop after N seconds (0 = run forever)")
    p.add_argument("--count", type=int, default=0, help="stop after N packets (0 = unlimited)")
    p.add_argument("--dump", metavar="FILE", help="append raw s16le PCM payload to FILE")
    p.add_argument("--quiet", action="store_true", help="suppress per-event lines")
    return p.parse_args()


def main():
    args = parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)
    print(f"listening on {args.bind}:{args.port} ...", flush=True)

    dump = open(args.dump, "ab") if args.dump else None
    stats = {"packets": 0, "bytes": 0, "samples": 0, "bad": 0, "lost": 0}
    next_seq = None
    first_rate = None
    started = time.time()
    last_report = started

    while True:
        now = time.time()
        if args.seconds and now - started >= args.seconds:
            break
        if args.count and stats["packets"] >= args.count:
            break
        try:
            packet, addr = sock.recvfrom(65535)
        except socket.timeout:
            continue
        ip = addr[0]

        if len(packet) < HEADER.size:
            stats["bad"] += 1
            print(f"SHORT packet {len(packet)}B from {ip}", flush=True)
            continue
        magic, version, flags, header_len, sequence, sample_rate, sample_count, payload_len = \
            HEADER.unpack_from(packet, 0)
        if magic != MAGIC or version != VERSION or header_len != HEADER.size:
            stats["bad"] += 1
            print(f"BAD header from {ip}: magic={magic!r} ver={version} hlen={header_len}", flush=True)
            continue
        payload = packet[header_len:]
        if len(payload) != payload_len or payload_len != sample_count * 2:
            stats["bad"] += 1
            print(f"LEN mismatch from {ip}: payload={len(payload)}B "
                  f"hdr={payload_len}B samples={sample_count}", flush=True)
            continue

        if next_seq is not None and sequence != next_seq:
            gap = (sequence - next_seq) & 0xFFFFFFFF
            stats["lost"] += gap
            if not args.quiet:
                print(f"GAP: expected seq={next_seq}, got {sequence} (+{gap} lost)", flush=True)
        next_seq = (sequence + 1) & 0xFFFFFFFF

        stats["packets"] += 1
        stats["bytes"] += len(packet)
        stats["samples"] += sample_count
        if first_rate is None:
            first_rate = sample_rate
            print(f"first packet from {ip}: seq={sequence} rate={sample_rate}Hz "
                  f"samples={sample_count} payload={payload_len}B total={len(packet)}B", flush=True)
        if dump:
            dump.write(payload)
        if not args.quiet and now - last_report >= 5.0:
            last_report = now
            elapsed = now - started
            print(f"[{elapsed:6.1f}s] packets={stats['packets']} lost={stats['lost']} "
                  f"{stats['packets']/elapsed:.1f} pkt/s "
                  f"{stats['samples']/max(elapsed, 1e-9):.0f} samp/s", flush=True)

    if dump:
        dump.close()
    elapsed = max(time.time() - started, 1e-9)
    print("--- summary ---")
    print(f"duration      : {elapsed:.1f}s")
    print(f"packets       : {stats['packets']}")
    print(f"lost (seq gap): {stats['lost']}")
    print(f"bad packets   : {stats['bad']}")
    print(f"total bytes   : {stats['bytes']}")
    print(f"avg packet/s  : {stats['packets']/elapsed:.1f}")
    print(f"avg samples/s : {stats['samples']/elapsed:.0f}")
    if first_rate is not None:
        print(f"sample rate   : {first_rate} Hz")
        health = stats["samples"] / elapsed / EXPECTED_RATE * 100
        print(f"capture health: {health:.1f}% of {EXPECTED_RATE} Hz stream"
              if first_rate == EXPECTED_RATE else
              f"unexpected rate (expected {EXPECTED_RATE} Hz)")
    return 0 if stats["bad"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
