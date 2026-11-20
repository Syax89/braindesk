#!/usr/bin/env python3
"""Proxy HTTP minimale (GET/HEAD/... e CONNECT) solo su 127.0.0.1.
Serve al Brain per apt tramite tunnel SSH inverso (ssh -R 3128:127.0.0.1:3128)."""
import socket, threading, select, sys
from urllib.parse import urlsplit

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 3128

def pipe(a, b):
    socks = [a, b]
    try:
        while True:
            r, _, x = select.select(socks, [], socks, 120)
            if x or not r:
                return
            for s in r:
                data = s.recv(65536)
                if not data:
                    return
                (b if s is a else a).sendall(data)
    except OSError:
        pass

def handle(c):
    try:
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = c.recv(65536)
            if not chunk:
                return
            head += chunk
        hdr, rest = head.split(b"\r\n\r\n", 1)
        lines = hdr.split(b"\r\n")
        method, target, ver = lines[0].split(b" ", 2)
        if method == b"CONNECT":
            host, port = target.decode().rsplit(":", 1)
            s = socket.create_connection((host, int(port)), timeout=30)
            c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            if rest:
                s.sendall(rest)
        else:
            u = urlsplit(target.decode())
            s = socket.create_connection((u.hostname, u.port or 80), timeout=30)
            path = (u.path or "/") + (("?" + u.query) if u.query else "")
            out = [method + b" " + path.encode() + b" " + ver]
            for l in lines[1:]:
                if not l.lower().startswith((b"proxy-connection:", b"proxy-authorization:")):
                    out.append(l)
            s.sendall(b"\r\n".join(out) + b"\r\n\r\n" + rest)
        pipe(c, s)
        s.close()
    except Exception as e:
        try:
            c.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        except OSError:
            pass
    finally:
        c.close()

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(64)
print("proxy su 127.0.0.1:%d" % PORT, flush=True)
while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
