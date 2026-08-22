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

## Stream results as the simulation runs (C and JS)

Streaming needs **no kernel API beyond `GSSK_Step` and `GSSK_GetState`**, both
of which exist and are both exported to WASM. There is no callback interface, no
`GSSK_Run`-with-sink, and none is needed: you own the loop, so you decide what
happens between steps — write a CSV row, push a WebSocket frame, update a chart,
throttle, or stop early.

### The C loop

```c
GSSK_Instance *inst = NULL;
if (GSSK_Init(json, &inst) != GSSK_SUCCESS) { /* handle */ }

size_t n  = GSSK_GetStateSize(inst);
double dt = GSSK_GetDt(inst);

while (GSSK_GetCurrentTime(inst) < GSSK_GetTEnd(inst) - 1e-12) {
    GSSK_Status st = GSSK_Step(inst, dt);
    if (st == GSSK_ERR_DIVERGENCE) break;      /* NaN/Inf; state is not usable */

    /* Re-fetch each iteration. It costs one load, and it is correct
       unconditionally — see the lifetime rule below. */
    const double *state = GSSK_GetState(inst);
    printf("%.4f", GSSK_GetCurrentTime(inst));
    for (size_t i = 0; i < n; i++) printf(",%.6f", state[i]);
    printf("\n");
}
GSSK_Free(inst);
```

Use `GSSK_StepAdaptive(inst)` instead of `GSSK_Step` when you want the solver to
choose the step size; the reading pattern is identical, and `GSSK_GetLastStepSize`
tells you how far the step actually went.

### Pointer lifetime — the rule that makes "just call GetState" safe

`GSSK_GetState` returns a pointer to the kernel's live state array, not a copy.
So the advice is only safe if the aliasing rules are stated:

| Call | Effect on a previously returned `GSSK_GetState` pointer |
|---|---|
| `GSSK_Step`, `GSSK_StepAdaptive` | **Stays valid.** The step writes through the same array. |
| `GSSK_Reset` | **Stays valid.** Values are rewritten in place. |
| `GSSK_AddEdge` | **Stays valid.** It grows the edge arrays, not the state array. |
| `GSSK_DeactivateEdge`, `GSSK_DeactivateNode` | **Stays valid.** Deactivation flags; it does not shrink the arrays. |
| `GSSK_AddNode` | **INVALIDATES it.** The state array is `realloc`'d, which may relocate it. |
| `GSSK_Free` | Invalidates it, obviously. |

`GSSK_AddNode` is the one that bites, and it bites intermittently: a `realloc`
often returns the same block, so a cached pointer appears to work and then
one-in-several-adds silently starts reading freed memory. In a probe adding 64
nodes to a 2-node model, the array relocated on 16 of the 64 adds.

**So: do not cache the pointer across anything but a step.** Re-fetch it, which
is what the loop above does. A *rejected* `GSSK_AddNode` does not invalidate it
— the validation happens before the `realloc` — but there is no reason to rely
on that.

Note also that `GSSK_GetStateSize` changes after `GSSK_AddNode`. Re-read the
size along with the pointer if your loop mutates topology.

`GSSK_Replay` calls `GSSK_AddNode` internally when the log contains one, but it
builds its own instance and hands it back, so this is not a mid-stream hazard
unless you are streaming from the instance it returned — in which case the same
rule applies to it.

### The JS/WASM loop

Same two calls, one extra wrinkle. `_GSSK_GetState` returns an *offset into the
WASM heap*, and the JS-side typed-array views are objects that can be replaced
when the heap grows. So the WASM version of the aliasing rule is: **re-read
`mod.HEAPF64` as well as the pointer**, and never hold a `Float64Array` view
across a call into the kernel.

```js
import { GSSKSimulator } from './js/gssk.js';

const sim = await GSSKSimulator.create(json, { wasmPath: 'dist/gssk.wasm' });

while (sim.currentTime < sim.endTime - 1e-12) {
  sim.step();
  // `sim.state` re-fetches the pointer, re-reads HEAPF64, and returns a copy.
  onRow(sim.currentTime, sim.state);
}
sim.free();
```

`sim.state` already does the right thing: it calls `_GSSK_GetState` fresh, reads
`this.#mod.HEAPF64.buffer` fresh, and `.slice()`s so you get a copy rather than
a window onto kernel memory. If you drop to the raw module instead of the
binding, do the same three things — the shape to avoid is hoisting
`const heap = mod.HEAPF64` or `const ptr = mod._GSSK_GetState(inst)` out of the
loop.

As currently built, `dist/gssk.wasm` does **not** enable
`ALLOW_MEMORY_GROWTH`, so the heap is fixed and the views are in fact never
replaced today. Do not depend on that: it is a build flag, re-reading costs
nothing, and the pattern above is correct either way.

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

## Using built-in composite node types (Phase 8)

Composite node types expand at parse time into Phase 7 primitives, so a
modeller can reach for Odum's Fig 1.2b symbols (producer, consumer, etc.)
without manually wiring the internals.

```json
{
  "nodes": [
    { "id": "plant", "type": "producer", "value": 100.0,
      "params": { "k_production": 0.01, "k_respiration": 0.03 } },
    { "id": "herbivore", "type": "consumer", "value": 20.0,
      "params": { "k_metabolism": 0.08 } }
  ],
  "edges": [
    { "origin": "plant", "target": "herbivore",
      "logic": "interaction",
      "params": { "k": 0.002, "control_node": "herbivore__body" } }
  ]
}
```

What `producer` expands to:

* `plant__body` — the autocatalytic storage (receives external inflows;
  external `origin: "plant"` and `target: "plant"` edges route here).
* `plant__gate` — the interaction node implementing the self-feedback gate.
* `plant__heat` — the heat-sink for respiration.

What `consumer` expands to:

* `herbivore__body` — the consumer's storage.
* `herbivore__heat` — the metabolism sink.

When you reference an expanded node (e.g. as a `control_node`), use the
namespaced id `{composite_id}__{template_id}`.  When you reference the
composite itself (e.g. `origin: "plant"`), the kernel routes the edge to
the composite's default input/output (here both are `__body`).

The expansion is observable via:

```c
size_t arch_count = GSSK_GetArchetypeCount(inst);     // ≥ 4 built-ins
size_t comp_count = GSSK_GetCompositeCount(inst);     // composites in model
const char *cid   = GSSK_GetCompositeID(inst, 0);     // original id
```

## Defining a custom archetype (Phase 8)

Add a top-level `"archetypes"` object to the model JSON.  Each entry is a
named template whose `nodes`, `edges`, and `ports` define the internal
structure.  Any subsequent `type: "<name>"` declaration expands using
the template, namespaced by the instance id.

```json
{
  "archetypes": {
    "relay": {
      "nodes": [
        { "id": "buf", "type": "storage", "value": 0.0 }
      ],
      "edges": [],
      "ports": { "in": "buf", "out": "buf" }
    }
  },
  "nodes": [
    { "id": "src",  "type": "source", "value": 10.0 },
    { "id": "r",    "type": "relay",  "value":  5.0 },
    { "id": "sink", "type": "sink",   "value":  0.0 }
  ],
  "edges": [
    { "origin": "src", "target": "r",    "logic": "constant", "params": { "k": 1.0 } },
    { "origin": "r",   "target": "sink", "logic": "linear",   "params": { "k": 0.5 } }
  ]
}
```

The `r` composite expands to a single primitive `r__buf` (a storage node)
with default-in and default-out both bound to `buf`.  External edges that
target `r` are redirected to `r__buf`.

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
