import pandas as pd
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from sklearn.preprocessing import MinMaxScaler
import pickle
import os

# ── Config ────────────────────────────────────────────────────────────────────
DATA_PATH    = "/home/parallels/mymalloc/ml/train_data.csv"
MODEL_PATH   = "/home/parallels/mymalloc/ml/lstm_model.pt"
SCALER_PATH  = "/home/parallels/mymalloc/ml/scaler.pkl"
WINDOW       = 16       # how many past events the model sees
HIDDEN_SIZE  = 64
NUM_LAYERS   = 2
EPOCHS       = 50
BATCH_SIZE   = 32
LR           = 1e-3
VAL_SPLIT    = 0.2

# ── Feature engineering ───────────────────────────────────────────────────────
OP_MAP = {"buddy_alloc": 0, "buddy_free": 1, "slab_alloc": 2, "slab_free": 3}

def load_features(path):
    df = pd.read_csv(path)

    # Encode op as integer
    df["op_enc"] = df["op"].map(OP_MAP).fillna(0).astype(int)

    # Feature columns: op_enc, order, free0..free10, frag_score_pct
    feat_cols = ["op_enc", "order"] + [f"free{i}" for i in range(11)] + ["frag_score_pct"]
    data = df[feat_cols].values.astype(np.float32)

    return data

def make_windows(data, window):
    X, y = [], []
    for i in range(len(data) - window):
        X.append(data[i:i+window])          # window of events
        y.append(data[i+window, -1])        # next frag_score_pct
    return np.array(X, dtype=np.float32), np.array(y, dtype=np.float32)

# ── Dataset ───────────────────────────────────────────────────────────────────
class FragDataset(Dataset):
    def __init__(self, X, y):
        self.X = torch.tensor(X)
        self.y = torch.tensor(y).unsqueeze(1)   # (N, 1)

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]

# ── Model: LSTM + Attention ───────────────────────────────────────────────────
class Attention(nn.Module):
    """Scaled dot-product attention over LSTM hidden states."""
    def __init__(self, hidden_size):
        super().__init__()
        self.attn = nn.Linear(hidden_size, 1)

    def forward(self, lstm_out):
        # lstm_out: (batch, seq, hidden)
        scores = self.attn(lstm_out)            # (batch, seq, 1)
        weights = torch.softmax(scores, dim=1)  # (batch, seq, 1)
        context = (weights * lstm_out).sum(dim=1)  # (batch, hidden)
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
        lstm_out, _ = self.lstm(x)              # (batch, seq, hidden)
        context, attn_weights = self.attention(lstm_out)
        out = self.regressor(context)           # (batch, 1)
        return out, attn_weights

# ── Training ──────────────────────────────────────────────────────────────────
def train():
    print("Loading data...")
    data = load_features(DATA_PATH)

    # Scale all features to [0, 1]
    scaler = MinMaxScaler()
    data_scaled = scaler.fit_transform(data)

    X, y = make_windows(data_scaled, WINDOW)
    print(f"  Windows: {len(X)}  |  Features per step: {X.shape[2]}")

    # Train / val split (time-ordered — no shuffle)
    split = int(len(X) * (1 - VAL_SPLIT))
    X_train, X_val = X[:split], X[split:]
    y_train, y_val = y[:split], y[split:]

    train_loader = DataLoader(FragDataset(X_train, y_train), batch_size=BATCH_SIZE, shuffle=True)
    val_loader   = DataLoader(FragDataset(X_val,   y_val),   batch_size=BATCH_SIZE)

    model     = FragLSTM(input_size=X.shape[2], hidden_size=HIDDEN_SIZE, num_layers=NUM_LAYERS)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.MSELoss()

    best_val_loss = float("inf")
    print(f"\nTraining LSTM+Attention for {EPOCHS} epochs...")
    print(f"{'Epoch':>6}  {'Train Loss':>12}  {'Val Loss':>10}  {'Val MAE (%)':>12}")
    print("-" * 48)

    for epoch in range(1, EPOCHS + 1):
        # ── train
        model.train()
        train_loss = 0.0
        for xb, yb in train_loader:
            optimizer.zero_grad()
            pred, _ = model(xb)
            loss = criterion(pred, yb)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            train_loss += loss.item() * len(xb)
        train_loss /= len(X_train)

        # ── validate
        model.eval()
        val_loss = 0.0
        mae_sum  = 0.0
        with torch.no_grad():
            for xb, yb in val_loader:
                pred, _ = model(xb)
                val_loss += criterion(pred, yb).item() * len(xb)
                # un-scale frag score for MAE in real % units
                pred_real = pred.numpy() * 100
                true_real = yb.numpy()  * 100
                mae_sum  += np.abs(pred_real - true_real).sum()
        val_loss /= len(X_val)
        val_mae   = mae_sum / len(X_val)

        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(model.state_dict(), MODEL_PATH)

        if epoch % 5 == 0 or epoch == 1:
            print(f"{epoch:>6}  {train_loss:>12.6f}  {val_loss:>10.6f}  {val_mae:>11.2f}%")

    print(f"\nBest val loss: {best_val_loss:.6f}")
    print(f"Model saved -> {MODEL_PATH}")

    # Save scaler so inference uses identical normalisation
    with open(SCALER_PATH, "wb") as f:
        pickle.dump(scaler, f)
    print(f"Scaler saved -> {SCALER_PATH}")

if __name__ == "__main__":
    train()