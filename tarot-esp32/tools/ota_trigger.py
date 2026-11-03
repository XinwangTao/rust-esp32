"""BLE 触发 OTA 并监视通知：python ota_trigger.py [命令] [监听秒数]

用法：
  python ota_trigger.py                 # 只读 STAT（默认）
  python ota_trigger.py OTA 180         # 触发 OTA 并监听 180 秒
"""
import asyncio
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

from bleak import BleakClient, BleakScanner

SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


async def main() -> None:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "STAT"
    listen_s = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (ad.service_uuids and SVC in [u.lower() for u in ad.service_uuids])
        or (d.name or "").startswith("MicDesktop"),
        timeout=30,
    )
    if dev is None:
        print("NOT_FOUND: 没扫到麦克风（板子是否在广播？平板 App 是否占着连接？）")
        return
    print(f"FOUND {dev.name} {dev.address}", flush=True)

    def on_notify(_sender, data: bytearray) -> None:
        if len(data) == 166:          # ADPCM 音频帧，忽略
            return
        try:
            text = data.decode("ascii")
        except UnicodeDecodeError:
            print(f"[bin len={len(data)}]", flush=True)
            return
        print(f"[{len(data)}] {text.rstrip()}", flush=True)

    async with BleakClient(dev) as client:
        await client.start_notify(TX, on_notify)
        await asyncio.sleep(0.4)
        try:
            await client.write_gatt_char(RX, b"STAT", response=False)
            print("WRITE STAT ok", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"WRITE STAT failed: {e}", flush=True)
        await asyncio.sleep(1.0)
        if cmd != "STAT":
            print(f"WRITE {cmd}", flush=True)
            await client.write_gatt_char(RX, cmd.encode(), response=False)
        await asyncio.sleep(listen_s)


if __name__ == "__main__":
    asyncio.run(main())
