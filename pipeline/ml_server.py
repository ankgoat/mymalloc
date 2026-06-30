#!/usr/bin/env python3
import socket
import json
import os
import sys
import time

sys.path.insert(0, "/home/parallels/mymalloc/ml")
from infer import FragPredictor

COLLECTOR_SOCKET = "/tmp/mymalloc_collector.sock"
HINT_SOCKET      = "/tmp/mymalloc_hints.sock"

predictor = None

metrics = {
    "total_events": 0, "total_hints": 0, "presplit_count": 0,
    "precoalesce_count": 0, "noop_count": 0, "mae_sum": 0.0,
    "mae_count": 0, "last_predicted": None,
}

def send_hint(hint_conn, predicted_order, action):
    hint = {"predicted_order": predicted_order, "action": action}
    try:
        hint_conn.sendall((json.dumps(hint) + "\n").encode())
        print(f"[ml_server] hint sent: {action} order={predicted_order}")
    except Exception as e:
        print(f"[ml_server] hint send failed: {e}")

def ml_inference(event):
    global predictor, metrics
    result, hint = predictor.push_event(event)
    metrics["total_events"] += 1
    if result is None:
        return None, None
    pred_pct   = result["predicted_frag_pct"]
    actual_pct = float(event.get("frag_score_pct", 0))
    latency_us = result["latency_us"]
    if metrics["last_predicted"] is not None:
        metrics["mae_sum"]   += abs(metrics["last_predicted"] - actual_pct)
        metrics["mae_count"] += 1
    metrics["last_predicted"] = pred_pct
    if hint == "PRESPLIT":
        metrics["presplit_count"] += 1
    elif hint == "PRECOALESCE":
        metrics["precoalesce_count"] += 1
    else:
        metrics["noop_count"] += 1
    if hint != "NOOP":
        metrics["total_hints"] += 1
    mae = metrics["mae_sum"]/metrics["mae_count"] if metrics["mae_count"] else 0.0
    print(f"[ml_server] pred={pred_pct:.1f}% actual={actual_pct:.0f}% "
          f"hint={hint} latency={latency_us:.1f}us MAE={mae:.2f}%")
    order = int(event.get("order", 0))
    return order, (hint if hint != "NOOP" else None)

def handle_collector(conn, hint_conn):
    print("[ml_server] collector connected")
    buf = ""
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
                    event = json.loads(line)
                    predicted_order, action = ml_inference(event)
                    if action and hint_conn:
                        send_hint(hint_conn, predicted_order, action)
                except json.JSONDecodeError as e:
                    print(f"[ml_server] bad JSON: {e}")
    except Exception as e:
        print(f"[ml_server] error: {e}")
    finally:
        conn.close()
        print("[ml_server] collector disconnected")
        mae = metrics["mae_sum"]/metrics["mae_count"] if metrics["mae_count"] else 0.0
        print("\n[ml_server] -- Session Summary --")
        print(f"[ml_server]   Total events:     {metrics['total_events']}")
        print(f"[ml_server]   Total hints sent: {metrics['total_hints']}")
        print(f"[ml_server]   PRESPLIT:         {metrics['presplit_count']}")
        print(f"[ml_server]   PRECOALESCE:      {metrics['precoalesce_count']}")
        print(f"[ml_server]   NOOP:             {metrics['noop_count']}")
        print(f"[ml_server]   MAE:              {mae:.2f}%")

def run():
    global predictor
    print("[ml_server] Loading LSTM+Attention model...")
    predictor = FragPredictor()
    print("[ml_server] Model ready.")
    for path in [COLLECTOR_SOCKET, HINT_SOCKET]:
        if os.path.exists(path):
            os.unlink(path)
    hint_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    hint_server.bind(HINT_SOCKET)
    hint_server.listen(1)
    hint_server.settimeout(30)
    col_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    col_server.bind(COLLECTOR_SOCKET)
    col_server.listen(1)
    print(f"[ml_server] ready -- waiting for hint_controller on {HINT_SOCKET}")
    print(f"[ml_server] ready -- waiting for collector on {COLLECTOR_SOCKET}")
    try:
        hint_conn, _ = hint_server.accept()
        print("[ml_server] hint_controller connected")
    except socket.timeout:
        print("[ml_server] WARNING: hint_controller never connected")
        hint_conn = None
    col_conn, _ = col_server.accept()
    handle_collector(col_conn, hint_conn)
    if hint_conn:
        hint_conn.close()
    col_server.close()
    hint_server.close()
    for path in [COLLECTOR_SOCKET, HINT_SOCKET]:
        if os.path.exists(path):
            os.unlink(path)

if __name__ == "__main__":
    run()
