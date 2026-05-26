# GSSK Python Binding

Pure-Python ctypes binding for the GSSK simulation kernel.
No build step beyond compiling the shared C library.

## Setup

```bash
# From the repository root
make shared          # builds lib/libgssk.so (Linux) or lib/libgssk.dylib (macOS)
```

## Usage

```python
from python.gssk import GSSKSimulator, from_file

# From a JSON string
import json
with open("examples/decay_model.json") as f:
    sim = GSSKSimulator(f.read())

# Or use the helper
sim = from_file("examples/decay_model.json")

# Properties
print(sim.model_name)      # "Decay Test"
print(sim.kernel_version()) # "3.0.0"
print(sim.state_size)      # 2

# Single step
s = sim.step()             # [Q_biomass, Q_environment]

# Full run
results = sim.run()        # list of state snapshots

# Named results
named = sim.run_named()    # {"biomass": [...], "environment": [...]}

# pandas DataFrame (requires pandas)
df = sim.run_dataframe()
print(df.head())
```

## Multi-carrier

```python
sim = from_file("examples/household_model.json")
print(sim.carrier_count)              # 4
print(sim.node_carrier(1))            # "money"  (bank_account)
print(sim.carrier_conservation_error(0))  # error for carrier 0 after step
```

## Sensitivity

```python
sim = from_file("examples/decay_model.json")
sim.enable_forward_sensitivity([0])   # track edge 0 (respiration k)
for _ in range(40):
    sim.step()
dQ_dk = sim.get_sensitivity(0, 0)    # ∂Q_biomass/∂k_respiration
```

## Serialisation (round-trip)

```python
sim = from_file("examples/decay_model.json")
for _ in range(10): sim.step()
snap = sim.serialize_snapshot()       # JSON string with snapshot block
sim2 = GSSKSimulator(snap)            # resumes from t=5.0
```

## Status

This binding targets the Phase 6.2 milestone.
When `gsk-py` ships on PyPI the import will change to `import gssk`,
with the same API surface.
