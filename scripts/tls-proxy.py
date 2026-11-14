#!/usr/bin/env python3
# A man in the middle for TLS 1.3 negative tests (scripts/tls-negative.sh).
# Each listening port tampers with the server's records in one way; the
# client (tls-test in Bhairav OS) must refuse every one of them.
#
#   tls-proxy.py UPSTREAM_PORT
#
#   8454 flip-hello      one bit of the ServerHello's random changed
#   8455 flip-encrypted  one bit of the first encrypted record changed
#   8456 retype          the first encrypted record relabelled "handshake"
#   8457 inject          a plaintext handshake record after ServerHello
#   8458 truncate        the server's last record (close_notify) dropped
#   8459 pass            nothing changed (the control)
import socket
import sys
import threading

MODES = {8454: "flip-hello", 8455: "flip-encrypted", 8456: "retype",
         8457: "inject", 8458: "truncate", 8459: "pass"}
UPSTREAM = ("127.0.0.1", int(sys.argv[1]))
HANDSHAKE, APPLICATION_DATA = 22, 23


def count_client_records(client, upstream, seen):
    # Like pump, but counts the client's encrypted records: its Finished,
    # then its request. Only after both may "truncate" start holding back.
    buffer = b""
    try:
        while True:
            data = client.recv(65536)
            if not data:
                break
            upstream.sendall(data)
            buffer += data
            while len(buffer) >= 5 and len(buffer) >= 5 + int.from_bytes(buffer[3:5], "big"):
                n = 5 + int.from_bytes(buffer[3:5], "big")
                if buffer[0] == APPLICATION_DATA:
                    seen[0] += 1
                buffer = buffer[n:]
    except OSError:
        pass
    try:
        upstream.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def handle(client, mode):
    upstream = socket.create_connection(UPSTREAM)
    client_encrypted = [0]
    threading.Thread(target=count_client_records, args=(client, upstream, client_encrypted), daemon=True).start()
    buffer = b""
    hello_seen = encrypted_seen = False
    held = None
    try:
        while True:
            data = upstream.recv(65536)
            if not data:
                break
            buffer += data
            out = b""
            while len(buffer) >= 5 and len(buffer) >= 5 + int.from_bytes(buffer[3:5], "big"):
                n = 5 + int.from_bytes(buffer[3:5], "big")
                record, buffer = bytearray(buffer[:n]), buffer[n:]
                if record[0] == HANDSHAKE and not hello_seen:
                    hello_seen = True
                    if mode == "flip-hello":
                        record[5 + 4 + 2 + 10] ^= 1  # header, handshake header, version, then the random
                    if mode == "inject":  # an "EncryptedExtensions", unencrypted
                        record += bytes([HANDSHAKE, 3, 3, 0, 6, 8, 0, 0, 2, 0, 0])
                elif record[0] == APPLICATION_DATA and not encrypted_seen:
                    encrypted_seen = True
                    if mode == "flip-encrypted":
                        record[5 + (len(record) - 5) // 2] ^= 1
                    if mode == "retype":
                        record[0] = HANDSHAKE
                if mode == "truncate" and client_encrypted[0] >= 2:
                    # The response: always one record behind, so the last
                    # (close_notify) never goes out.
                    if held is not None:
                        out += held
                    held = bytes(record)
                    continue
                out += bytes(record)
            if out:
                client.sendall(out)
    except OSError:
        pass
    client.close()  # (truncate: without the server's close_notify)
    upstream.close()


def listen(port, mode):
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", port))
    server.listen(8)
    while True:
        client, _ = server.accept()
        threading.Thread(target=handle, args=(client, mode), daemon=True).start()


for port, mode in MODES.items():
    threading.Thread(target=listen, args=(port, mode), daemon=True).start()
threading.Event().wait()
