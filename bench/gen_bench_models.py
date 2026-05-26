#!/usr/bin/env python3
"""Generate bench/bench_N.json benchmark models with N storage nodes in a chain."""
import json
import sys
import os

def make_chain(n: int, t_end: float = 100.0, dt: float = 0.1) -> dict:
    """N-node linear decay chain: source → store_0 → store_1 → … → sink."""
    nodes = [
        {"id": "src",  "type": "source",  "value": 100.0},
    ]
    for i in range(n):
        nodes.append({"id": f"s{i}", "type": "storage", "value": float(10 * (n - i))})
    nodes.append({"id": "sink", "type": "sink", "value": 0.0})

    edges = [
        {"id": "e_src",  "origin": "src",  "target": "s0",   "logic": "constant", "params": {"k": 1.0}},
    ]
    for i in range(n - 1):
        edges.append({
            "id": f"e{i}",
            "origin": f"s{i}",
            "target": f"s{i+1}",
            "logic": "linear",
            "params": {"k": 0.05},
        })
    edges.append({
        "id": f"e{n-1}",
        "origin": f"s{n-1}",
        "target": "sink",
        "logic": "linear",
        "params": {"k": 0.05},
    })

    return {
        "metadata": {
            "schema_version": 3,
            "name": f"Bench {n}",
            "description": f"Linear chain benchmark with {n} storage nodes.",
            "kernel_version": "3.0.0",
        },
        "nodes": nodes,
        "edges": edges,
        "config": {"t_start": 0.0, "t_end": t_end, "dt": dt, "method": "rk4"},
    }


if __name__ == "__main__":
    out_dir = os.path.join(os.path.dirname(__file__))
    for n in [10, 100, 500]:
        model = make_chain(n)
        path = os.path.join(out_dir, f"bench_{n}.json")
        with open(path, "w") as f:
            json.dump(model, f, indent=2)
        print(f"Wrote {path}")
