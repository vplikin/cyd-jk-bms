#!/usr/bin/env python3
"""Probe an EPEVER / Solar Guardian BLE gateway (HN_xxxx) for Modbus access.

Usage:  python3 tools/probe_mppt.py HN_XXXXXXXXXXXX
Close Solar Guardian / nRF Connect on the phone first (the module takes one link).
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

if len(sys.argv) < 2:
    sys.exit(__doc__)
NAME = sys.argv[1]
ADDRS = [1, 10, 255, 0]
WAIT = 1.5


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def req(addr: int, reg: int, n: int, fn: int = 0x04) -> bytes:
    body = bytes([addr, fn, reg >> 8, reg & 0xFF, n >> 8, n & 0xFF])
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def find_modbus(buf: bytes):
    """Return (addr, payload) of the first valid 0x03 reply inside buf."""
    for i in range(len(buf) - 4):
        if buf[i + 1] not in (0x03, 0x04, 0x83, 0x84):
            continue
        if buf[i + 1] & 0x80:
            frame = buf[i:i + 5]
        else:
            ln = buf[i + 2]
            frame = buf[i:i + 3 + ln + 2]
        if len(frame) >= 5 and crc16(frame[:-2]) == (frame[-2] | frame[-1] << 8):
            return frame
    return None


def decode_3100(frame: bytes) -> str:
    d = frame[3:3 + frame[2]]
    r = [(d[k] << 8) | d[k + 1] for k in range(0, len(d), 2)]
    s = []
    if len(r) >= 1: s.append(f"PV {r[0] / 100:.2f} В")
    if len(r) >= 2: s.append(f"PV {r[1] / 100:.2f} А")
    if len(r) >= 4: s.append(f"PV {((r[3] << 16) | r[2]) / 100:.1f} Вт")
    return ", ".join(s)


async def main():
    print(f"Ищу {NAME} ... (телефон должен быть отключён от модуля)")
    dev = await BleakScanner.find_device_by_name(NAME, timeout=15)
    if not dev:
        print("НЕ НАЙДЕН. Закройте Solar Guardian / nRF Connect и выключите BT на телефоне.")
        return
    print(f"Найден: {dev.address}")

    async with BleakClient(dev, timeout=20) as cl:
        print("Подключено.\n\n=== GATT ===")
        notify, write = [], []
        for svc in cl.services:
            print(f"Service {svc.uuid}")
            for ch in svc.characteristics:
                print(f"   {ch.uuid}  [{', '.join(ch.properties)}]  {ch.description}")
                if svc.uuid.startswith("02f00000"):
                    continue  # OTA service of the module, leave it alone
                if {"notify", "indicate"} & set(ch.properties):
                    notify.append(ch)
                if {"write", "write-without-response"} & set(ch.properties):
                    write.append(ch)

        rx = []  # (time, uuid, bytes)

        def make_cb(u):
            def cb(_, data: bytearray):
                rx.append((time.time(), u, bytes(data)))
                print(f"      <- {u[4:8]}: {bytes(data).hex(' ')}")
            return cb

        for ch in notify:
            try:
                await cl.start_notify(ch, make_cb(ch.uuid))
                print(f"Подписка: {ch.uuid}")
            except Exception as e:
                print(f"Подписка {ch.uuid} не удалась: {e}")
        await asyncio.sleep(1)
        rx.clear()

        print("\n=== Пробы Modbus (fn 0x04, чтение 0x3100 x4) ===")
        hits = []
        for ch in write:
            for addr in ADDRS:
                pkt = req(addr, 0x3100, 4)
                resp = "write-without-response" not in ch.properties
                print(f"   -> {ch.uuid[4:8]} addr={addr}: {pkt.hex(' ')}")
                start = len(rx)
                try:
                    await cl.write_gatt_char(ch, pkt, response=resp)
                except Exception as e:
                    print(f"      ошибка записи: {e}")
                    continue
                await asyncio.sleep(WAIT)
                got = rx[start:]
                if not got:
                    continue
                # Reassemble per notifying characteristic
                by = {}
                for _, u, b in got:
                    by[u] = by.get(u, b"") + b
                for u, buf in by.items():
                    fr = find_modbus(buf)
                    hits.append((ch.uuid, addr, u, buf, fr))
                    if fr:
                        break

        print("\n=== ВЕРДИКТ ===")
        good = [h for h in hits if h[4] and not h[4][1] & 0x80]
        if good:
            w, a, n, buf, fr = good[0]
            print("✅ Прозрачный Modbus работает.")
            print(f"   write={w}\n   notify={n}\n   адрес контроллера={a}")
            print(f"   ответ: {fr.hex(' ')}")
            print(f"   данные: {decode_3100(fr)}")
        elif any(h[4] for h in hits):
            w, a, n, buf, fr = [h for h in hits if h[4]][0]
            print("⚠️  Modbus отвечает ошибкой (0x84) — канал верный, но регистр/адрес не тот.")
            print(f"   write={w} notify={n} addr={a} ответ={fr.hex(' ')}")
        elif hits:
            print("⚠️  Что-то отвечает, но это не Modbus RTU. Пришлите вывод целиком:")
            for w, a, n, buf, _ in hits:
                print(f"   write={w[4:8]} addr={a} notify={n[4:8]}: {buf.hex(' ')}")
        else:
            print("❌ Ни на одну пробу нет ответа. Возможно, нужна служебная команда")
            print("   от приложения (рукопожатие). Пришлите вывод целиком.")


asyncio.run(main())
