#!/usr/bin/env python3
"""Inspect/control the virtual board (physical GPIO levels)."""
import os
import socket
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: ctl.py GPIO pico PIN GET|PUT|DIR|MUX|PULL [VALUE] | UART pico|esp TX|RX [BYTE] | POWER esp", file=sys.stderr)
        return 2
    path = os.environ.get("BOARDSTATE_SOCKET", "/tmp/boardstate-daemon.sock")
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(path)
        client.sendall(" ".join(sys.argv[1:]).encode())
        answer = client.recv(128).decode().strip()
    print(answer)
    return 1 if answer.startswith("ERR") else 0


if __name__ == "__main__":
    sys.exit(main())
