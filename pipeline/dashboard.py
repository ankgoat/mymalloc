#!/usr/bin/env python3
"""
dashboard.py — Phase 9
Subscribes to the collector socket stream and renders a live
terminal dashboard showing fragmentation score, per-order free
counts, and recent events.
"""

import socket
import json
import os
import time
import sys
from collections import deque

SOCKET_PATH  = "/tmp/mymalloc_dashboard.sock"
HISTORY_SIZE = 50   # how many frag scores to keep for the graph

# ANSI colour helpers
def cls():       print("\033[2J\033[H", end="")
def bold(s):     return f"\033[1m{s}\033[0m"
def green(s):    return f"\033[32m{s}\033[0m"
def yellow(s):   return f"\033[33m{s}\033[0m"
def red(s):      return f"\033[31m{s}\033[0m"
def cyan(s):     return f"\033[36m{s}\033[0m"

def score_colour(score):
    if score < 30:   return green(f"{score:3d}%")
    elif score < 70: return yellow(f"{score:3d}%")
    else:            return red(f"{score:3d}%")

def draw_bar(count, max_count, width=20):
    if max_count == 0:
        filled = 0
    else:
        filled = int((count / max_count) * width)
    bar = "█" * filled + "░" * (width - filled)
    return bar

def draw_graph(history, width=50, height=8):
    """Draw a simple ASCII line graph of frag score over time."""
    lines = []
    if not history:
        return ["  (no data yet)"]
    max_val = 100
    for row in range(height, 0, -1):
        threshold = (row / height) * max_val
        line = ""
        # Only show last `width` data points
        data = list(history)[-width:]
        for val in data:
            if val >= threshold:
                line += "█"
            else:
                line += " "
        label = f"{int(threshold):3d}%|"
        lines.append(label + line)
    lines.append("    +" + "-" * min(len(list(history)), width))
    return lines

def render(events, frag_history, free_counts, latest_score):
    cls()
    print(bold("═" * 60))
    print(bold("  mymalloc — Phase 9 Live Dashboard"))
    print(bold("═" * 60))

    # Fragmentation score
    print(f"\n  {bold('Fragmentation Score:')}  {score_colour(latest_score)}")
    print()

    # Graph
    print(bold("  Score History:"))
    for line in draw_graph(frag_history):
        print("  " + line)
    print()

    # Per-order free counts bar chart
    print(bold("  Free Blocks per Order:"))
    max_count = max(free_counts) if free_counts else 1
    for o, count in enumerate(free_counts):
        kb = (4 << o)
        bar = draw_bar(count, max(max_count, 1))
        print(f"  order {o:2d} ({kb:5d} KB)  {bar}  {count}")
    print()

    # Recent events
    print(bold("  Recent Events:"))
    print(f"  {'timestamp':>16}  {'op':<14}  {'order':>5}  {'frag':>4}")
    print("  " + "-" * 48)
    for ev in list(events)[-10:]:
        ts   = ev.get("timestamp_ns", 0)
        op   = ev.get("op", "?")
        ord_ = ev.get("order", "?")
        sc   = ev.get("frag_score_pct", 0)
        print(f"  {ts:>16}  {op:<14}  {ord_:>5}  {score_colour(sc)}")

    print(f"\n  {bold('Total events seen:')} {len(frag_history)}")
    print(bold("═" * 60))
    print("  Press Ctrl+C to exit")
    sys.stdout.flush()

def run():
    # Clean up stale socket
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(1)
    print(f"[dashboard] waiting for collector on {SOCKET_PATH}")
    print("[dashboard] start collector_dashboard.py to connect")

    conn, _ = server.accept()
    print("[dashboard] collector connected — rendering dashboard")

    frag_history = deque(maxlen=HISTORY_SIZE)
    free_counts  = [0] * 11
    events       = deque(maxlen=100)
    latest_score = 0
    buf          = ""

    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data.decode()
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                    events.append(ev)
                    latest_score = ev.get("frag_score_pct", 0)
                    frag_history.append(latest_score)
                    # Update free counts from event
                    for o in range(11):
                        key = f"free{o}"
                        if key in ev:
                            free_counts[o] = ev[key]
                    render(events, frag_history, free_counts, latest_score)
                except json.JSONDecodeError:
                    pass
    except KeyboardInterrupt:
        print("\n[dashboard] stopped")
    finally:
        conn.close()
        server.close()
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)

if __name__ == "__main__":
    run()