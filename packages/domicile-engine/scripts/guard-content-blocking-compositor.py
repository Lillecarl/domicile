#!/usr/bin/env python3
"""The compositor's end of the control socket, for guard-content-blocking.sh.

It answers the browser's `hello` with a `welcome` and one `content_blocking`
message, the one it was given as JSON without its type, as the real
compositor sends it at the handshake from a desk's `content_blocking`
(`domicile-protocol`'s `wire/host-messages.jsonl` spells it).

It says `sent the content blocking` once the line is written.
"""

import argparse
import json
import os
import socket
import sys


# What a desk without content_blocking sends.
OFF = {
    "enabled": False,
    "lists": [],
    "resources": "",
    "filters": [],
    "mode": "standard",
    "sites": {},
    "electron_apps": False,
}


def send(connection, message):
    line = json.dumps(message) + "\n"
    connection.sendall(line.encode("utf-8"))
    print("sent: %s" % line.strip(), flush=True)


def serve(path, blocking):
    if os.path.exists(path):
        os.unlink(path)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(4)
    print("listening on %s" % path, flush=True)
    while True:
        connection, _ = listener.accept()
        print("a channel connected", flush=True)
        with connection:
            remainder = b""
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                remainder += chunk
                while b"\n" in remainder:
                    line, remainder = remainder.split(b"\n", 1)
                    # The real compositor says nothing before `hello`.
                    if json.loads(line).get("type") == "hello":
                        send(connection,
                             {"type": "welcome", "protocol_version": 1})
                        send(connection,
                             dict({"type": "content_blocking"}, **blocking))
                        print("sent the content blocking", flush=True)
        print("the channel went away", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="path to bind")
    parser.add_argument(
        "--content-blocking",
        default=json.dumps(OFF),
        help="the message's JSON without its type; blocking off by default",
    )
    arguments = parser.parse_args()
    try:
        serve(arguments.socket, json.loads(arguments.content_blocking))
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
