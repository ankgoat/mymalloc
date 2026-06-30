#!/usr/bin/env python3
"""
collector_dashboard.py — Phase 9
Reads trace.csv and fans out each event to TWO sockets:
  1. ml_server    (/tmp/mymalloc_collector.sock)
  2. dashboard    (/tmp/mymalloc_dashboard.sock)
"""

import socket
import json
import os
import sys
import time

ML_SOCKET        = "/tmp/mymalloc_collector.sock"
DASHBOARD_SOCKET = "/tmp/mymalloc_dashboard.sock"
TRACE_FILE       = os.path.expanduser("~/mymalloc/ml/train_data.csv")
POLL_INTERVAL    = 0.2

def parse_line(header, line):
    keys   = header.strip().split(",")
    values = line.strip().split(",")
    if len(values) != len(keys):
        return None
    event = {}
    for k, v in zip(keys, values):
        event[k] = int(v) if v.lstrip("-").isdigit() else v
    return event

def connect_to(path, name):
    for _ in range(30):
        if os.path.exists(path):
            break
        print(f"[collector] waiting for {name}...")
        time.sleep(1)
    else:
        print(f"[collector] ERROR: {name} socket never appeared")
        sys.exit(1)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    print(f"[collector] connected to {name}")
    return sock

def run():
    print("[collector] starting fan-out collector")
    ml_sock   = connect_to(ML_SOCKET,        "ml_server")
    dash_sock = connect_to(DASHBOARD_SOCKET, "dashboard")

    seen_lines = 0
    header     = None

    while True:
        try:
            if not os.path.exists(TRACE_FILE):
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
                try:
                    ml_sock.sendall(msg.encode())
                except Exception:
                    pass
                try:
                    dash_sock.sendall(msg.encode())
                except Exception:
                    pass
                print(f"[collector] sent: {event['op']} "
                      f"order={event.get('order','?')} "
                      f"frag={event.get('frag_score_pct','?')}%")
                seen_lines += 1
            time.sleep(POLL_INTERVAL)
        except KeyboardInterrupt:
            print("[collector] stopping")
            break

    ml_sock.close()
    dash_sock.close()

if __name__ == "__main__":
    run()