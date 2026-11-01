#!/usr/bin/env python3
"""Bridges a serial port to stdin and stdout as raw bytes.

The chip resets when it sees DTR low with RTS high. Opening a port raises both and pyserial then
updates DTR first, so DTR must stay high here or the update passes through that reset state.
"""
import os
import select
import sys

import serial

port = serial.Serial()
port.port = sys.argv[1]
port.baudrate = 115200
port.timeout = 0
port.dtr = True
port.rts = False
port.open()

stdin_fd, stdout_fd = sys.stdin.fileno(), sys.stdout.fileno()
while True:
    readable, _, _ = select.select([port.fileno(), stdin_fd], [], [], 1.0)
    if port.fileno() in readable:
        data = port.read(4096)
        if data:
            os.write(stdout_fd, data)
    if stdin_fd in readable:
        data = os.read(stdin_fd, 1024)
        if not data:
            break
        port.write(data)
        port.flush()
