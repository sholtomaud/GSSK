# Cookbook

Practical recipes for common GSSK tasks.

## Run a model from JSON (C)

```c
#include "gssk.h"
#include <stdio.h>

int main(void) {
    const char *json = "{ ... }"; // your model JSON
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        fprintf(stderr, "Init failed: %s\n", GSSK_GetErrorDescription(inst));
        GSSK_Free(inst);
        return 1;
    }

    double dt = GSSK_GetDt(inst);
    while (GSSK_GetCurrentTime(inst) < GSSK_GetTEnd(inst) - 1e-12) {
        st = GSSK_Step(inst, dt);
        if (st == GSSK_ERR_DIVERGENCE) { /* handle */ break; }
        const double *state = GSSK_GetState(inst);
        printf("t=%.2f  Q0=%.4f\n", GSSK_GetCurrentTime(inst), state[0]);
    }

    GSSK_Free(inst);
    return 0;
}
```

## Load a model from a JSON file (Python)

```python
from python.gssk import from_file

sim = from_file("examples/decay_model.json")
results = sim.run()            # list of state snapshots
df = sim.run_dataframe()       # pandas DataFrame (requires pandas)
print(df.tail())
```

## Parametric sweep over edge k (Python)

```python
from python.gssk import GSSKSimulator
import json

with open("examples/decay_model.json") as f:
    json_str = f.read()

for k in [0.01, 0.05, 0.1, 0.5]:
    sim = GSSKSimulator(json_str)
    idx = sim.find_edge("resp")
    sim.set_edge_k(idx, k)
    results = sim.run()
    final_biomass = results[-1][0]
    print(f"k={k:.2f}  biomass(t_end)={final_biomass:.2f}")
```

## Resume from a snapshot (Python)

```python
sim1 = from_file("examples/decay_model.json")
for _ in range(10):
    sim1.step()

snap = sim1.serialize_snapshot()   # JSON with "snapshot" block

sim2 = GSSKSimulator(snap)         # continues from t=5.0
sim2.run()
```

## Forward sensitivity analysis (Python)

```python
sim = from_file("examples/decay_model.json")
edge_idx = sim.find_edge("resp")
sim.enable_forward_sensitivity([edge_idx])

for _ in range(40):
    sim.step()

# ∂Q_biomass / ∂k_resp
node_idx = sim.find_node("biomass")
dQ_dk = sim.get_sensitivity(edge_idx, node_idx)
print(f"Sensitivity: {dQ_dk:.4f}")
```

## Multi-carrier model in JavaScript

```js
import { GSSKSimulator } from './js/gssk.js';

const json = await fetch('examples/household_model.json').then(r => r.text());
const sim  = await GSSKSimulator.create(json, { wasmPath: 'dist/gssk.wasm' });

console.log('Carriers:', sim.carrierCount);

const named = sim.runNamed();
console.log('Bank balance over time:', named['bank_account']);

sim.free();
```

## Inspect carriers and conservation (C)

```c
int n = GSSK_GetCarrierCount(inst);
for (int i = 0; i < n; i++) {
    const GSSK_Carrier *c = GSSK_GetCarrier(inst, i);
    printf("Carrier %s (%s) conserved=%d\n", c->id, c->unit, c->conserved);
}

GSSK_Step(inst, dt);
for (int i = 0; i < n; i++) {
    double err = GSSK_GetCarrierConservationError(inst, i);
    printf("  conservation error[%d] = %.6e\n", i, err);
}
```

## Export and replay mutation log (C)

```c
// Make some structural changes
GSSK_SetMutationCause(inst, "scenario A: add solar panel");
GSSK_AddNode(inst, "solar", "source", 0.0, "energy");

// Export the log
char *log_json = NULL;
GSSK_ExportMutationLog(inst, &log_json);
printf("%s\n", log_json);
GSSK_FreeString(log_json);
```

## Adaptive stepping for stiff models (Swift)

```swift
let sim = try GSSKSimulator(json: jsonString)
while sim.currentTime < sim.endTime - 1e-12 {
    _ = try sim.stepAdaptive()
}
let final = sim.state
```

## Build and run tests

```bash
make               # build native library and CLI
make test          # regression suite against expected CSVs
make test-update   # regenerate expected CSVs after model changes
make test-python   # Python ctypes binding tests
swift test         # Swift binding tests
make test-asan     # re-run regression under AddressSanitizer
make coverage-check  # lcov coverage gate (≥85%)
```

## Benchmark

```bash
make bench         # print timing for all benchmark scenarios
make bench-check   # fail if slowest scenario exceeds BENCH_BASELINE_MS (default 500 ms)
BENCH_BASELINE_MS=200 make bench-check   # tighter gate
```

## Build WASM module

```bash
# Requires emscripten (emcc)
make wasm          # produces dist/gssk.js + dist/gssk.wasm
```

Serve `dist/` alongside your web app and import via `<script type="module">` or a bundler.
