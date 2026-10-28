#!/usr/bin/env python3
"""文字转语音并推给板子扬声器播放。

用 Windows SAPI（离线）合成 wav，重采样成 16 kHz / 单声道 / s16LE，
再按 AUD0 帧 20 ms 一包推到板子的 UDP 下行端口。

用法:
  python tools/tts_play.py --ip 192.168.1.50 --text "你好"
  python tools/tts_play.py --ip 192.168.1.50 --text "测试一下" --verify
  python tools/tts_play.py --ip 192.168.1.50 --text "你好" --voice "Microsoft Huihui Desktop"
"""
import argparse
import audioop
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave

HEADER = struct.Struct("<4sBBHIIHH")
RATE = 16000
FRAME = 320  # 20 ms


def synthesize(text, voice, out_path):
    """调用 Windows SAPI 合成 wav。"""
    esc = out_path.replace("'", "''")
    t = text.replace("'", "''")
    v = voice.replace("'", "''")
    ps = (
        "Add-Type -AssemblyName System.Speech; "
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        f"$s.SelectVoice('{v}'); "
        f"$s.SetOutputToWaveFile('{esc}'); "
        f"$s.Speak('{t}'); "
        "$s.Dispose();"
    )
    r = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", ps],
        capture_output=True, text=True,
    )
    if r.returncode != 0 or not os.path.exists(out_path):
        raise RuntimeError("SAPI 合成失败: %s %s" % (r.stdout, r.stderr))


def to_pcm16k(path):
    """读 wav → 16 kHz 单声道 s16LE 字节流。"""
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        raw = audioop.lin2lin(raw, width, 2)
    if ch == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    if rate != RATE:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, RATE, None)
    return raw, (ch, width, rate)


def play(ip, port, pcm):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total = len(pcm) // 2 // FRAME
    t0 = time.time()
    for i in range(total):
        chunk = pcm[i * FRAME * 2:(i + 1) * FRAME * 2]
        payload = struct.pack("<%dh" % FRAME, *struct.unpack("<%dh" % FRAME, chunk))
        hdr = HEADER.pack(b"AUD0", 1, 0, 20, i, RATE, FRAME, len(payload))
        s.sendto(hdr + payload, (ip, port))
        # 实时节奏：按帧时长节流，允许略微领先，避免 I2S DMA 欠载
        target = t0 + (i + 1) * FRAME / RATE
        delay = target - time.time()
        if delay > 0:
            time.sleep(delay)
    s.close()
    return total, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--port", type=int, default=5001)
    ap.add_argument("--voice", default="Microsoft Huihui Desktop")
    ap.add_argument("--keep-wav", help="保留下载前的 wav 到该路径")
    ap.add_argument("--verify", action="store_true", help="用 INMP441 上行回采确认确实发声")
    ap.add_argument("--udp-port", type=int, default=5000, help="上行端口（仅 --verify 用）")
    a = ap.parse_args()

    tmp = a.keep_wav or os.path.join(tempfile.gettempdir(), "tts_play.wav")
    synthesize(a.text, a.voice, tmp)
    pcm, src = to_pcm16k(tmp)
    print("合成: %r  原始 %d ch / %d bit / %d Hz  时长 %.2fs" %
          (a.text, src[0], src[1] * 8, src[2], len(pcm) / 2 / RATE))

    col = None
    base = 0.0
    if a.verify:
        from speaker_selftest import Collector  # 与本脚本同目录
        col = Collector(a.udp_port)
        col.start()
        time.sleep(1.0)
        # 播放前先测静音基线，语音能量通常远低于满幅正弦
        _, tb = col.mark()
        time.sleep(1.0)
        pre = [b for ts, b in col.frames if ts >= tb and b]
        if pre:
            rms_list = sorted((sum(x * x for x in b) / len(b)) ** 0.5 for b in pre)
            base = rms_list[len(rms_list) // 2]
        idx, t0 = col.mark()

    n, took = play(a.ip, a.port, pcm)
    print("已推送 %d 帧（%.2fs 音频，用时 %.2fs）-> %s:%d" % (n, len(pcm) / 2 / RATE, took, a.ip, a.port))

    rc = 0
    if col is not None:
        time.sleep(0.8)
        frames = col.since(idx)
        thr = max(250.0, base * 2.0)
        voiced = 0
        peak = 0.0
        for ts, buf in frames:
            if ts < t0 or not buf:
                continue
            rms = (sum(x * x for x in buf) / len(buf)) ** 0.5
            peak = max(peak, rms)
            if rms > thr:
                voiced += 1
        col.stop()
        print("回采: %d 帧，静音基线 RMS %.0f，超阈值(%.0f) %d 帧，峰值 RMS %.0f" %
              (len(frames), base, thr, voiced, peak))
        if voiced < 3:
            print("结论: FAIL 未检出播放声")
            rc = 1
        else:
            print("结论: PASS 扬声器确实放出了这段语音")

    if not a.keep_wav and os.path.exists(tmp):
        try:
            os.remove(tmp)
        except OSError:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
