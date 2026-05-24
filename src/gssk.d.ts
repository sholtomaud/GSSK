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

  _GSSK_EnsembleForecast(kernelPtr: number, runs: number, perturbation: number): number;
  _GSSK_FreeEnsembleResult(resPtr: number): void;
  _GSSK_Calibrate(kernelPtr: number, obsPtr: number, obsCount: number, iterations: number): number;
  _GSSK_Free(kernelPtr: number): void;
  _malloc(size: number): number;
  _free(ptr: number): void;

  stringToUTF8(str: string, outPtr: number, maxBytes: number): void;
  UTF8ToString(ptr: number): string;
  lengthBytesUTF8(str: string): number;
  HEAPU8: Uint8Array;
  HEAPF64: Float64Array;
}

declare function createGSSK(): Promise<GSSKModule>;
export default createGSSK;
