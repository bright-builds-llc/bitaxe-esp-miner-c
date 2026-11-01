#!/usr/bin/env python3
"""Runs the Rust repository's Stratum V2 fixture, on the pipes its V2 modes require.

Its readiness line goes to stdout, and it holds private addresses and a public key, so keep it private.
"""
import json
import os
import signal
import socket
import subprocess
import sys


def route_ipv4_to(peer: str) -> str:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((peer, 9))
    address = probe.getsockname()[0]
    probe.close()
    return address


def run_noise(binary: str, root: str, attempt: str, peer: str) -> int:
    """The Noise mode's bounds are fixed by the fixture, and it writes its readiness to its root."""
    return subprocess.call(
        [
            binary, "--mode", "noise-serial", "--listen-address", f"{route_ipv4_to(peer)}:0",
            "--expected-peer-address", peer, "--accept-timeout-seconds", "120", "--read-timeout-seconds", "10",
            "--lifetime-seconds", "150", "--session-timeout-seconds", "180", "--private-root", root, "--attempt-id", attempt,
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
    )


def main() -> int:
    if sys.argv[3] == "noise":
        return run_noise(*sys.argv[1:3], *sys.argv[4:6])
    binary, root, scope, attempt, peer, user = sys.argv[1:7]
    # The connection facts land next to the root: the fixture refuses a root it didn't create.
    connection_path = f"{root}.connection.json"
    input_read, input_write = os.pipe()
    ready_read, ready_write = os.pipe()
    connection_read, connection_write = os.pipe()
    listen = route_ipv4_to(peer)

    def inherit_descriptors() -> None:
        os.dup2(input_read, 0)
        os.dup2(ready_write, 1)
        os.dup2(connection_write, 3)

    child = subprocess.Popen(
        [binary, "--mode", "v2-serial", "--scope", scope, "--private-root", root, "--attempt-id", attempt],
        preexec_fn=inherit_descriptors,
        close_fds=False,
    )
    signal.signal(signal.SIGTERM, lambda *_: (child.kill(), os._exit(143)))
    for descriptor in (input_read, ready_write, connection_write):
        os.close(descriptor)
    line = json.dumps({
        "schema": "str005-v2-fixture-input-v1",
        "scope": scope,
        "attemptId": attempt,
        "listenIpv4": listen,
        "expectedPeerIpv4": peer,
        "userIdentity": user,
    }, separators=(",", ":"))
    with os.fdopen(input_write, "w") as stream:
        stream.write(line + "\n")
    with os.fdopen(ready_read, "r") as stream:
        ready = stream.readline()
    if not ready:
        return child.wait() or 1
    sys.stdout.write(ready if ready.endswith("\n") else ready + "\n")
    sys.stdout.flush()
    with os.fdopen(connection_read, "rb") as stream, open(connection_path, "wb", opener=lambda p, f: os.open(p, f, 0o600)) as out:
        out.write(stream.read())
    return child.wait()


if __name__ == "__main__":
    sys.exit(main())
