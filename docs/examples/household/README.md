# Household Ecological-Economy

A complete walkthrough of the multi-carrier household model introduced in Phase 5.
Source file: [`examples/household_model.json`](../../../examples/household_model.json)
Annotated source: [`examples/household_model_annotated.json`](../../../examples/household_model_annotated.json)

---

## Why a Household?

A household is one of the smallest self-contained ecological-economic systems.
It imports money (salary), energy (sunlight, fuel), material (food, goods), and
information (news, decisions), and exports waste, heat, and tax.
Every one of Odum's five flow logic types appears naturally:

| Logic | Example edge |
|-------|-------------|
| CONSTANT | Salary paid each month — fixed rate regardless of account balance |
| LINEAR | Battery self-discharge — proportional to charge stored |
| INTERACTION | Groceries purchased — rate depends on *both* bank balance and market |
| LIMIT (Michaelis-Menten) | Food eaten — saturates when fridge is full |
| THRESHOLD | Credit card minimum payment — only fires when debt > AUD 500 |

This makes the household model the canonical integration test for the kernel.

---

## Carriers

The model declares four carriers in the top-level `carriers` array:

```json
"carriers": [
  { "id": "money",       "unit": "AUD",             "conserved": true  },
  { "id": "energy",      "unit": "kWh",             "conserved": true  },
  { "id": "material",    "unit": "kg",              "conserved": true  },
  { "id": "information", "unit": "decisions/month", "conserved": false }
]
```

`conserved: true` tells the kernel to track the relative change in total
storage-Q for that carrier each step via `GSSK_GetCarrierConservationError`.
Information is marked `conserved: false` because attention and decisions are
dissipated (forgotten), not physically conserved.

---

## Node Inventory

### Money carrier

| Node | Type | Initial value | Role |
|------|------|---------------|------|
| `salary` | source | 1.0 | Normalised unit; edge k=5000 AUD/month |
| `bank_account` | storage | 5 000 AUD | Current account |
| `super_fund` | storage | 50 000 AUD | Superannuation savings |
| `credit_card_debt` | storage | 0 AUD | Revolving credit balance |
| `tax` | sink | 0 AUD | Government / mortgage payments |

### Energy carrier

| Node | Type | Initial value | Role |
|------|------|---------------|------|
| `sunlight` | source | 1.0 | Normalised; `quality_input: 1.0` for emergy |
| `solar_battery` | storage | 5 kWh | Rooftop battery |
| `grid_credit` | storage | 0 kWh | Feed-in credit accumulated |
| `vehicle_fuel` | storage | 40 kWh | Petrol/LPG in vehicle tank |
| `body_energy` | storage | 350 kWh | Metabolic energy reserve |
| `heat_loss` | sink | 0 kWh | Irreversible thermodynamic loss |

### Material carrier

| Node | Type | Initial value | Role |
|------|------|---------------|------|
| `grocery_market` | source | 1.0 | Normalised market supply |
| `mains_water` | source | 1.0 | Municipal water supply |
| `pantry` | storage | 10 kg | Dry goods and non-perishables |
| `fridge` | storage | 5 kg | Chilled food |
| `wardrobe` | storage | 50 kg | Clothing stock |
| `appliances_stock` | storage | 200 kg | Household durables |
| `waste_bin` | storage | 0 kg | Intermediate waste accumulator |
| `waste_landfill` | sink | 0 kg | Final waste destination |

### Information carrier

| Node | Type | Initial value | Role |
|------|------|---------------|------|
| `news_inflow` | source | 1.0 | Normalised; `quality_input` not set |
| `household_attention` | storage | 100 decisions/month | Cognitive bandwidth |
| `pending_decisions` | storage | 5 decisions/month | Outstanding choices |
| `forgotten_decisions` | sink | 0 | Decisions lost / unresolved |

---

## Edge Logic Walkthrough

### CONSTANT edges

```json
{ "id": "salary_in",  "origin": "salary",  "target": "bank_account",
  "carrier": "money", "logic": "constant", "params": { "k": 5000.0 } }
```

The kernel computes `F = k`, so 5 000 AUD/month flows into `bank_account`
regardless of its current balance. This is appropriate for a fixed salary.

### LINEAR edges

```json
{ "id": "grocery_payment", "origin": "bank_account", "target": "tax",
  "carrier": "money", "logic": "linear", "params": { "k": 0.1 } }
```

`F = k × Q_origin` = 10 % of the current account balance per month.
Represents discretionary spending proportional to perceived wealth — a
standard Keynesian consumption function in miniature.

### INTERACTION edge — cross-carrier coupling

```json
{ "id": "grocery_receive",
  "origin": "grocery_market", "target": "pantry",
  "carrier": "material", "logic": "interaction",
  "params": { "k": 0.001, "control_node": "bank_account" } }
```

`F = k × Q_origin × Q_control` — grocery flow is the *product* of market
supply and bank balance. This is Odum's "work gate": money acts as the
controlling signal that modulates material flow. When the account is flush
the pantry fills quickly; when it is empty, groceries stop regardless of
market supply.

**This is a cross-carrier interaction**: `carrier: "material"` but
`control_node` (bank_account) belongs to the `"money"` carrier. The kernel
enforces no cross-carrier restriction — the Odum four-position encoding
handles this naturally (Position 1 is the code/carrier; Position 3 is the
flow; control is a force from any domain).

### LIMIT edge — saturation

```json
{ "id": "fridge_eat",
  "origin": "fridge", "target": "body_energy",
  "carrier": "material", "logic": "limit",
  "params": { "k": 2.0, "threshold": 3.0 } }
```

Michaelis-Menten kinetics: `F = k × Q / (1 + Q/C)` where C = `threshold` = 3 kg.
Eating saturates — you cannot eat faster than your stomach allows even when
the fridge is overflowing. The kernel accepts either `threshold` (used as C)
or `control_node` (whose Q is used as C) for LIMIT edges.

At `Q_fridge = 5 kg`: `F = 2 × 5 / (1 + 5/3) ≈ 3.75 kg/month`.
At `Q_fridge = 0.5 kg`: `F = 2 × 0.5 / (1 + 0.5/3) ≈ 0.86 kg/month`.

### THRESHOLD edge

```json
{ "id": "credit_payment",
  "origin": "credit_card_debt", "target": "bank_account",
  "carrier": "money", "logic": "threshold",
  "params": { "k": 300.0, "threshold": 500.0 } }
```

`F = k` when `Q_origin > threshold`, else `F = 0`.
A minimum credit card repayment of AUD 300/month only activates when the
balance exceeds AUD 500. The kernel's Illinois-algorithm event detector
locates the exact crossing time within each step.

---

## Emergy Accounting

Two source nodes declare `quality_input: 1.0`:

```json
{ "id": "salary",  "type": "source", "quality_input": 1.0, "carrier": "money"  }
{ "id": "sunlight","type": "source", "quality_input": 1.0, "carrier": "energy" }
```

After each `GSSK_Step`, the kernel propagates transformation ratios (Tr)
through the flow network. A storage node's Tr is the quality-weighted sum of
its inflows divided by the total flow. This gives a partial emergy accounting:

- `solar_battery.Tr` reflects the solar origin of stored energy.
- `body_energy.Tr` is elevated if the fridge draw has high transformity.
- `bank_account.Tr` traces the labour-transformity of the salary.

See `GSSK_GetTransformationRatio` and `GSSK_GetEdgeQualityFlow` for the API.

---

## Running the Model

**CLI:**
```bash
make all
make demo
```

The output CSV has one column per node and one row per time step (dt = 0.1 month,
t_end = 24 months → 240 data rows + header).

**Swift:**
```swift
let json = try String(contentsOfFile: "examples/household_model.json")
let sim  = try GSSKSimulator(json: json)

// Carrier metadata
print(sim.carrierCount)          // 4
print(sim.carrier(at: 0)?.id)   // "money"
print(sim.nodeCarrier(at: 1))    // "bank_account" → "money"

// Run and read results
let results = try sim.runNamed()
print(results["bank_account"]!.last!)   // ~24 095 AUD at t=24
```

---

## Steady-State Analysis

At constant salary (`k_salary = 5000 AUD/month`) with linear spending
(`k_spend = 0.1`) and constant outflows (mortgage 1800, tax 600):

`dQ_bank/dt = 5000 − 0.1 × Q_bank − 1800 − 600`

The fixed point is found by setting `dQ/dt = 0`:

```
Q_bank* = (5000 − 1800 − 600) / 0.1 = 26 000 AUD
```

From initial conditions Q₀ = 5 000 AUD, the system approaches this
asymptote with time constant `τ = 1/k_spend = 10 months`.
By t = 24 months the model reaches ≈ 24 095 AUD, still short of the
theoretical steady-state — consistent with `τ = 10` months.

The analytical solution is:
```
Q_bank(t) = Q* + (Q₀ − Q*) × exp(−t/τ)
           = 26000 + (5000 − 26000) × exp(−t/10)
```

At t = 24: `26000 − 21000 × exp(−2.4) ≈ 24 084 AUD` (matches CSV to < 0.1%).

---

## Cross-Carrier Sensitivity

The grocery_receive interaction edge links money to material:

`F_grocery = k × Q_market × Q_bank = 0.001 × 1 × Q_bank`

Doubling `k_grocery` from 0.001 → 0.002 doubles the material inflow rate.
At steady state, pantry equilibrium satisfies:

`0 = F_in − k_spoil × Q_pantry − k_ptof × Q_pantry`
`Q_pantry* = F_in / (k_spoil + k_ptof) = (k_grocery × Q_bank*) / (0.03 + 0.2)`

With `k_grocery = 0.001`: `Q_pantry* ≈ 0.001 × 26000 / 0.23 ≈ 113 kg`
With `k_grocery = 0.002`: `Q_pantry* ≈ 0.002 × 26000 / 0.23 ≈ 226 kg`

A 2× increase in grocery spend rate produces a 2× pantry steady-state,
as expected from the linear scaling of the interaction term. See
`examples/household_notebook.ipynb` for a simulation-validated version.

---

## Files

| File | Description |
|------|-------------|
| `examples/household_model.json` | Canonical model (used by `make test`) |
| `examples/household_model_annotated.json` | Same model with `_note` fields on each node and edge |
| `examples/household_notebook.ipynb` | Jupyter notebook: run, plot, sensitivity analysis |
| [Interactive WASM Demo](/GSSK/demo/) | Browser demo with live simulation and interactive sliders |
