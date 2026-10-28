#!/usr/bin/env python3
"""扬声器自检工具：向下行端口发控制帧，并（可选）用 INMP441 上行回采验证。

仅依赖标准库 + pyserial。

用法示例:
  # 只触发自检音（不需要串口）
  python tools/speaker_selftest.py --ip 192.168.1.50

  # 触发 + 回采验证（需要 pyserial，串口可选用于复位）
  python tools/speaker_selftest.py --ip 192.168.1.50 --verify --dump spk.pcm

  # 排查接线：逐个尝试引脚排列
  python tools/speaker_selftest.py --ip 192.168.1.50 --verify --sweep
"""
import argparse
import itertools
import math
import socket
import struct
import sys
import threading
import time

HEADER = struct.Struct("<4sBBHIIHH")
RATE = 16000
TONES = (880, 1320)


def goertzel(buf, freq, rate=RATE):
    """单频点功率，归一化到样本数平方。"""
    n = len(buf)
    w = 2.0 * math.pi * freq / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in buf:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (n * n)


class Collector(threading.Thread):
    """收上行 AUD0 帧，记录 (时间, s16 样本)。"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.frames = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.2)

    def run(self):
        while not self._stop.is_set():
            try:
                pkt, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            if len(pkt) < HEADER.size:
                continue
            magic, ver, flags, hlen, seq, rate, samples, plen = HEADER.unpack_from(pkt, 0)
            if magic != b"AUD0" or hlen > len(pkt):
                continue
            body = pkt[hlen:hlen + plen]
            n = len(body) // 2
            if n == 0:
                continue
            with self._lock:
                self.frames.append((time.time(), struct.unpack("<%dh" % n, body[:n * 2])))

    def mark(self):
        with self._lock:
            return len(self.frames), time.time()

    def since(self, idx):
        with self._lock:
            return list(self.frames[idx:])

    def stop(self):
        self._stop.set()
        try:
            self.sock.close()
        except OSError:
            pass


def send_frame(ip, port, payload):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.sendto(payload, (ip, port))
    finally:
        s.close()


def analyse(frames, t0, freq_list):
    """返回每个频点的命中帧数与该帧的 RMS。"""
    hits = {f: 0 for f in freq_list}
    for ts, buf in frames:
        if ts < t0:
            continue
        rms = math.sqrt(sum(x * x for x in buf) / len(buf))
        for f in freq_list:
            if goertzel(buf, f) > 3000:
                hits[f] += 1
    return hits


def sweep(ip, port, collector, pins):
    print("%-16s %-10s %s" % ("pins(bclk,ws,din)", "verdict", "tone energy"))
    for perm in itertools.permutations(pins):
        idx, _ = collector.mark()
        send_frame(ip, port, ("PIN:%d,%d,%d" % perm).encode())
        time.sleep(1.5)
        t0 = time.time() - 1.5
        frames = collector.since(idx)
        best = max((goertzel(b, f) for _, b in frames for f in TONES), default=0.0)
        print("%-16s %-10s %.0f" % ("%d,%d,%d" % perm, "HIT" if best > 3000 else "-", best))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True, help="板子局域网 IP")
    ap.add_argument("--spk-port", type=int, default=5001, help="下行端口（默认 5001）")
    ap.add_argument("--udp-port", type=int, default=5000, help="上行端口（默认 5000）")
    ap.add_argument("--verify", action="store_true", help="用上行回采验证是否真的发声")
    ap.add_argument("--sweep", action="store_true", help="遍历 7/15/16 的 6 种排列")
    ap.add_argument("--pins", default="7,15,16", help="--sweep 使用的引脚集合")
    ap.add_argument("--dump", help="把回采的原始 s16le PCM 存盘")
    a = ap.parse_args()

    if not a.verify:
        send_frame(a.ip, a.spk_port, b"TONE")
        print("TONE sent to %s:%d" % (a.ip, a.spk_port))
        return 0

    col = Collector(a.udp_port)
    col.start()
    time.sleep(1.0)
    if not col.frames:
        print("!! 没有收到上行音频，检查 --udp-port 与板子是否在线", file=sys.stderr)
        return 1

    if a.sweep:
        sweep(a.ip, a.spk_port, col, [int(x) for x in a.pins.split(",")])
    else:
        idx, t0 = col.mark()
        send_frame(a.ip, a.spk_port, b"TONE")
        time.sleep(1.6)
        frames = col.since(idx)
        hits = analyse(frames, t0, TONES)
        print("回采帧数: %d" % len(frames))
        for f in TONES:
            print("  %4d Hz 命中 %d 帧" % (f, hits[f]))
        ok = all(hits[f] >= 1 for f in TONES)
        print("结论: %s" % ("PASS 扬声器有声音" if ok else "FAIL 未检出声音"))
        if a.dump:
            with open(a.dump, "wb") as fh:
                for _, buf in frames:
                    fh.write(struct.pack("<%dh" % len(buf), *buf))
            print("已保存: %s" % a.dump)
        col.stop()
        return 0 if ok else 1

    col.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
