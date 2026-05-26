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

  /** Schema version of the loaded model (2 or 3). */
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

  _GSSK_EnsembleForecast(kernelPtr: number, runs: number, perturbation: number): number;
  _GSSK_FreeEnsembleResult(resPtr: number): void;
  _GSSK_Calibrate(kernelPtr: number, obsPtr: number, obsCount: number, iterations: number): number;
  _GSSK_Free(kernelPtr: number): void;
  _malloc(size: number): number;
  _free(ptr: number): void;

  /** Phase 5 — Multi-carrier schema. */
  /** Number of carriers declared in the top-level carriers array. */
  _GSSK_GetCarrierCount(kernelPtr: number): number;
  /**
   * Pointer to a GSSK_Carrier struct {id[32], unit[32], conserved: bool}.
   * Returns 0 if idx >= carrierCount.
   */
  _GSSK_GetCarrier(kernelPtr: number, idx: number): number;
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

  stringToUTF8(str: string, outPtr: number, maxBytes: number): void;
  UTF8ToString(ptr: number): string;
  lengthBytesUTF8(str: string): number;
  HEAPU8: Uint8Array;
  HEAPF64: Float64Array;
}

declare function createGSSK(): Promise<GSSKModule>;
export default createGSSK;
