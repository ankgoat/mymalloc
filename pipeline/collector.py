#!/usr/bin/env python3
import socket
import json
import time
import os
import sys

SOCKET_PATH   = "/tmp/mymalloc_collector.sock"
TRACE_FILE    = os.path.expanduser("~/mymalloc/pipeline/trace.csv")
POLL_INTERVAL = 0.1

def parse_line(header, line):
    keys   = header.strip().split(",")
    values = line.strip().split(",")
    if len(values) != len(keys):
        return None
    event = {}
    for k, v in zip(keys, values):
        event[k] = int(v) if v.lstrip("-").isdigit() else v
    return event

def run():
    print(f"[collector] starting, reading {TRACE_FILE}")
    for _ in range(30):
        if os.path.exists(SOCKET_PATH):
            break
        print("[collector] waiting for ml_server...")
        time.sleep(1)
    else:
        print("[collector] ERROR: ml_server socket never appeared")
        sys.exit(1)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCKET_PATH)
    print("[collector] connected to ml_server")

    seen_lines = 0
    header     = None

    while True:
        try:
            if not os.path.exists(TRACE_FIwhat LE):
                time.sleep(POLL_INTERVAL)
                continue
            with open(TRACE_FILE, "r") as f:
                lines = f.readlines()
            if not lines:
                time.sleep(POLL_INTERVAL)
                continue
            if header is None:
                header     = lines[0]
                seen_lines = 1
            for line in lines[seen_lines:]:
                line = line.strip()
                if not line:
                    continue
                event = parse_line(header, line)
                if event is None:
                    continue
                msg = json.dumps(event) + "\n"
                sock.sendall(msg.encode())
                print(f"[collector] sent: {event['op']} order={event.get('order','?')}")
                seen_lines += 1
            time.sleep(POLL_INTERVAL)
        except BrokenPipeError:
            print("[collector] ml_server disconnected")
            break
        except KeyboardInterrupt:
            print("[collector] stopping")
            break
    sock.close()

if __name__ == "__main__":
    run()