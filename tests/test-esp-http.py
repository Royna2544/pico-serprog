#!/usr/bin/env python3
"""Independent HTTP wire tests against the mock's upstream HTTP engine."""
import argparse
import base64
import hashlib
import http.client
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="esp-http-") as root:
        Path(root, "page.html").write_text("<b>static</b>")
        Path(root, "binary.bin").write_bytes(bytes(range(256)))
        Path(root, "escape").symlink_to("/etc")
        process = subprocess.Popen([str(args.binary.resolve()), "http", root],
                                   stdout=subprocess.PIPE, text=True)
        try:
            assert select.select([process.stdout], [], [], 10)[0], "server startup timeout"
            port = int(process.stdout.readline())

            def request(path, method="GET", body=None, headers=None, connection=None):
                client = connection or http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                client.request(method, path, body, headers or {})
                response = client.getresponse()
                result = response.status, dict(response.getheaders()), response.read()
                if connection is None:
                    client.close()
                return result

            status, headers, body = request("/")
            assert status == 200 and body == b"<h1>mock</h1>"
            assert headers["Access-Control-Allow-Origin"] == "*"
            assert request("/", "HEAD")[2] == b""
            assert request("/missing")[0] == 404
            assert request("/args?name=A%20B&value=a%2Bb+z")[2].startswith(b"A B|a+b z|")
            assert request("/items/42")[2] == b"42"
            assert request("/glob/a/b")[2] == b"glob"
            assert request("/regex/123")[2] == b"123"
            assert request("/custom")[2] == b"custom"
            assert request("/hook")[2] == b"hook"
            assert request("/stream")[2] == b"streamed"
            for method in ["POST", "PUT", "PATCH", "DELETE"]:
                payload = b"binary\x00body\xff"
                status, headers, body = request("/api", method, payload,
                    {"Content-Type": "application/octet-stream", "X-Test": method})
                assert status == 200 and body == payload and headers["X-Echo"] == method, (method, status, headers, body)
            assert request("/api", "OPTIONS")[0] == 200
            assert request("/form", "POST", "name=x+y&value=a%26b",
                {"Content-Type": "application/x-www-form-urlencoded"})[2] == b"x y|a&b"
            assert request("/auth")[0] == 401
            auth = base64.b64encode(b"user:pw").decode()
            assert request("/auth", headers={"Authorization": "Basic " + auth})[2] == b"authorized"
            assert request("/auth", headers={"Authorization": "Basic wrong"})[0] == 401
            status, headers, _ = request("/digest")
            assert status == 401
            challenge = dict(re.findall(r'(\w+)="([^"]*)"', headers["WWW-Authenticate"]))
            md5 = lambda value: hashlib.md5(value.encode()).hexdigest()
            nonce, realm, opaque = (challenge[k] for k in ("nonce", "realm", "opaque"))
            nc, cnonce = "00000001", "host-test"
            response = md5(md5("user:" + realm + ":pw") + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + md5("GET:/digest"))
            digest = f'Digest username="user", realm="{realm}", nonce="{nonce}", uri="/digest", response="{response}", opaque="{opaque}", qop=auth, nc={nc}, cnonce="{cnonce}"'
            assert request("/digest", headers={"Authorization": digest})[2] == b"authorized"
            status, headers, body = request("/chunked")
            assert status == 200 and headers["Transfer-Encoding"] == "chunked" and body == b"firstsecond"
            payload = bytes(range(256)) * 29 + b"\r\n--not-the-boundary\x00tail"
            boundary = "host-upload-boundary"
            form = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="data.bin"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode()
                    + payload + f"\r\n--{boundary}--\r\n".encode())
            status, headers, body = request("/upload", "POST", form,
                {"Content-Type": "multipart/form-data; boundary=" + boundary})
            assert status == 200 and body == payload and headers["X-Filename"] == "data.bin"
            assert headers["X-Upload-Events"] == "1,1"
            status, headers, body = request("/static/page.html")
            assert status == 200 and body == b"<b>static</b>" and "ETag" in headers
            assert request("/static/page.html", headers={"If-None-Match": headers["ETag"]})[0] == 304
            assert request("/static/binary.bin")[2] == bytes(range(256))
            assert request("/static/page.html", "HEAD")[2] == b""
            assert request("/static/../outside")[0] == 404
            assert request("/static/escape/passwd")[0] == 404
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            assert request("/", connection=connection)[0] == 200
            local_port = connection.sock.getsockname()[1]
            assert request("/items/7", connection=connection)[2] == b"7"
            assert connection.sock.getsockname()[1] == local_port
            connection.close()
            with socket.create_connection(("127.0.0.1", port), timeout=5) as drip:
                for value in b"GET /items/drip HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n":
                    drip.sendall(bytes([value]))
                    time.sleep(0.001)
                response = http.client.HTTPResponse(drip)
                response.begin()
                assert response.status == 200 and response.read() == b"drip"
            assert request("/quit")[2] == b"bye"
            assert process.wait(timeout=5) == 0
            print("PASS HTTP routes, REST verbs, binary bodies, forms, uploads, auth, chunks, files, keep-alive, fragmented requests")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
