import random
import csv
import time

# Mirrors the kernel's buddy allocator logic to generate realistic trace data
# Output format matches /proc/mymalloc/trace exactly

POOL_ORDER = 10
NR_ORDERS = POOL_ORDER + 1
TRACE_PATH = "/home/parallels/mymalloc/ml/train_data.csv"
N_EVENTS = 5000

def compute_frag_score(free_lists):
    for o in range(POOL_ORDER, -1, -1):
        if free_lists[o] > 0:
            return ((POOL_ORDER - o) * 100) // POOL_ORDER
    return 100

def generate_trace(n_events):
    free_lists = [0] * NR_ORDERS
    free_lists[POOL_ORDER] = 1
    allocated = {}  # addr -> order
    next_addr = 0x1000

    rows = []
    ts = 1_000_000_000

    for _ in range(n_events):
        ts += random.randint(100_000, 2_000_000)

        can_alloc = any(free_lists[o] > 0 for o in range(NR_ORDERS))
        can_free = len(allocated) > 0

        if can_free and (not can_alloc or random.random() < 0.45):
            # free a random allocation
            addr = random.choice(list(allocated.keys()))
            order = allocated.pop(addr)
            # coalesce upward
            while order < POOL_ORDER:
                if free_lists[order] > 0:
                    free_lists[order] -= 1
                    order += 1
                else:
                    break
            free_lists[order] += 1
            op = "buddy_free"
        else:
            # alloc at a random order
            order = random.randint(0, 6)
            # find smallest available order >= requested
            split_order = None
            for o in range(order, NR_ORDERS):
                if free_lists[o] > 0:
                    split_order = o
                    break
            if split_order is None:
                continue
            # split down
            free_lists[split_order] -= 1
            while split_order > order:
                split_order -= 1
                free_lists[split_order] += 1
            addr = next_addr
            next_addr += (1 << order) * 4096
            allocated[addr] = order
            op = "buddy_alloc"

        frag = compute_frag_score(free_lists)
        row = [ts, op, order, hex(addr)] + list(free_lists) + [frag]
        rows.append(row)

    return rows

header = (["timestamp_ns", "op", "order", "addr"] +
          [f"free{i}" for i in range(NR_ORDERS)] +
          ["frag_score_pct"])

rows = generate_trace(N_EVENTS)

with open(TRACE_PATH, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(header)
    w.writerows(rows)

print(f"Generated {len(rows)} events -> {TRACE_PATH}")

# Quick sanity check
scores = [r[-1] for r in rows]
print(f"Frag score range: {min(scores)}% - {max(scores)}%")
print(f"Unique frag scores: {sorted(set(scores))}")