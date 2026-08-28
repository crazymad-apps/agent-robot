#!/usr/bin/env python3
import argparse
import base64
import pathlib
import re
import sys
import time

import serial

def parse_args():
    parser = argparse.ArgumentParser(description="Capture Agent Robot WAV transfer")
    parser.add_argument("port", help="Serial device, for example /dev/cu.usbmodem1101")
    parser.add_argument("output", type=pathlib.Path, help="Destination .wav file")
    return parser.parse_args()


def main():
    args = parse_args()
    raw_transfer = bytearray()
    expected_size = None
    receiving = False
    deadline = time.monotonic() + 60

    with serial.Serial(args.port, 115200, timeout=1) as device:
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            stripped = line.strip()

            if stripped.startswith(b"AR_WAV_BEGIN "):
                expected_size = int(stripped.split()[1])
                receiving = True
                print(f"Receiving {expected_size} WAV bytes...")
                continue
            if stripped == b"AR_WAV_END":
                break

            if receiving:
                # Preserve the exact stream. A serial timeout can split both an
                # encoded line and an ANSI terminal sequence across reads.
                raw_transfer.extend(line)
            else:
                print(stripped.decode("utf-8", errors="replace"))
        else:
            raise TimeoutError("Timed out waiting for WAV transfer")

    if expected_size is None:
        raise RuntimeError("WAV begin marker was not received")
    without_ansi = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", raw_transfer)
    encoded_payload = b"".join(without_ansi.split())
    try:
        payload = base64.b64decode(encoded_payload, validate=True)
    except ValueError as error:
        expected_encoded_size = 4 * ((expected_size + 2) // 3)
        raise RuntimeError(
            "Invalid base64 payload: "
            f"{error}; expected {expected_encoded_size} encoded bytes, "
            f"received {len(encoded_payload)}"
        ) from error
    if len(payload) != expected_size:
        raise RuntimeError(
            f"WAV size mismatch: expected {expected_size}, received {len(payload)}"
        )
    if payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise RuntimeError("Received payload is not a RIFF/WAVE file")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(f"Saved {len(payload)} bytes to {args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"Capture failed: {error}", file=sys.stderr)
        sys.exit(1)
