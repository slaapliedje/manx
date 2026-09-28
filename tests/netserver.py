#!/usr/bin/env python3
"""netserver.py [host] - HTTP (port 8081) and gopher (port 7070) test server
for tests/test_net.sh. Plain sockets, so every framing case is exact.

HTTP paths:
  /len         Content-Length body, keep-alive
  /chunked     chunked body with an extension and a trailer
  /close       HTTP/1.0, body delimited by the close
  /redir       302 -> /redir2 (relative) -> 301 -> /len (absolute)
  /loop        redirects to itself forever
  /big         300000 bytes, chunked, in odd-sized chunks
  /slowhead    head sent in 1-byte pieces
  /count       body says how many requests this connection has served
Gopher: "" (menu), "/file.txt", "/search\tquery".
"""
import gzip, socket, sys, threading, zlib

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
BIG = bytes((i * 7 + 3) % 251 for i in range(300000))
HITS = {}

def send_http(c, path, n_on_conn, req=b""):
    head, _, body = req.partition(b"\r\n\r\n")
    hdrs = {}
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        hdrs[k.strip().lower()] = v.strip()
    method = head.split(b" ", 1)[0]
    if path == "/setcookie":
        c.sendall(b"HTTP/1.1 302 Found\r\nSet-Cookie: sid=abc123; Path=/\r\n"
                  b"Set-Cookie: pref=x; Max-Age=60\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n")
        return True
    query = path.partition("?")[2].encode()
    path = path.partition("?")[0]
    if path == "/echo":
        text = b"method=" + method + (b" query=" + query if query else b"") + b" cookie=" + hdrs.get(b"cookie", b"-") + b" type=" + \
            hdrs.get(b"content-type", b"-") + b" body=" + body
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(text) + text)
        return True
    if path in ("/fresh", "/etag"):
        HITS[path] = HITS.get(path, 0) + 1
        if path == "/etag" and hdrs.get(b"if-none-match") == b'"v1"':
            c.sendall(b'HTTP/1.1 304 Not Modified\r\nETag: "v1"\r\nContent-Length: 0\r\n\r\n')
            return True
        text = b"<title>%s</title><p>served %d times <a href=/len>next</a>" % (path.encode(), HITS[path])
        extra = b"Cache-Control: max-age=60\r\n" if path == "/fresh" else b'ETag: "v1"\r\n'
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n" + extra +
                  b"Content-Length: %d\r\n\r\n" % len(text) + text)
        return True
    if path == "/post303":
        c.sendall(b"HTTP/1.1 303 See Other\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n")
        return True
    if path == "/post307":
        c.sendall(b"HTTP/1.1 307 Temporary Redirect\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n")
        return True
    if path == "/len":
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello")
    elif path == "/chunked":
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=ISO-8859-1\r\n"
                  b"Transfer-Encoding: chunked\r\n\r\n6;x=y\r\n<html>\r\n7\r\nchunked\r\n0\r\nX-T: 1\r\n\r\n")
    elif path == "/close":
        c.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil close")
        return False
    elif path == "/redir":
        c.sendall(b"HTTP/1.1 302 Found\r\nLocation: redir2\r\nContent-Length: 3\r\n\r\nxyz")
    elif path == "/redir2":
        c.sendall(("HTTP/1.1 301 Moved\r\nLocation: http://%s:8081/len\r\nContent-Length: 0\r\n\r\n" % HOST).encode())
    elif path == "/loop":
        c.sendall(b"HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n")
    elif path == "/big":
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n")
        i, step = 0, 1
        while i < len(BIG):
            part = BIG[i:i + step]
            c.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
            i += len(part)
            step = step * 3 % 9973 + 1
        c.sendall(b"0\r\n\r\n")
    elif path == "/slowhead":
        for b in b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n":
            c.sendall(bytes([b]))
        c.sendall(b"slow")
    elif path in ("/gzip", "/deflate", "/rawdeflate", "/gzip-chunked", "/gzip-cut"):
        text = b"compressed " * 3 + b"hello"
        if path == "/deflate":
            body, enc = zlib.compress(text), b"deflate"
        elif path == "/rawdeflate":
            co = zlib.compressobj(6, zlib.DEFLATED, -15)
            body, enc = co.compress(text) + co.flush(), b"deflate"
        else:
            body, enc = gzip.compress(text, mtime=0), b"gzip"
        if path == "/gzip-cut":
            body = body[:len(body) - 6]
        head = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: " + enc + b"\r\n"
        if path == "/gzip-chunked":
            c.sendall(head + b"Transfer-Encoding: chunked\r\n\r\n")
            for i in range(0, len(body), 3):
                part = body[i:i + 3]
                c.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
            c.sendall(b"0\r\n\r\n")
        else:
            c.sendall(head + b"Content-Length: %d\r\n\r\n" % len(body) + body)
    elif path == "/count":
        body = str(n_on_conn).encode()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    else:
        c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
    return True

def http_conn(c):
    buf, n = b"", 0
    try:
        while True:
            while b"\r\n\r\n" not in buf:
                d = c.recv(4096)
                if not d:
                    return
                buf += d
            head, buf = buf.split(b"\r\n\r\n", 1)
            n += 1
            clen = 0
            for line in head.split(b"\r\n")[1:]:
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":", 1)[1])
            while len(buf) < clen:
                d = c.recv(4096)
                if not d:
                    return
                buf += d
            body, buf = buf[:clen], buf[clen:]
            path = head.split(b" ")[1].decode()
            keep = send_http(c, path, n, head + b"\r\n\r\n" + body)
            if not keep or b"HEAD " == head[:5] and False:
                return
    finally:
        c.close()

def gopher_conn(c):
    try:
        sel = b""
        while not sel.endswith(b"\r\n"):
            d = c.recv(1024)
            if not d:
                return
            sel += d
        sel = sel[:-2].decode()
        if sel == "":
            c.sendall(("iWelcome\t\terror.host\t1\r\n0A file\t/file.txt\t%s\t7070\r\n.\r\n" % HOST).encode())
        elif sel == "/file.txt":
            c.sendall(b"gopher text\r\n")
        elif sel.startswith("/search\t"):
            c.sendall(("iyou searched: %s\t\terror.host\t1\r\n.\r\n" % sel.split("\t", 1)[1]).encode())
        else:
            c.sendall(b"3not found\t\terror.host\t1\r\n.\r\n")
    finally:
        c.close()

def serve(port, handler):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(8)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handler, args=(c,), daemon=True).start()

threading.Thread(target=serve, args=(7070, gopher_conn), daemon=True).start()
serve(8081, http_conn)
