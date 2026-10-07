#!/usr/bin/env python3
"""Send .amp project files to the synth over UART0 (protocol: project_xfer.h).

Usage:
    amp_xfer.py put PORT FILE.amp [--slot N] [--force]
    amp_xfer.py ls PORT

put stores the file in the first free slot, or in slot N (refused if used,
unless --force); the stored name is mapped from the file name on the device.
Prints the device's final reply; exit status 0 on "P< ok", 1 otherwise.
Needs pyserial.
"""

import argparse
import base64
import os
import sys
import time

import serial

BAUD = 115200
PIECE = 144            # bytes per d line (192 base64 characters)
REPLY_TIMEOUT = 5.0    # seconds per reply
FINAL_TIMEOUT = 10.0   # the reply to the last d line waits for the flash write


def read_reply(ser, timeout):
    """Return the next line starting with "P<"; other output is skipped."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode(errors="replace").rstrip("\r\n")
        if text.startswith("P<"):
            return text
    raise TimeoutError(f"no reply within {timeout:g} s")


def send(ser, line, timeout=REPLY_TIMEOUT):
    ser.write((line + "\n").encode())
    return read_reply(ser, timeout)


def is_ok(reply):
    return reply == "P< ok" or reply.startswith("P< ok ")


def cmd_put(ser, path, slot, force):
    with open(path, "rb") as f:
        data = f.read()
    slot_arg = "-" if slot is None else f"{slot}{'!' if force else ''}"
    reply = send(ser, f"P> put {slot_arg} {len(data)} {os.path.basename(path)}")
    if not is_ok(reply):
        print(reply)
        return 1
    for off in range(0, len(data), PIECE):
        piece = data[off:off + PIECE]
        last = off + PIECE >= len(data)
        reply = send(ser, "P> d " + base64.b64encode(piece).decode("ascii"),
                     FINAL_TIMEOUT if last else REPLY_TIMEOUT)
        if not is_ok(reply):
            print(reply)
            send(ser, "P> abort")
            return 1
    print(reply)
    return 0


def cmd_ls(ser):
    ser.write(b"P> ls\n")
    while True:
        reply = read_reply(ser, REPLY_TIMEOUT)
        print(reply)
        if not reply.startswith("P< slot "):
            return 0 if is_ok(reply) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_put = sub.add_parser("put", help="store a .amp file in a project slot")
    p_put.add_argument("port")
    p_put.add_argument("file")
    p_put.add_argument("--slot", type=int, help="slot index (default: first free)")
    p_put.add_argument("--force", action="store_true",
                       help="replace what is in --slot")
    p_ls = sub.add_parser("ls", help="list used project slots")
    p_ls.add_argument("port")
    args = ap.parse_args()

    if args.cmd == "put" and args.force and args.slot is None:
        ap.error("--force needs --slot")

    with serial.Serial(args.port, BAUD, timeout=0.1) as ser:
        ser.reset_input_buffer()
        try:
            if args.cmd == "put":
                return cmd_put(ser, args.file, args.slot, args.force)
            return cmd_ls(ser)
        except TimeoutError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1


if __name__ == "__main__":
    sys.exit(main())
