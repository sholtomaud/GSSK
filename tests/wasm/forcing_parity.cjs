/* WASM parity for forcing — requirement 3 of the upstream proposal.
 *
 * There is ONE distributed artifact, and it must not silently differ from
 * native. sin() and exp() are the two that could: an emscripten build could
 * substitute a different libm, and the divergence would be small enough to
 * look like round-off while being systematic.
 *
 * MEASURED, rather than assumed. Comparing the same eight sample points three
 * ways — macOS Apple clang, Linux GCC, and WASM — the WASM build agrees with
 * Linux GCC EXACTLY, bit for bit, on all eight for every waveform. macOS is
 * the outlier: its libm sin() differs by 1 ULP at two of the eight points
 * (t=2.5 and t=19.9 on the sine case, ~8.9e-16 and ~2.2e-16).
 *
 * So the tolerance below is NOT slack for WASM. It is slack for the platform
 * this check happens to run on. sin/exp are not required by IEEE-754 to be
 * correctly rounded, so any two libms may legitimately differ by <1 ULP, and
 * demanding bit-equality would mean GSSK shipping its own transcendentals.
 * That is a real question for the Phase G reconstruction story and is filed
 * separately — it is not something to decide inside a forcing task, and it
 * pre-dates forcing anyway (exp() is already on the Riccati duet path and
 * pow() on the adaptive step controller).
 *
 * 4 ULP is comfortably above the 1 ULP observed and far below anything that
 * could hide a substituted implementation, a wrong formula, or a dropped
 * parameter — which are the failures this check is actually for.
 *
 * It also confirms the four forcing symbols are actually callable through the
 * built dist/gssk.js — the WASM job exists because export-list breakage has
 * reached main unverified before, and a getter that is not exported does not
 * solve the problem it was added for.
 */
const fs = require('fs');
const createGSSK = require('../../dist/gssk.js');

const NATIVE = JSON.parse(fs.readFileSync(__dirname + '/../results/forcing_native.json', 'utf8'));

let failures = 0;
function check(cond, msg) { if (!cond) { console.log('  FAIL: ' + msg); failures++; } }

// Distance in representable doubles, so the bound is scale-free — an absolute
// epsilon would be far too loose near 1e6 and far too tight near 1e-6.
const _b = new DataView(new ArrayBuffer(8));
function ordinal(x) {
  _b.setFloat64(0, x);
  const hi = _b.getUint32(0), lo = _b.getUint32(4);
  let n = (BigInt(hi) << 32n) | BigInt(lo);
  // Map the sign-magnitude layout onto a monotone integer line.
  return (hi & 0x80000000) ? -(n & 0x7fffffffffffffffn) : n;
}
function ulpsApart(a, b) {
  if (a === b) return 0;
  if (!Number.isFinite(a) || !Number.isFinite(b)) return Infinity;
  const d = ordinal(a) - ordinal(b);
  return Number(d < 0n ? -d : d);
}
function withinUlps(a, b, n) { return ulpsApart(a, b) <= n; }

createGSSK().then(mod => {
  const alloc = s => { const n = mod.lengthBytesUTF8(s) + 1; const p = mod._malloc(n); mod.stringToUTF8(s, p, n); return p; };

  for (const cas of NATIVE.cases) {
    const mp = alloc(cas.model), outp = mod._malloc(4);
    const st = mod._GSSK_Init(mp, outp);
    if (st !== 0) { check(false, `${cas.name}: WASM init failed (${st})`); continue; }
    const inst = mod.HEAPU32[outp >> 2];

    check(mod._GSSK_GetNodeForcingKind(inst, 0) === cas.kind,
      `${cas.name}: kind ${mod._GSSK_GetNodeForcingKind(inst, 0)} != native ${cas.kind}`);

    for (const s of cas.samples) {
      const got = mod._GSSK_EvaluateNodeForcing(inst, 0, s.t);
      check(withinUlps(got, s.v, 4),
        `${cas.name} at t=${s.t}: WASM ${got} vs native ${s.v} — ` +
        `${ulpsApart(got, s.v)} ULP apart, above the 4 ULP libm allowance. ` +
        `That is too far to be rounding: suspect a substituted implementation, ` +
        `a wrong formula, or a parameter that did not reach the evaluator.`);
    }
    mod._GSSK_Free(inst);
  }

  // The edge evaluator too, so both attachment points are covered.
  const emp = alloc(NATIVE.edge_case.model), eoutp = mod._malloc(4);
  if (mod._GSSK_Init(emp, eoutp) === 0) {
    const einst = mod.HEAPU32[eoutp >> 2];
    check(mod._GSSK_GetEdgeForcingKind(einst, 0) === NATIVE.edge_case.kind,
      'edge forcing kind mismatch');
    for (const s of NATIVE.edge_case.samples) {
      const got = mod._GSSK_EvaluateEdgeForcing(einst, 0, s.t);
      check(withinUlps(got, s.v, 4),
        `edge at t=${s.t}: WASM ${got} vs native ${s.v} — ` +
        `${ulpsApart(got, s.v)} ULP apart`);
    }
    mod._GSSK_Free(einst);
  } else {
    check(false, 'edge case failed to init under WASM');
  }

  if (failures) { console.log(`\n=== WASM FORCING PARITY FAILED (${failures}) ===`); process.exit(1); }
  console.log(`=== WASM FORCING PARITY OK (${NATIVE.cases.length + 1} models, within 4 ULP) ===`);
});
