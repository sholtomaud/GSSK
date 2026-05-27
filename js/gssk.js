/**
 * gssk.js — Thin JavaScript/TypeScript-friendly wrapper around the GSSK WASM module.
 *
 * Usage (ES module, after `make wasm`):
 *
 *   import { GSSKSimulator } from './gssk.js';
 *
 *   const sim = await GSSKSimulator.create(jsonString);
 *   console.log(sim.stateSize);  // number of nodes
 *   sim.step();
 *   console.log(sim.state);      // Float64Array of Q values
 *   const results = sim.run();   // Array<Float64Array>
 *   sim.free();
 *
 * The module locates the WASM binary via a configurable wasmPath option.
 * Browser: set wasmPath to the URL of gssk.wasm (served alongside gssk.js).
 * Node.js: set wasmPath to the file-system path of gssk.wasm.
 */

/* global createGSSK */

// ── Status codes ─────────────────────────────────────────────────────────────
export const Status = Object.freeze({
  SUCCESS: 0,
  ERR_INVALID_JSON: 1,
  ERR_MALLOC_FAILED: 2,
  ERR_SCHEMA_VIOLATION: 3,
  ERR_DIVERGENCE: 4,
  ERR_NOT_FOUND: 5,
  ERR_UNSUPPORTED_SCHEMA: 6,
  ERR_UNKNOWN: 7,
  WARN_SOLVER_DIVERGENCE: 8,
});

// ── Module loader ─────────────────────────────────────────────────────────────
let _module = null;

/**
 * Load (or return already-loaded) GSSK WASM module.
 *
 * @param {{ wasmPath?: string, moduleFactory?: Function }} [opts]
 * @returns {Promise<object>} Emscripten module
 */
export async function loadModule(opts = {}) {
  if (_module) return _module;

  // Caller can inject a pre-loaded factory (e.g. from a bundler)
  const factory = opts.moduleFactory ?? (typeof createGSSK !== 'undefined' ? createGSSK : null);
  if (!factory) {
    throw new Error(
      'GSSK WASM module factory not found. ' +
      'Either load gssk.js via a <script> tag first, or pass opts.moduleFactory.'
    );
  }

  const moduleOpts = opts.wasmPath ? { locateFile: () => opts.wasmPath } : {};
  _module = await factory(moduleOpts);
  return _module;
}

// ── String helpers ────────────────────────────────────────────────────────────
function writeString(mod, str) {
  const bytes = mod.lengthBytesUTF8(str) + 1;
  const ptr = mod._malloc(bytes);
  mod.stringToUTF8(str, ptr, bytes);
  return ptr;
}

function readString(mod, ptr) {
  return mod.UTF8ToString(ptr);
}

// ── GSSKSimulator class ───────────────────────────────────────────────────────
export class GSSKSimulator {
  #mod;
  #inst;
  #stateSize;
  #manifest; // Map<number, string>  index → node ID

  constructor(mod, instPtr, stateSize, manifest) {
    this.#mod = mod;
    this.#inst = instPtr;
    this.#stateSize = stateSize;
    this.#manifest = manifest;
  }

  /**
   * Create a GSSKSimulator from a JSON model string.
   *
   * @param {string} jsonStr  GSSK-schema-conforming JSON.
   * @param {{ wasmPath?: string, moduleFactory?: Function }} [opts]
   * @returns {Promise<GSSKSimulator>}
   */
  static async create(jsonStr, opts = {}) {
    const mod = await loadModule(opts);

    const jsonPtr  = writeString(mod, jsonStr);
    const instPPtr = mod._malloc(4);
    mod.HEAPU32[instPPtr >> 2] = 0;

    const status = mod._GSSK_Init(jsonPtr, instPPtr);
    const instPtr = mod.HEAPU32[instPPtr >> 2];

    mod._free(jsonPtr);
    mod._free(instPPtr);

    if (status !== Status.SUCCESS) {
      const errPtr = mod._GSSK_GetErrorDescription(instPtr);
      const msg = errPtr ? readString(mod, errPtr) : `status=${status}`;
      mod._GSSK_Free(instPtr);
      throw new Error(`GSSK_Init failed: ${msg}`);
    }

    const n = mod._GSSK_GetStateSize(instPtr);
    const manifest = new Map();
    for (let i = 0; i < n; i++) {
      const idPtr = mod._GSSK_GetNodeID(instPtr, i);
      if (idPtr) manifest.set(i, readString(mod, idPtr));
    }

    return new GSSKSimulator(mod, instPtr, n, manifest);
  }

  // ── Properties ─────────────────────────────────────────────────────────────

  get stateSize()    { return this.#stateSize; }
  get currentTime()  { return this.#mod._GSSK_GetCurrentTime(this.#inst); }
  get startTime()    { return this.#mod._GSSK_GetTStart(this.#inst); }
  get endTime()      { return this.#mod._GSSK_GetTEnd(this.#inst); }
  get defaultDt()    { return this.#mod._GSSK_GetDt(this.#inst); }
  get stepCount()    { return this.#mod._GSSK_GetStepCount(this.#inst); }
  get edgeCount()    { return this.#mod._GSSK_GetEdgeCount(this.#inst); }
  get carrierCount() { return this.#mod._GSSK_GetCarrierCount(this.#inst); }
  get conservationError() { return this.#mod._GSSK_GetConservationError(this.#inst); }
  get nodeManifest() { return new Map(this.#manifest); }

  get modelName() {
    const ptr = this.#mod._GSSK_GetModelName(this.#inst);
    return ptr ? readString(this.#mod, ptr) : '';
  }

  /** Current state vector as a Float64Array (a view — copy if you need to keep it). */
  get statePtr() {
    return this.#mod._GSSK_GetState(this.#inst);
  }

  get state() {
    const ptr = this.statePtr;
    return new Float64Array(this.#mod.HEAPF64.buffer, ptr, this.#stateSize).slice();
  }

  get namedState() {
    const s = this.state;
    const out = {};
    for (const [i, name] of this.#manifest) out[name] = s[i];
    return out;
  }

  // ── Simulation ──────────────────────────────────────────────────────────────

  step(dt = null) {
    const h = dt ?? this.defaultDt;
    const st = this.#mod._GSSK_Step(this.#inst, h);
    if (st === Status.ERR_DIVERGENCE) throw new Error('GSSK: numerical divergence (NaN/Inf)');
    return this.state;
  }

  stepAdaptive() {
    const st = this.#mod._GSSK_StepAdaptive(this.#inst);
    if (st === Status.ERR_DIVERGENCE) throw new Error('GSSK: adaptive divergence');
    return this.state;
  }

  run(dt = null) {
    this.reset();
    const h = dt ?? this.defaultDt;
    const results = [];
    while (this.currentTime < this.endTime - 1e-12) {
      results.push(this.step(h));
    }
    return results;
  }

  /**
   * Run and return an object keyed by node ID, each value a number[].
   */
  runNamed(dt = null) {
    const raw = this.run(dt);
    const out = {};
    for (const name of this.#manifest.values()) out[name] = [];
    for (const snapshot of raw) {
      for (const [i, name] of this.#manifest) out[name].push(snapshot[i]);
    }
    return out;
  }

  reset() { this.#mod._GSSK_Reset(this.#inst); }

  // ── Edge access ─────────────────────────────────────────────────────────────

  edgeID(index) {
    const ptr = this.#mod._GSSK_GetEdgeID(this.#inst, index);
    return ptr ? readString(this.#mod, ptr) : null;
  }

  edgeK(index)          { return this.#mod._GSSK_GetEdgeK(this.#inst, index); }
  setEdgeK(index, k)    { this.#mod._GSSK_SetEdgeK(this.#inst, index, k); }
  edgeCarrier(index)    { const p = this.#mod._GSSK_GetEdgeCarrier(this.#inst, index); return p ? readString(this.#mod, p) : ''; }
  nodeCarrier(index)    { const p = this.#mod._GSSK_GetNodeCarrier(this.#inst, index); return p ? readString(this.#mod, p) : ''; }

  carrierConservationError(carrierIdx) {
    return this.#mod._GSSK_GetCarrierConservationError(this.#inst, carrierIdx);
  }

  findEdge(id) {
    const p = writeString(this.#mod, id);
    const r = this.#mod._GSSK_FindEdgeIdx(this.#inst, p);
    this.#mod._free(p);
    return r;
  }

  findNode(id) {
    const p = writeString(this.#mod, id);
    const r = this.#mod._GSSK_FindNodeIdx(this.#inst, p);
    this.#mod._free(p);
    return r;
  }

  // ── Serialisation ───────────────────────────────────────────────────────────

  serializeModel() {
    const outPPtr = this.#mod._malloc(4);
    this.#mod.HEAPU32[outPPtr >> 2] = 0;
    const st = this.#mod._GSSK_SerializeModel(this.#inst, outPPtr);
    const strPtr = this.#mod.HEAPU32[outPPtr >> 2];
    this.#mod._free(outPPtr);
    if (st !== Status.SUCCESS || !strPtr) throw new Error('SerializeModel failed');
    const json = readString(this.#mod, strPtr);
    this.#mod._GSSK_FreeString(strPtr);
    return json;
  }

  serializeSnapshot() {
    const outPPtr = this.#mod._malloc(4);
    this.#mod.HEAPU32[outPPtr >> 2] = 0;
    const st = this.#mod._GSSK_SerializeSnapshot(this.#inst, outPPtr);
    const strPtr = this.#mod.HEAPU32[outPPtr >> 2];
    this.#mod._free(outPPtr);
    if (st !== Status.SUCCESS || !strPtr) throw new Error('SerializeSnapshot failed');
    const json = readString(this.#mod, strPtr);
    this.#mod._GSSK_FreeString(strPtr);
    return json;
  }

  // ── Mutation log ────────────────────────────────────────────────────────────

  get mutationCount() { return this.#mod._GSSK_GetMutationCount(this.#inst); }

  exportMutationLog() {
    const outPPtr = this.#mod._malloc(4);
    this.#mod.HEAPU32[outPPtr >> 2] = 0;
    const st = this.#mod._GSSK_ExportMutationLog(this.#inst, outPPtr);
    const strPtr = this.#mod.HEAPU32[outPPtr >> 2];
    this.#mod._free(outPPtr);
    if (st !== Status.SUCCESS || !strPtr) throw new Error('ExportMutationLog failed');
    const json = readString(this.#mod, strPtr);
    this.#mod._GSSK_FreeString(strPtr);
    return json;
  }

  clearMutationLog() { this.#mod._GSSK_ClearMutationLog(this.#inst); }

  // ── Node type / archetype / composite ──────────────────────────────────────

  nodeTypeString(index) {
    const ptr = this.#mod._GSSK_GetNodeTypeString(this.#inst, index);
    return ptr ? readString(this.#mod, ptr) : '';
  }

  get archetypeCount() { return this.#mod._GSSK_GetArchetypeCount(this.#inst); }

  archetypeName(index) {
    const ptr = this.#mod._GSSK_GetArchetypeName(this.#inst, index);
    return ptr ? readString(this.#mod, ptr) : null;
  }

  get compositeCount() { return this.#mod._GSSK_GetCompositeCount(this.#inst); }

  compositeID(index) {
    const ptr = this.#mod._GSSK_GetCompositeID(this.#inst, index);
    return ptr ? readString(this.#mod, ptr) : null;
  }

  // ── Phase 9 — Pattern discovery / generativity ─────────────────────────────

  get motifCount()          { return this.#mod._GSSK_GetMotifCount(this.#inst); }
  get generativityIndex()   { return this.#mod._GSSK_GetGenerativityIndex(this.#inst); }

  motifCanon(index) {
    const ptr = this.#mod._GSSK_GetMotifCanon(this.#inst, index);
    return ptr ? readString(this.#mod, ptr) : null;
  }

  motifOccurrence(index)  { return this.#mod._GSSK_GetMotifOccurrence(this.#inst, index); }
  motifStableSteps(index) { return this.#mod._GSSK_GetMotifStableSteps(this.#inst, index); }
  isMotifCandidate(index) { return !!this.#mod._GSSK_IsMotifCandidate(this.#inst, index); }
  motifSize(index)        { return this.#mod._GSSK_GetMotifSize(this.#inst, index); }
  motifComplexity(index)  { return this.#mod._GSSK_GetMotifComplexity(this.#inst, index); }

  proposeArchetype(motifIndex, name) {
    const p = writeString(this.#mod, name);
    const st = this.#mod._GSSK_ProposeArchetype(this.#inst, motifIndex, p);
    this.#mod._free(p);
    if (st !== 0) throw new Error(`ProposeArchetype failed: status=${st}`);
  }

  // ── Lifecycle ───────────────────────────────────────────────────────────────

  /** Release kernel memory. Call when done — not called automatically. */
  free() {
    if (this.#inst) {
      this.#mod._GSSK_Free(this.#inst);
      this.#inst = null;
    }
  }
}

// ── TypeScript types (JSDoc) ──────────────────────────────────────────────────
// See ../src/gssk.d.ts for full low-level WASM interface types.
// GSSKSimulator is the high-level wrapper surface type.
