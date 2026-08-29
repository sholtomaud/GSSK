export interface GSSKModule {
  _GSSK_Init(jsonPtr: number, outInstPtr: number): number;
  _GSSK_GetErrorDescription(kernelPtr: number): number;
  _GSSK_Step(kernelPtr: number, dt: number): number;
  _GSSK_GetState(kernelPtr: number): number;
  _GSSK_GetStateSize(kernelPtr: number): number;
  _GSSK_GetTStart(kernelPtr: number): number;
  _GSSK_GetTEnd(kernelPtr: number): number;
  _GSSK_GetDt(kernelPtr: number): number;
  _GSSK_GetNodeID(kernelPtr: number, index: number): number;
  /** Node type as a GSSK_NodeType ordinal: 0 storage, 1 source, 2 sink,
   *  3 constant, 4 interaction, 5 gain, 6 loop_limited, 7 exchange, 8 switch,
   *  9 invalid (null instance or out-of-range index). */
  _GSSK_GetNodeType(kernelPtr: number, index: number): number;
  _GSSK_FindNodeIdx(kernelPtr: number, idPtr: number): number;
  _GSSK_GetEdgeID(kernelPtr: number, index: number): number;
  _GSSK_FindEdgeIdx(kernelPtr: number, idPtr: number): number;
  _GSSK_Reset(kernelPtr: number): void;
  _GSSK_GetEdgeCount(kernelPtr: number): number;
  _GSSK_GetEdgeK(kernelPtr: number, index: number): number;
  _GSSK_SetEdgeK(kernelPtr: number, index: number, k: number): void;

  _GSSK_GetTransformationRatio(kernelPtr: number): number;
  _GSSK_GetQualityFlow(kernelPtr: number): number;
  _GSSK_GetEdgeQualityFlow(kernelPtr: number, index: number): number;
  _GSSK_GetSolverConfidence(kernelPtr: number): number;

  _GSSK_AddNode(kernelPtr: number, jsonPtr: number): number;
  _GSSK_AddEdge(kernelPtr: number, jsonPtr: number): number;
  _GSSK_DeactivateEdge(kernelPtr: number, idPtr: number): number;
  _GSSK_DeactivateNode(kernelPtr: number, idPtr: number): number;
  _GSSK_ReclassifyNetwork(kernelPtr: number): number;

  /** Current simulation time (advances with each Step; restored from snapshot). */
  _GSSK_GetCurrentTime(kernelPtr: number): number;
  /** Number of steps taken since last Init/Reset. */
  _GSSK_GetStepCount(kernelPtr: number): number;

  /**
   * Serialize topology to JSON (no snapshot block). Returns pointer to
   * heap string; caller must free with _GSSK_FreeString.
   */
  _GSSK_SerializeModel(kernelPtr: number, outPtrPtr: number): number;
  /**
   * Serialize topology + live state (with snapshot block). Returns pointer
   * to heap string; caller must free with _GSSK_FreeString.
   */
  _GSSK_SerializeSnapshot(kernelPtr: number, outPtrPtr: number): number;
  /** Free a string returned by SerializeModel/SerializeSnapshot. */
  _GSSK_FreeString(ptr: number): void;

  /** Schema version of the loaded model (2, 3, or 4). v2 auto-migrates to 3 at init. */
  _GSSK_GetSchemaVersion(kernelPtr: number): number;
  /** Model name from metadata.name. Returns pointer to C string. */
  _GSSK_GetModelName(kernelPtr: number): number;
  /** Model description from metadata.description. */
  _GSSK_GetModelDescription(kernelPtr: number): number;
  /** Kernel version that created this model (e.g. "3.0.0"). */
  _GSSK_GetModelKernelVersion(kernelPtr: number): number;
  /** SHA256 hex hash from metadata.model_hash. */
  _GSSK_GetModelHash(kernelPtr: number): number;
  /** Current kernel version string (static, no instance needed). */
  _GSSK_GetVersionString(): number;
  /** Current kernel version as uint32: (major<<16)|(minor<<8)|patch. */
  _GSSK_GetVersionCode(): number;

  /** Phase 1 — per-edge IDC vs RK4 relative error from the last step. */
  _GSSK_GetEdgeErrorEstimate(kernelPtr: number, edgeIdx: number): number;
  /** Phase 1 — step-level max error (max over all edges). */
  _GSSK_GetStepErrorEstimate(kernelPtr: number): number;
  /** Phase 1 — number of threshold crossing events since last reset. */
  _GSSK_GetEventCount(kernelPtr: number): number;
  /** Phase 1 — simulation time of event at eventIdx. */
  _GSSK_GetEventTime(kernelPtr: number, eventIdx: number): number;
  /** Phase 1 — edge ID string pointer for event at eventIdx. */
  _GSSK_GetEventEdgeID(kernelPtr: number, eventIdx: number): number;
  /** Phase 1 — crossing direction: +1 upward, -1 downward. */
  _GSSK_GetEventDirection(kernelPtr: number, eventIdx: number): number;

  /** Phase 2 — advance one adaptively-sized DOPRI5 step; returns status. */
  /** Phase 3.1 — Enable forward sensitivity for a set of edge parameters. */
  _GSSK_EnableForwardSensitivity(kernelPtr: number, paramEdgeIndicesPtr: number, paramCount: number): number;
  /** Phase 3.1 — Disable forward sensitivity and free the matrix. */
  _GSSK_DisableForwardSensitivity(kernelPtr: number): void;
  /** Phase 3.1 — Read ∂Q[nodeIdx]/∂k[paramIdx]. paramIdx is the column in S. */
  _GSSK_GetSensitivity(kernelPtr: number, nodeIdx: number, paramIdx: number): number;
  /** Phase 3.2 — Compute adjoint gradient. targetsPtr: packed GSSK_AdjointTarget[]. */
  _GSSK_RunAdjoint(kernelPtr: number, targetsPtr: number, targetCount: number, paramEdgeIndicesPtr: number, paramCount: number, outGradientPtr: number): number;
  /** Phase 3.3 — ∂Tr[nodeIdx]/∂k[edgeIdx] via implicit differentiation. */
  _GSSK_GetTransformitySensitivity(kernelPtr: number, nodeIdx: number, edgeIdx: number): number;
  /** Phase 3.4 — Gradient-based calibration (L-M + forward sensitivity). */
  _GSSK_CalibrateGradient(kernelPtr: number, obsPtr: number, obsCount: number, paramEdgeIndicesPtr: number, paramCount: number, iterations: number): number;
  /** Phase 3.4 — Monte-Carlo calibration (original DE path). */
  _GSSK_CalibrateMonteCarlo(kernelPtr: number, obsPtr: number, obsCount: number, iterations: number): number;

  _GSSK_StepAdaptive(kernelPtr: number): number;
  /** Phase 2 — actual step size h used in the last accepted step. */
  _GSSK_GetLastStepSize(kernelPtr: number): number;
  /** Phase 2 — suggested h for the next GSSK_StepAdaptive call. */
  _GSSK_GetNextStepSize(kernelPtr: number): number;
  /** Phase 2 — relative total-Q change for the last step (conservation error). */
  _GSSK_GetConservationError(kernelPtr: number): number;

  /**
   * Runs `runs` perturbed simulations and returns a pointer to a
   * GSSK_EnsembleResult, or 0 on failure.  Must be released with
   * _GSSK_FreeEnsembleResult.
   *
   * DO NOT decode the returned pointer. Reading it through HEAPU32 bakes field
   * offsets and `size_t` width into your code, and neither is an ABI contract:
   * the fields sit at 0/4/8/12/16 under wasm32 but 0/8/16/24/32 in a native
   * 64-bit build, and -sMEMORY64 moves them again. Use the flat getters below,
   * for the same reason the GSSK_Carrier getters exist.
   */
  _GSSK_EnsembleForecast(kernelPtr: number, runs: number, perturbation: number): number;
  _GSSK_FreeEnsembleResult(resPtr: number): void;
  /** Nodes per step in the result; 0 for a null pointer. Same as _GSSK_GetStateSize. */
  _GSSK_GetEnsembleNodeCount(resPtr: number): number;
  /** Time steps in the result; 0 for a null pointer. Both endpoints included. */
  _GSSK_GetEnsembleStepCount(resPtr: number): number;
  /**
   * Envelope values at (step, node). These apply the step-major stride
   * internally, so nothing outside the kernel repeats `s * nodeCount + n`.
   *
   * The envelopes are pointwise statistics ACROSS runs, not three sampled
   * trajectories: min <= mean <= max holds everywhere. Equality is expected
   * where every run agrees — constant nodes, and step 0 of every node, since
   * perturbation only touches edge k.
   *
   * Out-of-range indices and a null pointer both return 0.0, which is
   * indistinguishable from a real 0.0 in the data; bound-check against the
   * count getters first if that matters.
   */
  _GSSK_GetEnsembleMin(resPtr: number, step: number, node: number): number;
  _GSSK_GetEnsembleMax(resPtr: number, step: number, node: number): number;
  _GSSK_GetEnsembleMean(resPtr: number, step: number, node: number): number;
  _GSSK_Calibrate(kernelPtr: number, obsPtr: number, obsCount: number, iterations: number): number;
  _GSSK_Free(kernelPtr: number): void;
  _malloc(size: number): number;
  _free(ptr: number): void;

  /**
   * Phase H — Forcing functions.
   *
   * Waveform kinds: 0 none, 1 step, 2 impulse, 3 ramp, 4 sawtooth, 5 square,
   * 6 sine, 7 exponential, 8 jitter.
   *
   * Flat scalars by design — no GSSK_Forcing struct crosses the boundary, for
   * the same reason the flat carrier getters exist.
   */
  /** Waveform kind attached to the node, or 0 for unforced / out of range. */
  _GSSK_GetNodeForcingKind(kernelPtr: number, nodeIdx: number): number;
  /** Waveform kind attached to the edge, or 0 for unforced / out of range. */
  _GSSK_GetEdgeForcingKind(kernelPtr: number, edgeIdx: number): number;
  /**
   * The node's forcing value at time t — THE SAME evaluator the derivative
   * path uses, so a rendered curve cannot drift from the simulated one.
   * Falls back to the node's declared value when unforced; 0 if out of range.
   *
   * JITTER ignores `t` and returns the value latched for the CURRENT step.
   * Drawing fresh would advance the RNG, so asking what the model is doing
   * would change what it does.
   */
  _GSSK_EvaluateNodeForcing(kernelPtr: number, nodeIdx: number, t: number): number;
  /** As above, for the edge's rate k. */
  _GSSK_EvaluateEdgeForcing(kernelPtr: number, edgeIdx: number, t: number): number;

  /** Phase 5 — Multi-carrier schema. */
  /** Number of carriers declared in the top-level carriers array. */
  _GSSK_GetCarrierCount(kernelPtr: number): number;
  /**
   * Pointer to a GSSK_Carrier struct {id[32], unit[32], conserved: bool}.
   * Returns 0 if idx >= carrierCount.
   *
   * PREFER the flat getters below. Decoding this struct from JS bakes field
   * offsets, `bool` width and trailing padding into your code — none of which
   * is an ABI contract, and all of which breaks by returning plausible garbage
   * rather than by failing.
   */
  _GSSK_GetCarrier(kernelPtr: number, idx: number): number;
  /**
   * Pointer to the carrier id string at idx. Never null; empty string if
   * idx >= carrierCount (unlike _GSSK_GetCarrier, which returns 0).
   */
  _GSSK_GetCarrierID(kernelPtr: number, idx: number): number;
  /**
   * Pointer to the carrier unit string at idx, e.g. "AUD", "kWh". This is the
   * y-axis label; carriers with different units may not share an axis.
   * Never null; empty string if idx >= carrierCount.
   */
  _GSSK_GetCarrierUnit(kernelPtr: number, idx: number): number;
  /**
   * 1 if the carrier at idx was declared conserved, else 0. Out of range also
   * returns 0 — bound-check against _GSSK_GetCarrierCount if that matters.
   */
  _GSSK_GetCarrierConserved(kernelPtr: number, idx: number): number;
  /** Index of the carrier with this id, or -1 if not found. */
  _GSSK_FindCarrierIdx(kernelPtr: number, idPtr: number): number;
  /** Pointer to carrier string on node at nodeIdx. Never null; may be empty. */
  _GSSK_GetNodeCarrier(kernelPtr: number, nodeIdx: number): number;
  /** Pointer to carrier string on edge at edgeIdx (Odum Position 1). Never null; may be empty. */
  _GSSK_GetEdgeCarrier(kernelPtr: number, edgeIdx: number): number;
  /**
   * Per-carrier relative conservation error from the last step.
   * Returns 0.0 if carrier is not conserved or no step has been taken.
   */
  _GSSK_GetCarrierConservationError(kernelPtr: number, carrierIdx: number): number;

  /** Phase 4 — Mutation log. */
  _GSSK_GetMutationCount(kernelPtr: number): number;
  /** Returns pointer to a GSSK_MutationRecord struct, or 0 if out of range. */
  _GSSK_GetMutationRecord(kernelPtr: number, idx: number): number;
  /** Set cause string for the next mutation to be appended. Pass 0 for default "user". */
  _GSSK_SetMutationCause(kernelPtr: number, causePtr: number): void;
  /** Clear all mutation log entries (capacity retained). */
  _GSSK_ClearMutationLog(kernelPtr: number): void;
  /** Serialize mutation log as a JSON array. Caller must free with _GSSK_FreeString. */
  _GSSK_ExportMutationLog(kernelPtr: number, outPtrPtr: number): number;
  /**
   * Replay from initial_json applying mutations_json at their recorded times,
   * stopping at target_t. out_inst_ptr receives the new kernel pointer.
   */
  _GSSK_Replay(initialJsonPtr: number, mutationsJsonPtr: number, targetT: number, outInstPtrPtr: number): number;

  /**
   * Randomness. Only _GSSK_EnsembleForecast and _GSSK_CalibrateMonteCarlo
   * consume it; stepping and gradient calibration are fully deterministic.
   * The generator is per-instance and seeded at init, so same model + same
   * seed gives bit-identical results. Seeds are uint64 and exceed the exact
   * range of a JS number — pass them as BigInt.
   */
  _GSSK_SetSeed(kernelPtr: number, seed: bigint): void;
  /** Seed last set (BigInt). Capture this in a run manifest. */
  _GSSK_GetSeed(kernelPtr: number): bigint;
  /** Next 64-bit draw on the instance stream. Advances it. */
  _GSSK_NextRandom(kernelPtr: number): bigint;
  /** Next draw uniformly in [min, max). Advances the stream. */
  _GSSK_NextRandomUniform(kernelPtr: number, min: number, max: number): number;

  /** Phase 8 — Composite node types & archetypes. */
  /** Number of archetypes registered (built-in + user-defined). */
  _GSSK_GetArchetypeCount(kernelPtr: number): number;
  /** Pointer to archetype name at idx, or 0 if out of range. */
  _GSSK_GetArchetypeName(kernelPtr: number, idx: number): number;
  /** Number of composite instances expanded at GSSK_Init time. */
  _GSSK_GetCompositeCount(kernelPtr: number): number;
  /** Pointer to the original (unexpanded) composite id, or 0 if out of range. */
  _GSSK_GetCompositeID(kernelPtr: number, compositeIdx: number): number;
  /** Pointer to the archetype name this composite expanded from, or 0 if OOB. */
  _GSSK_GetCompositeArchetype(kernelPtr: number, compositeIdx: number): number;
  /**
   * Pointer to the composite instance id owning this node — empty string if
   * the node was declared directly, 0 if nodeIdx is out of range.
   * Use this instead of splitting `{instance}__{member}` node ids: a directly
   * declared node may legitimately contain "__", and a composite id containing
   * "__" cannot be split unambiguously.
   */
  _GSSK_GetNodeComposite(kernelPtr: number, nodeIdx: number): number;
  /** Pointer to the node's role in its archetype ("body"/"gate"/"heat"); empty if none. */
  _GSSK_GetNodeRole(kernelPtr: number, nodeIdx: number): number;
  /** Number of state nodes this composite expanded to; 0 if out of range. */
  _GSSK_GetCompositeMemberCount(kernelPtr: number, compositeIdx: number): number;
  /** Node index of memberIdx within this composite; SIZE_MAX (2^32-1 in wasm32) if OOB. */
  _GSSK_GetCompositeMemberIndex(kernelPtr: number, compositeIdx: number, memberIdx: number): number;

  stringToUTF8(str: string, outPtr: number, maxBytes: number): void;
  UTF8ToString(ptr: number): string;
  lengthBytesUTF8(str: string): number;
  HEAPU8: Uint8Array;
  HEAPF64: Float64Array;
}

declare function createGSSK(): Promise<GSSKModule>;
export default createGSSK;
