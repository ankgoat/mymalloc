import torch
import numpy as np
import pickle
import os

# ── Must match train_lstm.py exactly ─────────────────────────────────────────
MODEL_PATH  = "/home/parallels/mymalloc/ml/lstm_model.pt"
SCALER_PATH = "/home/parallels/mymalloc/ml/scaler.pkl"
WINDOW      = 16
HIDDEN_SIZE = 64
NUM_LAYERS  = 2

OP_MAP = {"buddy_alloc": 0, "buddy_free": 1, "slab_alloc": 2, "slab_free": 3}

# ── Paste model definition here (must match train_lstm.py) ───────────────────
import torch.nn as nn

class Attention(nn.Module):
    def __init__(self, hidden_size):
        super().__init__()
        self.attn = nn.Linear(hidden_size, 1)

    def forward(self, lstm_out):
        scores  = self.attn(lstm_out)
        weights = torch.softmax(scores, dim=1)
        context = (weights * lstm_out).sum(dim=1)
        return context, weights

class FragLSTM(nn.Module):
    def __init__(self, input_size, hidden_size, num_layers):
        super().__init__()
        self.lstm = nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            dropout=0.2
        )
        self.attention = Attention(hidden_size)
        self.regressor = nn.Sequential(
            nn.Linear(hidden_size, 32),
            nn.ReLU(),
            nn.Linear(32, 1)
        )

    def forward(self, x):
        lstm_out, _ = self.lstm(x)
        context, attn_weights = self.attention(lstm_out)
        out = self.regressor(context)
        return out, attn_weights

# ── Inference engine ──────────────────────────────────────────────────────────
INPUT_SIZE = 14   # op_enc, order, free0..free10, frag_score_pct

class FragPredictor:
    def __init__(self):
        with open(SCALER_PATH, "rb") as f:
            self.scaler = pickle.load(f)

        self.model = FragLSTM(INPUT_SIZE, HIDDEN_SIZE, NUM_LAYERS)
        self.model.load_state_dict(torch.load(MODEL_PATH, weights_only=True))
        self.model.eval()

        # Ring buffer of recent events
        self.window = []
        print("[infer] Model loaded. Ready.")

    def push_event(self, event: dict):
        """
        Feed one telemetry event. Returns (predicted_frag_pct, hint) once
        the window is full, otherwise returns (None, None).

        event keys: op, order, free0..free10, frag_score_pct
        """
        row = self._encode(event)
        self.window.append(row)
        if len(self.window) > WINDOW:
            self.window.pop(0)

        if len(self.window) < WINDOW:
            return None, None

        return self._predict()

    def _encode(self, event):
        op_enc = OP_MAP.get(event.get("op", "buddy_alloc"), 0)
        order  = float(event.get("order", 0))
        frees  = [float(event.get(f"free{i}", 0)) for i in range(11)]
        frag   = float(event.get("frag_score_pct", 0))
        return [op_enc, order] + frees + [frag]

    def _predict(self):
        import time
        t0 = time.perf_counter()

        arr = np.array(self.window, dtype=np.float32)           # (16, 14)
        arr_scaled = self.scaler.transform(arr)                 # normalise
        tensor = torch.tensor(arr_scaled).unsqueeze(0)          # (1, 16, 14)

        with torch.no_grad():
            pred_scaled, _ = self.model(tensor)

        # Un-scale: frag_score_pct is the last feature column
        dummy = np.zeros((1, INPUT_SIZE), dtype=np.float32)
        dummy[0, -1] = pred_scaled.item()
        pred_pct = self.scaler.inverse_transform(dummy)[0, -1]
        pred_pct = float(np.clip(pred_pct, 0, 100))

        latency_us = (time.perf_counter() - t0) * 1e6

        # Map to hint for kernel
        current_frag = self.window[-1][-1]   # raw (pre-scale) last frag
        if pred_pct > current_frag + 5:
            hint = "PRESPLIT"
        elif pred_pct < current_frag - 5:
            hint = "PRECOALESCE"
        else:
            hint = "NOOP"

        return {
            "predicted_frag_pct": round(pred_pct, 2),
            "current_frag_pct":   current_frag,
            "hint":               hint,
            "latency_us":         round(latency_us, 2),
        }, hint


# ── Quick self-test ───────────────────────────────────────────────────────────
if __name__ == "__main__":
    predictor = FragPredictor()

    # Feed 20 synthetic events and print predictions
    import random
    ops = ["buddy_alloc", "buddy_free"]
    frag = 50.0

    for i in range(20):
        op    = random.choice(ops)
        order = random.randint(0, 5)
        frag  = max(0, min(100, frag + random.randint(-10, 10)))
        frees = [random.randint(0, 3) for _ in range(11)]

        event = {"op": op, "order": order, "frag_score_pct": frag,
                 **{f"free{j}": frees[j] for j in range(11)}}

        result, hint = predictor.push_event(event)
        if result:
            print(f"event {i+1:>2} | frag={frag:5.1f}% | "
                  f"pred={result['predicted_frag_pct']:5.2f}% | "
                  f"hint={result['hint']:<12} | "
                  f"latency={result['latency_us']:.1f}µs")
        else:
            print(f"event {i+1:>2} | warming up window... ({len(predictor.window)}/{WINDOW})")