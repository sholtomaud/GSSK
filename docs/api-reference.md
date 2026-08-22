# API Reference

## C API (`include/gssk.h`)

All functions return a `GSSK_Status` code unless noted otherwise. Status codes:

| Code | Value | Meaning |
|------|-------|---------|
| `GSSK_SUCCESS` | 0 | Operation succeeded |
| `GSSK_ERR_INVALID_JSON` | 1 | JSON parse failure |
| `GSSK_ERR_MALLOC_FAILED` | 2 | Memory allocation failure |
| `GSSK_ERR_SCHEMA_VIOLATION` | 3 | Model violates schema constraints |
| `GSSK_ERR_DIVERGENCE` | 4 | NaN or Inf detected in state |
| `GSSK_ERR_NOT_FOUND` | 5 | Named element does not exist |
| `GSSK_ERR_UNSUPPORTED_SCHEMA` | 6 | Schema version not supported |
| `GSSK_ERR_UNKNOWN` | 7 | Unexpected internal error |
| `GSSK_WARN_SOLVER_DIVERGENCE` | 8 | Adaptive solver could not converge (non-fatal) |

### Lifecycle

```c
GSSK_Status GSSK_Init(const char *json_str, GSSK_Instance **out);
void        GSSK_Free(GSSK_Instance *inst);
void        GSSK_Reset(GSSK_Instance *inst);
```

`GSSK_Init` parses the JSON model, allocates the kernel, and writes the instance pointer to `*out`. Always call `GSSK_Free` when done.

### Time Stepping

```c
GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt);
GSSK_Status GSSK_StepAdaptive(GSSK_Instance *inst);
```

### State Access

```c
const double *GSSK_GetState(GSSK_Instance *inst);
int           GSSK_GetStateSize(GSSK_Instance *inst);
double        GSSK_GetCurrentTime(GSSK_Instance *inst);
double        GSSK_GetTStart(GSSK_Instance *inst);
double        GSSK_GetTEnd(GSSK_Instance *inst);
double        GSSK_GetDt(GSSK_Instance *inst);
int           GSSK_GetStepCount(GSSK_Instance *inst);
double        GSSK_GetConservationError(GSSK_Instance *inst);
```

### Node / Edge Access

```c
const char *GSSK_GetNodeID(GSSK_Instance *inst, int index);
int         GSSK_FindNodeIdx(GSSK_Instance *inst, const char *id);
const char *GSSK_GetEdgeID(GSSK_Instance *inst, int index);
int         GSSK_FindEdgeIdx(GSSK_Instance *inst, const char *id);
int         GSSK_GetEdgeCount(GSSK_Instance *inst);
double      GSSK_GetEdgeK(GSSK_Instance *inst, int index);
void        GSSK_SetEdgeK(GSSK_Instance *inst, int index, double k);
const char *GSSK_GetNodeCarrier(GSSK_Instance *inst, int nodeIdx);
const char *GSSK_GetEdgeCarrier(GSSK_Instance *inst, int edgeIdx);
```

### Carriers

```c
int                  GSSK_GetCarrierCount(GSSK_Instance *inst);
const GSSK_Carrier  *GSSK_GetCarrier(GSSK_Instance *inst, int idx);
double               GSSK_GetCarrierConservationError(GSSK_Instance *inst, int carrierIdx);

/* Flat accessors — same data, no struct layout */
const char *GSSK_GetCarrierID(GSSK_Instance *inst, size_t idx);
const char *GSSK_GetCarrierUnit(GSSK_Instance *inst, size_t idx);
int         GSSK_GetCarrierConserved(GSSK_Instance *inst, size_t idx);
int         GSSK_FindCarrierIdx(GSSK_Instance *inst, const char *id);
```

`GSSK_Carrier` is a struct with fields `id[32]`, `unit[32]`, and `conserved`
(bool).

**Which to call.** In C, either — `GSSK_GetCarrier` hands back the whole struct
and there is nothing to decode. **Across the WASM boundary, use the flat
getters.** `_GSSK_GetCarrier` returns a bare heap pointer, so reading `unit` or
`conserved` from JS means assuming field offsets, the width of `bool`, and the
absence of trailing padding. None of those are part of any ABI contract, and a
field reorder invalidates all of them by returning plausible garbage rather than
by failing. The flat getters carry no layout across the boundary.

`unit` is the y-axis label a plotting consumer would otherwise hardcode, and
`conserved` together with `unit` is what a consumer needs to decide that two
series may not share a scale (ADR-6, ADR-8).

**Out-of-range conventions differ, deliberately:**

| Call | Out of range |
|---|---|
| `GSSK_GetCarrier` | `NULL` |
| `GSSK_GetCarrierID` / `GSSK_GetCarrierUnit` | `""` (never `NULL`), following `GSSK_GetNodeCarrier` |
| `GSSK_GetCarrierConserved` | `0` — indistinguishable from a declared non-conserved carrier, so bound-check against `GSSK_GetCarrierCount` first if the difference matters |
| `GSSK_FindCarrierIdx` | `-1` when not found, matching `GSSK_FindNodeIdx` / `GSSK_FindEdgeIdx` |

### Sensitivity

```c
GSSK_Status GSSK_EnableForwardSensitivity(GSSK_Instance *inst, const int *edge_indices, int n);
void        GSSK_DisableForwardSensitivity(GSSK_Instance *inst);
double      GSSK_GetSensitivity(GSSK_Instance *inst, int edge_idx, int node_idx);
```

### Mutation Log

```c
int         GSSK_GetMutationCount(GSSK_Instance *inst);
GSSK_Status GSSK_ExportMutationLog(GSSK_Instance *inst, char **out_json);
void        GSSK_ClearMutationLog(GSSK_Instance *inst);
GSSK_Status GSSK_SetMutationCause(GSSK_Instance *inst, const char *cause);
void        GSSK_FreeString(char *str);
```

### Serialisation

```c
GSSK_Status GSSK_SerializeModel(GSSK_Instance *inst, char **out_json);
GSSK_Status GSSK_SerializeSnapshot(GSSK_Instance *inst, char **out_json);
```

Free the returned string with `GSSK_FreeString`.

### Version

```c
const char *GSSK_GetVersionString(GSSK_Instance *inst);
int         GSSK_GetVersionCode(GSSK_Instance *inst);
const char *GSSK_GetModelName(GSSK_Instance *inst);
```

---

## Python API (`python/gssk.py`)

```bash
make shared   # builds lib/libgssk.so / lib/libgssk.dylib
```

### `GSSKSimulator(json_str)`

```python
from python.gssk import GSSKSimulator, from_file

sim = GSSKSimulator(json_str)   # initialise from JSON string
sim = from_file("model.json")   # convenience wrapper
```

| Property / Method | Returns | Description |
|-------------------|---------|-------------|
| `sim.state_size` | `int` | Number of state nodes |
| `sim.start_time` | `float` | Model t_start |
| `sim.end_time` | `float` | Model t_end |
| `sim.default_dt` | `float` | Model dt |
| `sim.current_time` | `float` | Elapsed simulation time |
| `sim.edge_count` | `int` | Number of edges |
| `sim.carrier_count` | `int` | Number of declared carriers |
| `sim.state` | `list[float]` | Current state vector |
| `sim.named_state` | `dict[str, float]` | State keyed by node ID |
| `sim.model_name` | `str` | `metadata.name` from JSON |
| `sim.step(dt=None)` | `list[float]` | Advance one step |
| `sim.run(dt=None)` | `list[list[float]]` | Run full simulation |
| `sim.run_named(dt=None)` | `dict[str, list[float]]` | Run and return by node ID |
| `sim.run_dataframe(dt=None)` | `pd.DataFrame` | Run and return as DataFrame |
| `sim.reset()` | — | Reset to t_start |
| `sim.edge_id(i)` | `str` | Edge ID at index i |
| `sim.edge_k(i)` | `float` | Edge k at index i |
| `sim.set_edge_k(i, k)` | — | Update edge k |
| `sim.find_edge(id)` | `int` | Index of named edge, or -1 |
| `sim.find_node(id)` | `int` | Index of named node, or -1 |
| `sim.node_carrier(i)` | `str` | Carrier ID of node i |
| `sim.edge_carrier(i)` | `str` | Carrier ID of edge i |
| `sim.carrier_conservation_error(i)` | `float` | Per-carrier conservation error |
| `sim.enable_forward_sensitivity(edge_indices)` | — | Enable tangent-linear sensitivity |
| `sim.disable_forward_sensitivity()` | — | Disable sensitivity |
| `sim.get_sensitivity(edge_idx, node_idx)` | `float` | ∂Q_node / ∂k_edge |
| `sim.serialize_model()` | `str` | JSON model (no snapshot) |
| `sim.serialize_snapshot()` | `str` | JSON model + current state |
| `GSSKSimulator.kernel_version()` | `str` | Kernel version string |

**Exceptions**: `GSSKError`, `GSSKSchemaError`, `GSSKDivergenceError`.

---

## JavaScript / TypeScript API (`js/gssk.js`)

After building WASM (`make wasm`):

```js
import { GSSKSimulator, loadModule, Status } from './js/gssk.js';
```

### `GSSKSimulator.create(jsonStr, opts?)`

```js
const sim = await GSSKSimulator.create(jsonStr, {
  wasmPath: './dist/gssk.wasm',   // browser URL or Node.js path
  // moduleFactory: createGSSK,   // inject pre-loaded factory
});
```

| Property / Method | Returns | Description |
|-------------------|---------|-------------|
| `sim.stateSize` | `number` | Number of state nodes |
| `sim.currentTime` | `number` | Elapsed simulation time |
| `sim.startTime` | `number` | Model t_start |
| `sim.endTime` | `number` | Model t_end |
| `sim.defaultDt` | `number` | Model dt |
| `sim.stepCount` | `number` | Steps taken |
| `sim.edgeCount` | `number` | Number of edges |
| `sim.carrierCount` | `number` | Number of declared carriers |
| `sim.conservationError` | `number` | Global conservation error |
| `sim.state` | `Float64Array` | Current state (copy) |
| `sim.namedState` | `object` | State keyed by node ID |
| `sim.nodeManifest` | `Map<number,string>` | index → node ID |
| `sim.modelName` | `string` | `metadata.name` |
| `sim.step(dt?)` | `Float64Array` | Advance one step |
| `sim.stepAdaptive()` | `Float64Array` | Adaptive step |
| `sim.run(dt?)` | `Float64Array[]` | Run full simulation |
| `sim.runNamed(dt?)` | `object` | Run and return by node ID |
| `sim.reset()` | — | Reset to t_start |
| `sim.edgeID(i)` | `string\|null` | Edge ID at index i |
| `sim.edgeK(i)` | `number` | Edge k at index i |
| `sim.setEdgeK(i, k)` | — | Update edge k |
| `sim.edgeCarrier(i)` | `string` | Edge carrier ID |
| `sim.nodeCarrier(i)` | `string` | Node carrier ID |
| `sim.findEdge(id)` | `number` | Index or -1 |
| `sim.findNode(id)` | `number` | Index or -1 |
| `sim.carrierID(i)` | `string` | Carrier id at index i |
| `sim.carrierUnit(i)` | `string` | Carrier unit at index i — the y-axis label |
| `sim.carrierConserved(i)` | `boolean` | Whether carrier i was declared conserved |
| `sim.carriers` | `{id,unit,conserved}[]` | All declared carriers |
| `sim.findCarrier(id)` | `number` | Index of named carrier, or -1 |
| `sim.carrierConservationError(i)` | `number` | Per-carrier error |
| `sim.serializeModel()` | `string` | JSON model |
| `sim.serializeSnapshot()` | `string` | JSON model + snapshot |
| `sim.mutationCount` | `number` | Logged mutations |
| `sim.exportMutationLog()` | `string` | JSON mutation array |
| `sim.clearMutationLog()` | — | Clear log |
| `sim.free()` | — | Release WASM memory |

---

## Swift API (`Sources/GSSK/GSSK.swift`)

```swift
import GSSK

let sim = try GSSKSimulator(json: jsonString)
```

| Property / Method | Returns | Description |
|-------------------|---------|-------------|
| `sim.stateSize` | `Int` | Number of state nodes |
| `sim.currentTime` | `Double` | Elapsed simulation time |
| `sim.startTime` | `Double` | Model t_start |
| `sim.endTime` | `Double` | Model t_end |
| `sim.defaultDt` | `Double` | Model dt |
| `sim.stepCount` | `Int` | Steps taken |
| `sim.edgeCount` | `Int` | Number of edges |
| `sim.carrierCount` | `Int` | Number of declared carriers |
| `sim.conservationError` | `Double` | Global conservation error |
| `sim.state` | `[Double]` | Current state vector |
| `sim.namedState` | `[String: Double]` | State keyed by node ID |
| `sim.modelName` | `String` | `metadata.name` |
| `sim.carriers` | `[GSSKCarrier]` | All carrier definitions |
| `sim.carrier(at:)` | `GSSKCarrier?` | Carrier by index |
| `sim.nodeCarrier(at:)` | `String` | Node carrier ID |
| `sim.edgeCarrier(at:)` | `String` | Edge carrier ID |
| `sim.carrierConservationError(for:)` | `Double` | Per-carrier error |
| `sim.step(dt:)` | `[Double]` | Advance one step |
| `sim.run(dt:)` | `[[Double]]` | Run full simulation |
| `sim.runNamed(dt:)` | `[String: [Double]]` | Run by node ID |
| `sim.reset()` | — | Reset to t_start |
| `sim.edgeID(at:)` | `String?` | Edge ID at index |
| `sim.edgeK(at:)` | `Double` | Edge k at index |
| `sim.setEdgeK(at:value:)` | — | Update edge k |
| `sim.findEdge(id:)` | `Int` | Index or -1 |
| `sim.findNode(id:)` | `Int` | Index or -1 |
| `sim.serializeModel()` | `String` | JSON model |
| `sim.serializeSnapshot()` | `String` | JSON model + snapshot |
| `GSSKSimulator.kernelVersion` | `String` | Kernel version string |

`GSSKCarrier` is a struct with `id: String`, `unit: String`, `conserved: Bool`.

Throws `GSSKError` on init failure; throws `GSSKDivergenceError` on NaN/Inf in step.
