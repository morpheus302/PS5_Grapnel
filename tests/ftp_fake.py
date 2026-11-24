#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the console's FTP server, ftpsrv (ps5-payload-dev/ftpsrv cmd.c), for the tests of grapnel_send.py: its
replies and its ways, on a folder of this PC (the jail) and nothing outside it. USER without a password (230), TYPE, PASV,
CWD, PWD, MLSD of the current folder, SIZE, REST then STOR from that offset with the file cut at the end of what came (no
truncation before), APPE, RETR, MKD, RMD, DELE, SELF (a toggle), NOOP, FEAT, QUIT. cut_after: the data connection of the
next STOR is dropped after that many bytes (an interrupted send).

    python3 ftp_fake.py <jail> [port]
"""
import os
import socket
import socketserver
import sys
import threading


class Handler(socketserver.StreamRequestHandler):
    def reply(self, text):
        self.wfile.write((text + "\r\n").encode())

    def path(self, arg):
        p = arg if arg.startswith("/") else os.path.join(self.cwd, arg)
        p = os.path.normpath(p)
        jail = self.server.jail
        if not (p == jail or p.startswith(jail + os.sep)):
            raise PermissionError(p)
        return p

    def handle(self):
        self.cwd, self.offset, self.self2elf, self.data = self.server.jail, 0, True, None
        self.reply("220 ftpsrv (stand-in)")
        for raw in self.rfile:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            self.server.log.append(line)
            try:
                if getattr(self, "c_" + cmd, None) is None:
                    self.reply("502 Command not implemented")
                elif getattr(self, "c_" + cmd)(arg) == "quit":
                    return
            except PermissionError:
                self.reply("550 Permission denied")
            except FileNotFoundError:
                self.reply("550 No such file or directory")
            except OSError as e:
                self.reply("550 %s" % e.strerror)

    def open_data(self):
        conn, _ = self.data.accept()
        self.data.close()
        self.data = None
        return conn

    def c_USER(self, arg):
        self.reply("230 User logged in")

    def c_TYPE(self, arg):
        self.reply("200 Type set")

    def c_NOOP(self, arg):
        self.reply("200 NOOP OK")

    def c_FEAT(self, arg):
        self.reply("211 End")

    def c_QUIT(self, arg):
        self.reply("221 Goodbye")
        return "quit"

    def c_SELF(self, arg):
        self.self2elf = not self.self2elf
        self.reply("226 SELF transfer mode %s" % ("enabled" if self.self2elf else "disabled"))

    def c_PWD(self, arg):
        self.reply('257 "%s"' % self.cwd)

    def c_CWD(self, arg):
        p = self.path(arg)
        if not os.path.isdir(p):
            raise FileNotFoundError(p)
        self.cwd = p
        self.reply("250 OK")

    def c_PASV(self, arg):
        self.data = socket.socket()
        self.data.bind(("127.0.0.1", 0))
        self.data.listen(1)
        port = self.data.getsockname()[1]
        self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)." % (port >> 8, port & 255))

    def c_MLSD(self, arg):
        names = sorted(os.listdir(self.cwd))
        self.reply("150 Opening data transfer")
        conn = self.open_data()
        for n in [".", ".."] + names:
            st = os.stat(os.path.join(self.cwd, n))
            kind = "dir" if os.path.isdir(os.path.join(self.cwd, n)) else "file"
            conn.sendall(("type=%s;size=%d;modify=20261124000000; %s\r\n" % (kind, st.st_size, n)).encode())
        conn.close()
        self.reply("226 Transfer complete")

    def c_SIZE(self, arg):
        p = self.path(arg)
        if not os.path.exists(p):
            self.reply("550 No such file or directory")
            return
        self.reply("213 %d" % os.path.getsize(p))

    def c_REST(self, arg):
        self.offset = int(arg)
        self.reply("350 REST OK")

    def c_STOR(self, arg):
        off, self.offset = self.offset, 0
        p = self.path(arg)
        fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o777)
        os.lseek(fd, off, os.SEEK_SET)
        self.reply("150 Opening data transfer")
        conn = self.open_data()
        cut = self.server.cut_after
        got = 0
        while True:
            block = conn.recv(1 << 16)
            if not block:
                break
            if cut is not None and got + len(block) >= cut:
                os.write(fd, block[:cut - got])
                off += cut - got
                self.server.cut_after = None
                os.ftruncate(fd, off)
                os.close(fd)
                conn.close()
                self.reply("426 Connection closed; transfer aborted")
                return
            os.write(fd, block)
            got += len(block)
            off += len(block)
        os.ftruncate(fd, off)
        os.close(fd)
        conn.close()
        self.reply("226 Data transfer complete")

    def c_APPE(self, arg):
        if not self.offset:
            self.offset = os.path.getsize(self.path(arg))
        self.c_STOR(arg)

    def c_RETR(self, arg):
        p = self.path(arg)
        with open(p, "rb") as f:
            self.reply("150 Opening data transfer")
            conn = self.open_data()
            for block in iter(lambda: f.read(1 << 16), b""):
                conn.sendall(block)
            conn.close()
        self.reply("226 Transfer complete")

    def c_MKD(self, arg):
        os.mkdir(self.path(arg))
        self.reply("226 Directory created")   # (ftpsrv cmd.c: 226, not 257)

    def c_RMD(self, arg):
        os.rmdir(self.path(arg))
        self.reply("226 Directory deleted")   # (ftpsrv cmd.c: 226, not 250)

    def c_DELE(self, arg):
        os.remove(self.path(arg))
        self.reply("226 File deleted")   # (ftpsrv cmd.c: 226, not 250: what a real console answered, 24/11/2026)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, jail, port=0):
        super().__init__(("127.0.0.1", port), Handler)
        self.jail = os.path.realpath(jail)
        self.cut_after = None
        self.log = []

    def start(self):
        threading.Thread(target=self.serve_forever, daemon=True).start()
        return self.server_address[1]


if __name__ == "__main__":
    s = Server(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 2121)
    print("ftp stand-in on port %d, jail %s" % (s.server_address[1], s.jail), flush=True)
    s.serve_forever()
