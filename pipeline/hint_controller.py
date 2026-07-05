#!/usr/bin/env python3
"""
hint_controller.py -- Phase 11 live bridge, host side.

Receives ML predictions from ml_server.py over the existing Unix domain
socket, and writes each hint as its own file on the 9p-shared directory
that hint_inject.c (running inside the VM) polls and turns into a real
MYMALLOC_IOC_HINT ioctl call.

File-per-hint protocol: each hint is written as ~/mymalloc/share/hint_NNNNN,
containing one line "ACTION ORDER". The VM daemon reads and unlinks each
file, so hints can never fire twice. This replaces the earlier append-log
design that suffered from double-firing across polls on 9p.

A/B testing:
    MYMALLOC_HINTS_ENABLED=1   (default)  -> hints are written to the share
    MYMALLOC_HINTS_ENABLED=0              -> hints are logged but NOT written
                                              (baseline run)

Run order (unchanged from the README):
    Terminal 1: python3 ~/mymalloc/pipeline/ml_server.py
    Terminal 2: python3 ~/mymalloc/pipeline/hint_controller.py
    Terminal 3: python3 ~/mymalloc/pipeline/dashboard.py
    Terminal 4: python3 ~/mymalloc/pipeline/collector_dashboard.py

Before starting this, the VM must be booted with the 9p share mounted
and hint_inject running in the background inside the VM:
    ~ # ./hint_inject &
"""

import json
import os
import socket
import sys
import time

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

HINT_SOCKET_PATH = "/tmp/mymalloc_hints.sock"   # ml_server.py publishes hints here
SHARE_DIR        = os.path.expanduser("~/mymalloc/share")
HINT_PREFIX      = "hint_"                       # file-per-hint protocol
HINTS_ENABLED    = os.environ.get("MYMALLOC_HINTS_ENABLED", "1") == "1"

ACTION_NAMES = {0: "NOOP", 1: "PRESPLIT", 2: "PRECOALESCE"}

# ---------------------------------------------------------------------------
# Share-directory setup
# ---------------------------------------------------------------------------

hint_seq = 0

def prepare_share_dir():
    """Create the share directory and clear any stale hint_* files from a previous run."""
    if not HINTS_ENABLED:
        print("[hint_ctrl] MYMALLOC_HINTS_ENABLED=0 -- baseline mode, no hint files will be written")
        return
    os.makedirs(SHARE_DIR, exist_ok=True)
    stale = [f for f in os.listdir(SHARE_DIR) if f.startswith(HINT_PREFIX)]
    for f in stale:
        try:
            os.remove(os.path.join(SHARE_DIR, f))
        except OSError:
            pass
    if stale:
        print(f"[hint_ctrl] cleared {len(stale)} stale hint file(s) from {SHARE_DIR}")
    print(f"[hint_ctrl] writing hints to {SHARE_DIR}/{HINT_PREFIX}NNNNN")

# ---------------------------------------------------------------------------
# Counters for reporting
# ---------------------------------------------------------------------------

sent_counts = {"PRESPLIT": 0, "PRECOALESCE": 0, "NOOP": 0}
skipped     = 0

def send_hint(action_str, order):
    global skipped, hint_seq
    if not HINTS_ENABLED:
        skipped += 1
        print(f"[hint_ctrl] (skipped -- baseline) action={action_str} order={order}")
        return
    hint_seq += 1
    # Write to a temp name first, then rename atomically so the VM daemon
    # never sees a partially-written hint file.
    fname      = f"{HINT_PREFIX}{hint_seq:05d}"
    tmp_path   = os.path.join(SHARE_DIR, fname + ".tmp")
    final_path = os.path.join(SHARE_DIR, fname)
    with open(tmp_path, "w") as f:
        f.write(f"{action_str} {order}\n")
    os.rename(tmp_path, final_path)
    sent_counts[action_str] = sent_counts.get(action_str, 0) + 1
    print(f"[hint_ctrl] -> wrote {final_path} ({action_str} {order}) "
          f"(sent so far: {sent_counts})")

# ---------------------------------------------------------------------------
# Socket loop -- connect to ml_server.py and read JSON hints
# ---------------------------------------------------------------------------

def connect_to_ml_server(retries=10, delay=0.5):
    for attempt in range(retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(HINT_SOCKET_PATH)
            print(f"[hint_ctrl] connected to {HINT_SOCKET_PATH}")
            return sock
        except (ConnectionRefusedError, FileNotFoundError) as e:
            if attempt == retries - 1:
                print(f"[hint_ctrl] FATAL: cannot connect to {HINT_SOCKET_PATH}: {e}")
                sys.exit(1)
            print(f"[hint_ctrl] waiting for ml_server.py... ({attempt+1}/{retries})")
            time.sleep(delay)

def read_jsonl(sock):
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line.decode("utf-8"))
            except json.JSONDecodeError as e:
                print(f"[hint_ctrl] bad JSON: {e}: {line[:80]!r}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def print_summary():
    print()
    print("[hint_ctrl] ===== session summary =====")
    print(f"[hint_ctrl] hints enabled: {HINTS_ENABLED}")
    for action, n in sent_counts.items():
        print(f"[hint_ctrl]   {action:12s} written: {n}")
    print(f"[hint_ctrl]   skipped (baseline): {skipped}")
    print("[hint_ctrl] ===========================")

def main():
    print("[hint_ctrl] Phase 11 hint controller (file-per-hint 9p bridge) starting")
    print(f"[hint_ctrl] hints enabled: {HINTS_ENABLED}")

    prepare_share_dir()
    sock = connect_to_ml_server()

    try:
        for msg in read_jsonl(sock):
            # ml_server.py message shape (per existing code):
            #   {"predicted_order": <int>, "action": "PRESPLIT"|"PRECOALESCE"|"NOOP"}
            action = msg.get("action", "NOOP")
            order  = msg.get("predicted_order", 0)
            send_hint(action, order)
    except KeyboardInterrupt:
        print("\n[hint_ctrl] interrupted")
    finally:
        sock.close()
        print_summary()

if __name__ == "__main__":
    main()