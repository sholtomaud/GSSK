"""python/plot_demo.py — Generate demo PNG plots from GSSK simulation output.

Reads the CSVs written by `make demo` and produces dist/demo.png.
Requires matplotlib (pip install matplotlib).
"""
import csv
import math
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
except ImportError:
    print("ERROR: matplotlib is required for plot-demo. Install it with:")
    print("  pip install matplotlib")
    sys.exit(1)

DECAY_CSV     = "/tmp/gssk_demo_decay.csv"
HOUSEHOLD_CSV = "/tmp/gssk_demo_household.csv"
OUT_PNG       = "dist/demo.png"


def read_csv(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    headers = list(rows[0].keys())
    data = {h: [float(r[h]) for r in rows] for h in headers}
    return headers, data


def main():
    for path in (DECAY_CSV, HOUSEHOLD_CSV):
        if not os.path.exists(path):
            print(f"ERROR: {path} not found — run 'make demo' first.")
            sys.exit(1)

    decay_headers,     decay     = read_csv(DECAY_CSV)
    household_headers, household = read_csv(HOUSEHOLD_CSV)

    fig = plt.figure(figsize=(14, 10))
    fig.suptitle("GSSK Kernel — Demo Output", fontsize=14, fontweight="bold")
    gs = gridspec.GridSpec(2, 2, figure=fig, hspace=0.45, wspace=0.35)

    # ── Panel 1: Decay model — biomass vs analytical ──────────────────────────
    ax1 = fig.add_subplot(gs[0, 0])
    t = decay["time"]
    ax1.plot(t, decay["biomass"],     label="biomass (RK4)",  color="steelblue", lw=2)
    ax1.plot(t, decay["environment"], label="environment",     color="darkorange", lw=2)
    analytical = [100.0 * math.exp(-0.05 * ti) for ti in t]
    ax1.plot(t, analytical, "k--", lw=1, alpha=0.6, label="analytical exp(−0.05t)")
    ax1.set_title("Decay model")
    ax1.set_xlabel("time")
    ax1.set_ylabel("Q")
    ax1.legend(fontsize=8)
    ax1.grid(True, alpha=0.3)

    # ── Panel 2: Decay — RK4 vs analytical error ─────────────────────────────
    ax2 = fig.add_subplot(gs[0, 1])
    error = [abs(b - a) for b, a in zip(decay["biomass"], analytical)]
    ax2.semilogy(t, error, color="crimson", lw=1.5)
    ax2.set_title("Decay — RK4 absolute error")
    ax2.set_xlabel("time")
    ax2.set_ylabel("|RK4 − analytical|")
    ax2.grid(True, alpha=0.3, which="both")

    # ── Panel 3: Household — money carrier ───────────────────────────────────
    ax3 = fig.add_subplot(gs[1, 0])
    money_nodes = ["bank_account", "super_fund", "credit_card_debt", "tax"]
    for node in money_nodes:
        if node in household:
            ax3.plot(household["time"], household[node], label=node, lw=1.5)
    ax3.set_title("Household — money carrier (AUD)")
    ax3.set_xlabel("time")
    ax3.set_ylabel("Q (AUD)")
    ax3.legend(fontsize=8)
    ax3.grid(True, alpha=0.3)

    # ── Panel 4: Household — energy + material carriers ──────────────────────
    ax4 = fig.add_subplot(gs[1, 1])
    energy_nodes   = ["solar_battery", "body_energy", "vehicle_fuel"]
    material_nodes = ["pantry", "fridge"]
    for node in energy_nodes:
        if node in household:
            ax4.plot(household["time"], household[node], label=f"{node} (kWh)", lw=1.5)
    for node in material_nodes:
        if node in household:
            ax4.plot(household["time"], household[node], "--", label=f"{node} (kg)", lw=1.5)
    ax4.set_title("Household — energy & material carriers")
    ax4.set_xlabel("time")
    ax4.set_ylabel("Q")
    ax4.legend(fontsize=8)
    ax4.grid(True, alpha=0.3)

    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    fig.savefig(OUT_PNG, dpi=120, bbox_inches="tight")
    print(f"Plot saved to {OUT_PNG}")


if __name__ == "__main__":
    main()
