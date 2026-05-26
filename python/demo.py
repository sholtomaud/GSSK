"""python/demo.py — Quick demo of the GSSK Python ctypes binding."""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from python.gssk import from_file

# ── Decay model ───────────────────────────────────────────────────────────────
print("=== Decay model (biomass → environment, k=0.05, RK4) ===")
sim = from_file("examples/decay_model.json")
print(f"  Model:   {sim.model_name}")
print(f"  Kernel:  {sim.kernel_version()}")
print(f"  Nodes:   {sim.state_size}   Edges: {sim.edge_count}")
print(f"  t: {sim.start_time} → {sim.end_time}  dt={sim.default_dt}")

named = sim.run_named()
final = named["biomass"][-1]
analytical = 100.0 * math.exp(-0.05 * sim.end_time)
print(f"\n  Final biomass:  {final:.6f}")
print(f"  Analytical:     {analytical:.6f}")
print(f"  Error:          {abs(final - analytical):.2e}")

# ── Household model ───────────────────────────────────────────────────────────
print()
print("=== Household model (4-carrier ecological-economy) ===")
sim2 = from_file("examples/household_model.json")
print(f"  Model:    {sim2.model_name}")
print(f"  Nodes:    {sim2.state_size}   Edges: {sim2.edge_count}   Carriers: {sim2.carrier_count}")

results = sim2.run()
print(f"  Steps:    {len(results)}")

ns = sim2.named_state
print(f"\n  Final state snapshot:")
for name, q in list(ns.items())[:8]:
    print(f"    {name:<25} {q:>12.4f}")
if len(ns) > 8:
    print(f"    ... ({len(ns) - 8} more nodes)")
