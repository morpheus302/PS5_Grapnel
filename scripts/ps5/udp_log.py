#!/usr/bin/env python3
"""(ps5, grapnel) Keeps the log lines the console sends with HOOKSHOT_PS5_LOG_UDP=<this PC's address>:<port> in the title's
settings file (/data/grapnel/hookshot_env.txt). Optional: without the setting the title writes its log to
/data/grapnel/hookshot_log.txt only.

Each datagram is one line of the game's log or of its start-up trail ("trail: ..."). They are appended to
<out>/grapnel-udp-<start time>.log with the PC's arrival time in front, flushed at every line, so the file is complete up
to the last datagram that left the console, whatever happens to the console next. The trail's lines are echoed.

    python3 -I scripts/ps5/udp_log.py [--port 9148] [--bind 0.0.0.0] [--out DIR] [--quiet]

The PC's firewall must let UDP in on that port from the console. Standard library only.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import datetime
import os
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9148)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--out", default=".", help="the folder the log goes to (default: the current one)")
    ap.add_argument("--quiet", action="store_true", help="do not echo the trail's lines")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, "grapnel-udp-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.bind((a.bind, a.port))
    print("udp_log: listening on %s:%d, writing %s" % (a.bind, a.port, path), flush=True)
    with open(path, "ab", buffering=0) as f:
        while True:
            data, _ = s.recvfrom(2048)
            line = data.rstrip(b"\r\n")
            f.write(b"%.3f " % time.time() + line + b"\n")
            if not a.quiet and line.startswith(b"trail: "):
                print(line.decode("utf-8", "replace"), flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
