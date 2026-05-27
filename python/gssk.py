"""
gssk.py — Python ctypes binding for the GSSK simulation kernel.

Build the shared library first:
    make shared          # produces lib/libgssk.so (Linux) / lib/libgssk.dylib (macOS)

Usage:
    from gssk import GSSKSimulator

    with open("examples/decay_model.json") as f:
        json_str = f.read()

    sim = GSSKSimulator(json_str)
    print(sim.state)          # [100.0, 0.0]
    sim.step()
    print(sim.state)          # [97.53..., ...]

    # Run to completion, get all states as a list of lists
    results = sim.run()

    # Named state dict
    print(sim.named_state)    # {"biomass": 97.53, "environment": 2.46}

Phase 6.2 note: once `gsk-py` is published via PyPI this module will be
replaced by `import gssk` with the same surface API.
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
from pathlib import Path
from typing import Any

# ── Locate shared library ─────────────────────────────────────────────────────

def _find_lib() -> str:
    """Search for libgssk.so / libgssk.dylib relative to this file or CWD."""
    candidates = [
        Path(__file__).parent.parent / "lib" / "libgssk.so",
        Path(__file__).parent.parent / "lib" / "libgssk.dylib",
        Path(os.getcwd()) / "lib" / "libgssk.so",
        Path(os.getcwd()) / "lib" / "libgssk.dylib",
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    raise FileNotFoundError(
        "libgssk.so / libgssk.dylib not found. Run 'make shared' first.\n"
        f"Searched: {[str(p) for p in candidates]}"
    )


_lib = ctypes.CDLL(_find_lib())

# ── Status codes ──────────────────────────────────────────────────────────────

GSSK_SUCCESS                    = 0
GSSK_ERR_INVALID_JSON           = 1
GSSK_ERR_MALLOC_FAILED          = 2
GSSK_ERR_SCHEMA_VIOLATION       = 3
GSSK_ERR_DIVERGENCE             = 4
GSSK_ERR_NOT_FOUND              = 5
GSSK_ERR_UNSUPPORTED_SCHEMA     = 6
GSSK_ERR_UNKNOWN                = 7
GSSK_WARN_SOLVER_DIVERGENCE     = 8

# ── C function signatures ─────────────────────────────────────────────────────

_lib.GSSK_Init.argtypes            = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.GSSK_Init.restype             = ctypes.c_int

_lib.GSSK_Free.argtypes            = [ctypes.c_void_p]
_lib.GSSK_Free.restype             = None

_lib.GSSK_Reset.argtypes           = [ctypes.c_void_p]
_lib.GSSK_Reset.restype            = None

_lib.GSSK_Step.argtypes            = [ctypes.c_void_p, ctypes.c_double]
_lib.GSSK_Step.restype             = ctypes.c_int

_lib.GSSK_StepAdaptive.argtypes    = [ctypes.c_void_p]
_lib.GSSK_StepAdaptive.restype     = ctypes.c_int

_lib.GSSK_GetState.argtypes        = [ctypes.c_void_p]
_lib.GSSK_GetState.restype         = ctypes.POINTER(ctypes.c_double)

_lib.GSSK_GetStateSize.argtypes    = [ctypes.c_void_p]
_lib.GSSK_GetStateSize.restype     = ctypes.c_size_t

_lib.GSSK_GetTStart.argtypes       = [ctypes.c_void_p]
_lib.GSSK_GetTStart.restype        = ctypes.c_double

_lib.GSSK_GetTEnd.argtypes         = [ctypes.c_void_p]
_lib.GSSK_GetTEnd.restype          = ctypes.c_double

_lib.GSSK_GetDt.argtypes           = [ctypes.c_void_p]
_lib.GSSK_GetDt.restype            = ctypes.c_double

_lib.GSSK_GetCurrentTime.argtypes  = [ctypes.c_void_p]
_lib.GSSK_GetCurrentTime.restype   = ctypes.c_double

_lib.GSSK_GetStepCount.argtypes    = [ctypes.c_void_p]
_lib.GSSK_GetStepCount.restype     = ctypes.c_size_t

_lib.GSSK_GetNodeID.argtypes       = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetNodeID.restype        = ctypes.c_char_p

_lib.GSSK_FindNodeIdx.argtypes     = [ctypes.c_void_p, ctypes.c_char_p]
_lib.GSSK_FindNodeIdx.restype      = ctypes.c_int

_lib.GSSK_GetEdgeID.argtypes       = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetEdgeID.restype        = ctypes.c_char_p

_lib.GSSK_FindEdgeIdx.argtypes     = [ctypes.c_void_p, ctypes.c_char_p]
_lib.GSSK_FindEdgeIdx.restype      = ctypes.c_int

_lib.GSSK_GetEdgeCount.argtypes    = [ctypes.c_void_p]
_lib.GSSK_GetEdgeCount.restype     = ctypes.c_size_t

_lib.GSSK_GetEdgeK.argtypes        = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetEdgeK.restype         = ctypes.c_double

_lib.GSSK_SetEdgeK.argtypes        = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_double]
_lib.GSSK_SetEdgeK.restype         = None

_lib.GSSK_GetErrorDescription.argtypes = [ctypes.c_void_p]
_lib.GSSK_GetErrorDescription.restype  = ctypes.c_char_p

_lib.GSSK_GetModelName.argtypes    = [ctypes.c_void_p]
_lib.GSSK_GetModelName.restype     = ctypes.c_char_p

_lib.GSSK_GetVersionString.argtypes = []
_lib.GSSK_GetVersionString.restype  = ctypes.c_char_p

_lib.GSSK_GetConservationError.argtypes = [ctypes.c_void_p]
_lib.GSSK_GetConservationError.restype  = ctypes.c_double

_lib.GSSK_SerializeModel.argtypes    = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
_lib.GSSK_SerializeModel.restype     = ctypes.c_int

_lib.GSSK_SerializeSnapshot.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
_lib.GSSK_SerializeSnapshot.restype  = ctypes.c_int

_lib.GSSK_FreeString.argtypes        = [ctypes.c_char_p]
_lib.GSSK_FreeString.restype         = None

_lib.GSSK_GetCarrierCount.argtypes   = [ctypes.c_void_p]
_lib.GSSK_GetCarrierCount.restype    = ctypes.c_size_t

_lib.GSSK_GetNodeCarrier.argtypes    = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetNodeCarrier.restype     = ctypes.c_char_p

_lib.GSSK_GetEdgeCarrier.argtypes    = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetEdgeCarrier.restype     = ctypes.c_char_p

_lib.GSSK_GetCarrierConservationError.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetCarrierConservationError.restype  = ctypes.c_double

_lib.GSSK_GetSensitivity.argtypes    = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]
_lib.GSSK_GetSensitivity.restype     = ctypes.c_double

_lib.GSSK_EnableForwardSensitivity.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_size_t,
]
_lib.GSSK_EnableForwardSensitivity.restype  = ctypes.c_int

_lib.GSSK_DisableForwardSensitivity.argtypes = [ctypes.c_void_p]
_lib.GSSK_DisableForwardSensitivity.restype  = None

_lib.GSSK_GetNodeTypeString.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetNodeTypeString.restype  = ctypes.c_char_p

_lib.GSSK_GetArchetypeCount.argtypes = [ctypes.c_void_p]
_lib.GSSK_GetArchetypeCount.restype  = ctypes.c_size_t

_lib.GSSK_GetArchetypeName.argtypes  = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetArchetypeName.restype   = ctypes.c_char_p

_lib.GSSK_GetCompositeCount.argtypes = [ctypes.c_void_p]
_lib.GSSK_GetCompositeCount.restype  = ctypes.c_size_t

_lib.GSSK_GetCompositeID.argtypes    = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetCompositeID.restype     = ctypes.c_char_p

_lib.GSSK_GetMotifCount.argtypes     = [ctypes.c_void_p]
_lib.GSSK_GetMotifCount.restype      = ctypes.c_size_t

_lib.GSSK_GetMotifCanon.argtypes     = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetMotifCanon.restype      = ctypes.c_char_p

_lib.GSSK_GetMotifOccurrence.argtypes  = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetMotifOccurrence.restype   = ctypes.c_size_t

_lib.GSSK_GetMotifStableSteps.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetMotifStableSteps.restype  = ctypes.c_size_t

_lib.GSSK_IsMotifCandidate.argtypes  = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_IsMotifCandidate.restype   = ctypes.c_bool

_lib.GSSK_GetMotifSize.argtypes      = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetMotifSize.restype       = ctypes.c_size_t

_lib.GSSK_GetMotifComplexity.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_lib.GSSK_GetMotifComplexity.restype  = ctypes.c_double

_lib.GSSK_GetGenerativityIndex.argtypes = [ctypes.c_void_p]
_lib.GSSK_GetGenerativityIndex.restype  = ctypes.c_double

_lib.GSSK_ProposeArchetype.argtypes  = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
_lib.GSSK_ProposeArchetype.restype   = ctypes.c_int

# ── Exceptions ────────────────────────────────────────────────────────────────

class GSSKError(Exception):
    pass

class GSSKDivergenceError(GSSKError):
    pass

class GSSKSchemaError(GSSKError):
    pass

# ── Main class ────────────────────────────────────────────────────────────────

class GSSKSimulator:
    """
    High-level Python wrapper for the GSSK ODE simulation kernel.

    Parameters
    ----------
    json_str : str
        GSSK-schema-conforming JSON model string.

    Raises
    ------
    GSSKSchemaError
        If the JSON is invalid or violates the schema.
    GSSKError
        For any other kernel error.
    """

    def __init__(self, json_str: str) -> None:
        raw = ctypes.c_void_p()
        status = _lib.GSSK_Init(json_str.encode(), ctypes.byref(raw))
        self._inst = raw

        if status != GSSK_SUCCESS:
            msg = _lib.GSSK_GetErrorDescription(self._inst)
            msg_str = msg.decode() if msg else ""
            _lib.GSSK_Free(self._inst)
            self._inst = None
            if status == GSSK_ERR_SCHEMA_VIOLATION:
                raise GSSKSchemaError(msg_str)
            raise GSSKError(f"GSSK_Init failed (status={status}): {msg_str}")

        # Build node manifest
        n = int(_lib.GSSK_GetStateSize(self._inst))
        self._manifest: dict[int, str] = {}
        for i in range(n):
            cstr = _lib.GSSK_GetNodeID(self._inst, i)
            if cstr:
                self._manifest[i] = cstr.decode()

    def __del__(self) -> None:
        if self._inst is not None:
            _lib.GSSK_Free(self._inst)
            self._inst = None

    # ── Properties ──────────────────────────────────────────────────────────

    @property
    def state_size(self) -> int:
        return int(_lib.GSSK_GetStateSize(self._inst))

    @property
    def state(self) -> list[float]:
        """Current Q[] vector as a Python list."""
        n = self.state_size
        ptr = _lib.GSSK_GetState(self._inst)
        return list(ptr[:n])

    @property
    def named_state(self) -> dict[str, float]:
        """Current state keyed by node ID."""
        s = self.state
        return {name: s[i] for i, name in self._manifest.items()}

    @property
    def current_time(self) -> float:
        return _lib.GSSK_GetCurrentTime(self._inst)

    @property
    def start_time(self) -> float:
        return _lib.GSSK_GetTStart(self._inst)

    @property
    def end_time(self) -> float:
        return _lib.GSSK_GetTEnd(self._inst)

    @property
    def default_dt(self) -> float:
        return _lib.GSSK_GetDt(self._inst)

    @property
    def step_count(self) -> int:
        return int(_lib.GSSK_GetStepCount(self._inst))

    @property
    def conservation_error(self) -> float:
        return _lib.GSSK_GetConservationError(self._inst)

    @property
    def model_name(self) -> str:
        cstr = _lib.GSSK_GetModelName(self._inst)
        return cstr.decode() if cstr else ""

    @property
    def carrier_count(self) -> int:
        return int(_lib.GSSK_GetCarrierCount(self._inst))

    @property
    def node_manifest(self) -> dict[int, str]:
        return dict(self._manifest)

    @staticmethod
    def kernel_version() -> str:
        cstr = _lib.GSSK_GetVersionString()
        return cstr.decode() if cstr else ""

    # ── Simulation ───────────────────────────────────────────────────────────

    def step(self, dt: float | None = None) -> list[float]:
        """Advance by one fixed step. Returns new state vector."""
        h = dt if dt is not None else self.default_dt
        status = _lib.GSSK_Step(self._inst, h)
        if status == GSSK_ERR_DIVERGENCE:
            raise GSSKDivergenceError("NaN/Inf detected during integration")
        return self.state

    def step_adaptive(self) -> list[float]:
        """Advance by one DOPRI5 adaptive step."""
        status = _lib.GSSK_StepAdaptive(self._inst)
        if status == GSSK_ERR_DIVERGENCE:
            raise GSSKDivergenceError("NaN/Inf detected during adaptive integration")
        return self.state

    def run(self, dt: float | None = None) -> list[list[float]]:
        """Run to t_end. Returns all state vectors (one per step)."""
        self.reset()
        h = dt if dt is not None else self.default_dt
        results: list[list[float]] = []
        while self.current_time < self.end_time - 1e-12:
            results.append(self.step(h))
        return results

    def run_named(self, dt: float | None = None) -> dict[str, list[float]]:
        """Run to t_end. Returns time series keyed by node ID."""
        raw = self.run(dt)
        out: dict[str, list[float]] = {name: [] for name in self._manifest.values()}
        for snapshot in raw:
            for i, name in self._manifest.items():
                out[name].append(snapshot[i])
        return out

    def reset(self) -> None:
        _lib.GSSK_Reset(self._inst)

    # ── Edge access ──────────────────────────────────────────────────────────

    @property
    def edge_count(self) -> int:
        return int(_lib.GSSK_GetEdgeCount(self._inst))

    def edge_id(self, index: int) -> str | None:
        cstr = _lib.GSSK_GetEdgeID(self._inst, index)
        return cstr.decode() if cstr else None

    def edge_k(self, index: int) -> float:
        return _lib.GSSK_GetEdgeK(self._inst, index)

    def set_edge_k(self, index: int, k: float) -> None:
        _lib.GSSK_SetEdgeK(self._inst, index, k)

    def find_edge(self, edge_id: str) -> int:
        """Return kernel index of edge by ID, or -1 if not found."""
        return int(_lib.GSSK_FindEdgeIdx(self._inst, edge_id.encode()))

    def find_node(self, node_id: str) -> int:
        return int(_lib.GSSK_FindNodeIdx(self._inst, node_id.encode()))

    # ── Multi-carrier ────────────────────────────────────────────────────────

    def node_carrier(self, node_index: int) -> str:
        cstr = _lib.GSSK_GetNodeCarrier(self._inst, node_index)
        return cstr.decode() if cstr else ""

    def edge_carrier(self, edge_index: int) -> str:
        cstr = _lib.GSSK_GetEdgeCarrier(self._inst, edge_index)
        return cstr.decode() if cstr else ""

    def carrier_conservation_error(self, carrier_index: int) -> float:
        return _lib.GSSK_GetCarrierConservationError(self._inst, carrier_index)

    # ── Sensitivity ──────────────────────────────────────────────────────────

    def enable_forward_sensitivity(self, param_edge_indices: list[int]) -> None:
        arr = (ctypes.c_size_t * len(param_edge_indices))(*param_edge_indices)
        status = _lib.GSSK_EnableForwardSensitivity(self._inst, arr, len(param_edge_indices))
        if status != GSSK_SUCCESS:
            raise GSSKError(f"EnableForwardSensitivity failed (status={status})")

    def disable_forward_sensitivity(self) -> None:
        _lib.GSSK_DisableForwardSensitivity(self._inst)

    def get_sensitivity(self, node_idx: int, param_idx: int) -> float:
        return _lib.GSSK_GetSensitivity(self._inst, node_idx, param_idx)

    # ── Node type / archetype / composite ────────────────────────────────────

    def node_type_string(self, node_index: int) -> str:
        cstr = _lib.GSSK_GetNodeTypeString(self._inst, node_index)
        return cstr.decode() if cstr else ""

    @property
    def archetype_count(self) -> int:
        return int(_lib.GSSK_GetArchetypeCount(self._inst))

    def archetype_name(self, index: int) -> str | None:
        cstr = _lib.GSSK_GetArchetypeName(self._inst, index)
        return cstr.decode() if cstr else None

    @property
    def composite_count(self) -> int:
        return int(_lib.GSSK_GetCompositeCount(self._inst))

    def composite_id(self, index: int) -> str | None:
        cstr = _lib.GSSK_GetCompositeID(self._inst, index)
        return cstr.decode() if cstr else None

    # ── Phase 9 — Pattern discovery / generativity ───────────────────────────

    @property
    def motif_count(self) -> int:
        return int(_lib.GSSK_GetMotifCount(self._inst))

    def motif_canon(self, index: int) -> str | None:
        cstr = _lib.GSSK_GetMotifCanon(self._inst, index)
        return cstr.decode() if cstr else None

    def motif_occurrence(self, index: int) -> int:
        return int(_lib.GSSK_GetMotifOccurrence(self._inst, index))

    def motif_stable_steps(self, index: int) -> int:
        return int(_lib.GSSK_GetMotifStableSteps(self._inst, index))

    def is_motif_candidate(self, index: int) -> bool:
        return bool(_lib.GSSK_IsMotifCandidate(self._inst, index))

    def motif_size(self, index: int) -> int:
        return int(_lib.GSSK_GetMotifSize(self._inst, index))

    def motif_complexity(self, index: int) -> float:
        return float(_lib.GSSK_GetMotifComplexity(self._inst, index))

    @property
    def generativity_index(self) -> float:
        return float(_lib.GSSK_GetGenerativityIndex(self._inst))

    def propose_archetype(self, motif_index: int, name: str) -> None:
        status = _lib.GSSK_ProposeArchetype(self._inst, motif_index, name.encode())
        if status != GSSK_SUCCESS:
            raise GSSKError(f"ProposeArchetype failed (status={status})")

    # ── Serialisation ────────────────────────────────────────────────────────

    def serialize_model(self) -> str:
        out = ctypes.c_char_p()
        status = _lib.GSSK_SerializeModel(self._inst, ctypes.byref(out))
        if status != GSSK_SUCCESS or not out.value:
            raise GSSKError("SerializeModel failed")
        result = out.value.decode()
        _lib.GSSK_FreeString(out)
        return result

    def serialize_snapshot(self) -> str:
        out = ctypes.c_char_p()
        status = _lib.GSSK_SerializeSnapshot(self._inst, ctypes.byref(out))
        if status != GSSK_SUCCESS or not out.value:
            raise GSSKError("SerializeSnapshot failed")
        result = out.value.decode()
        _lib.GSSK_FreeString(out)
        return result

    # ── pandas helper (optional) ─────────────────────────────────────────────

    def run_dataframe(self, dt: float | None = None):  # type: ignore[return]
        """
        Run to t_end and return a pandas DataFrame with one column per node
        and a 'time' column.

        Requires pandas to be installed.
        """
        try:
            import pandas as pd
        except ImportError as e:
            raise ImportError("pandas is required for run_dataframe()") from e

        self.reset()
        h = dt if dt is not None else self.default_dt
        records = []
        t = self.start_time
        while self.current_time < self.end_time - 1e-12:
            self.step(h)
            t = self.current_time
            row = {"time": t}
            row.update(self.named_state)
            records.append(row)
        return pd.DataFrame(records)


# ── Module-level convenience ──────────────────────────────────────────────────

def from_file(path: str) -> GSSKSimulator:
    """Create a GSSKSimulator from a JSON model file path."""
    with open(path, encoding="utf-8") as f:
        return GSSKSimulator(f.read())
