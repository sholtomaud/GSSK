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
