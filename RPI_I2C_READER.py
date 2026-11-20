#!/usr/bin/env python3
"""Read the 32-byte frame from the STM32 RTOS monitor (I2C slave 0x2A)."""
import struct, time
from smbus2 import SMBus, i2c_msg

ADDR, BUS, FRAME_LEN = 0x2A, 1, 32
NAMES = ("joystick", "tamperCount", "wakeCount", "adcRaw", "cpuLoad", "workloadCount")

def read_frame(bus):
    msg = i2c_msg.read(ADDR, FRAME_LEN)      # pure read, no register write
    bus.i2c_rdwr(msg)
    data = bytes(msg)
    chk = 0
    for b in data[:-1]:
        chk ^= b
    if chk != data[-1]:
        raise ValueError("checksum mismatch")
    return dict(zip(NAMES, struct.unpack("<6I", data[:24])))

with SMBus(BUS) as bus:
    while True:
        try:
            print(read_frame(bus))
        except (OSError, ValueError) as e:
            print("I2C error:", e)
        time.sleep(0.5)
