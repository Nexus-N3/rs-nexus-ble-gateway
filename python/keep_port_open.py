import glob
import time
import serial

ports = sorted(glob.glob("/dev/serial/by-id/*-if01"))

if len(ports) != 1:
    raise SystemExit(f"Expected one gateway: {ports}")

with serial.Serial(ports[0], 1_000_000, timeout=0.1) as ser:
    ser.setDTR(True)

    print("Gateway data port open, DTR asserted.", flush=True)
    time.sleep(30)

print("Serial port closed.")