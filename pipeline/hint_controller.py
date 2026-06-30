#!/usr/bin/env python3
"""
hint_controller.py — Phase 11

Receives ML predictions from ml_server.py over a Unix domain socket and
issues real MYMALLOC_IOC_HINT ioctl calls to /dev/mymalloc.

A/B testing:
    MYMALLOC_HINTS_ENABLED=1   (default)  → ioctl call is made
    MYMALLOC_HINTS_ENABLED=0              → ioctl call is skipped (baseline)

Run order from the README:
    Terminal 1: python3 ~/mymalloc/pipeline/ml_server.py
    Terminal 2: python3 ~/mymalloc/pipeline/hint_controller.py
    Terminal 3: python3 ~/mymalloc/pipeline/dashboard.py
    Terminal 4: python3 ~/mymalloc/pipeline/collector_dashboard.py
"""

import fcntl
import json
import os
import socket
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEVICE_PATH       = "/dev/mymalloc"
HINT_SOCKET_PATH  = "/tmp/mymalloc_hints.sock"   # ml_server.py publishes hints here
HINTS_ENABLED     = os.environ.get("MYMALLOC_HINTS_ENABLED", "1") == "1"

# ---------------------------------------------------------------------------
# ioctl number computation — must match the C _IOW macro exactly
#
#   #define MYMALLOC_IOC_MAGIC 0xBB
#   #define MYMALLOC_IOC_HINT  _IOW(MYMALLOC_IOC_MAGIC, 5, struct mymalloc_hint_arg)
#
#   struct mymalloc_hint_arg { unsigned int action; unsigned int order; };
#     → size = 8 bytes
#
#   _IOC(dir, type, nr, size) = (dir << 30) | (size << 16) | (type << 8) | nr
#   _IOW = direction bits = 0b01 → 0x40000000
# ---------------------------------------------------------------------------

_IOC_WRITE         = 0x40000000
MYMALLOC_IOC_MAGIC = 0xBB
HINT_CMD_NR        = 5
HINT_STRUCT_SIZE   = 8   # two unsigned ints

MYMALLOC_IOC_HINT = (
    _IOC_WRITE
    | (HINT_STRUCT_SIZE << 16)
    | (MYMALLOC_IOC_MAGIC << 8)
    | HINT_CMD_NR
)

ACTION_MAP = {
    "NOOP":        0,
    "PRESPLIT":    1,
    "PRECOALESCE": 2,
}

# ---------------------------------------------------------------------------
# Device handle
# ---------------------------------------------------------------------------

device_fd = None

def open_device():
    """Open /dev/mymalloc once at startup. Returns fd or None on failure."""
    global device_fd
    if not HINTS_ENABLED:
        print("[hint_ctrl] MYMALLOC_HINTS_ENABLED=0 — baseline mode, device NOT opened")
        return None
    try:
        device_fd = os.open(DEVICE_PATH, os.O_RDWR)
        print(f"[hint_ctrl] opened {DEVICE_PATH} fd={device_fd}")
        print(f"[hint_ctrl] MYMALLOC_IOC_HINT = 0x{MYMALLOC_IOC_HINT:08x}")
        return device_fd
    except OSError as e:
        print(f"[hint_ctrl] FATAL: cannot open {DEVICE_PATH}: {e}")
        print("[hint_ctrl] is the module loaded? did you mknod /dev/mymalloc?")
        sys.exit(1)

# ---------------------------------------------------------------------------
# ioctl helpers
# ---------------------------------------------------------------------------

# Counters for reporting
sent_counts = {"PRESPLIT": 0, "PRECOALESCE": 0, "NOOP": 0}
skipped     = 0
failed      = 0

def send_ioctl_hint(action_str, order):
    """Pack and send a hint ioctl. action_str ∈ {PRESPLIT, PRECOALESCE, NOOP}."""
    global failed, skipped

    action_int = ACTION_MAP.get(action_str, 0)

    # Always log; only ioctl if hints enabled and device opened.
    if not HINTS_ENABLED or device_fd is None:
        skipped += 1
        print(f"[hint_ctrl] (skipped — baseline) action={action_str} order={order}")
        return

    packed = struct.pack("II", action_int, int(order))
    try:
        fcntl.ioctl(device_fd, MYMALLOC_IOC_HINT, packed)
        sent_counts[action_str] = sent_counts.get(action_str, 0) + 1
        print(f"[hint_ctrl] → ioctl HINT action={action_str}({action_int}) order={order}")
    except OSError as e:
        failed += 1
        print(f"[hint_ctrl] ioctl FAILED action={action_str} order={order}: {e}")

# ---------------------------------------------------------------------------
# Socket loop — connect to ml_server.py and read JSON hints
# ---------------------------------------------------------------------------

def connect_to_ml_server(retries=10, delay=0.5):
    """Connect to the ml_server hint socket with retries."""
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
    """Yield JSON-decoded messages from a stream-oriented socket, one per newline."""
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
        print(f"[hint_ctrl]   {action:12s} sent: {n}")
    print(f"[hint_ctrl]   skipped (baseline): {skipped}")
    print(f"[hint_ctrl]   ioctl failures:     {failed}")
    print("[hint_ctrl] ===========================")

def main():
    print(f"[hint_ctrl] Phase 11 hint controller starting")
    print(f"[hint_ctrl] hints enabled: {HINTS_ENABLED}")

    open_device()
    sock = connect_to_ml_server()

    try:
        for msg in read_jsonl(sock):
            # Expected message shape from ml_server.py:
            #   { "pred": 42.7, "actual": 39.5, "hint": "PRESPLIT", "order": 3, ... }
            action = msg.get("hint", "NOOP")
            order  = msg.get("order", 0)
            send_ioctl_hint(action, order)
    except KeyboardInterrupt:
        print("\n[hint_ctrl] interrupted")
    finally:
        if device_fd is not None:
            os.close(device_fd)
        sock.close()
        print_summary()

if __name__ == "__main__":
    main()