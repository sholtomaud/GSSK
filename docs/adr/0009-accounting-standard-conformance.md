# ADR 0009 — Accounting-standard conformance: the trajectory is evidence, the ledger is downstream

- **Status**: proposed
- **Date**: 2026-09-01
- **Task**: `accounting-standards-conformance`
- **Supersedes**: nothing
- **Blocks**: `exchange-price-authored-vs-defaulted`, `ledger-conservation-diagnostic`, `model-hash-compute-or-withdraw`, `recognition-basis-declaration`

## Context

GSSK is to support pure accounting as a formally compliant system against accounting standards, and that compliance has to hold *simultaneously* with Odum's simulation method — the standards and the method must both be satisfied by one model, not by two models that agree at the edges. Neither direction may be bought at the other's expense: a ledger that conforms by special-casing money outside the energy-systems semantics is not a GSSK model, and an energy-systems model whose money leg is approximate is not a set of books.

**Standards in scope**: the AASB/IFRS recognition and measurement rules — the *Conceptual Framework for Financial Reporting*, AASB 15 / IFRS 15 *Revenue from Contracts with Customers*, AASB 102 / IAS 2 *Inventories*, AASB 9 / IFRS 9 *Financial Instruments* where a model carries them, and AASB 101 / IAS 1 for the accrual basis and materiality. Out of scope: auditing standards, tax effect accounting, and management accounting, which sit on top of a compliant ledger rather than defining one. Paragraph references below are to the IFRS text; a consumer should verify against the current compiled AASB standard.

GIP-0002 is what prompted this. An `exchange` inside an archetype could not have a per-instance price, so a model built on such an archetype loaded, ran, reported success and posted nothing. That is a tolerable simulation bug and a fatal ledger defect, and the difference is not one of degree: under the *Conceptual Framework* a faithful representation must be complete and free from error in its process (CF 2.12–2.19), and a set of statements that silently omits every transaction is neither. Fixing the instance without deciding the general rule guarantees the next instance is found by a consumer rather than by a test.

### What the kernel actually does today

Four findings, each verified against the code rather than inferred from it.

**1. The global conservation diagnostic is structurally zero for every model a ledger is made of.** `closed_system_conservation_error` (`src/gssk.c:2585`) returns `0.0` as soon as any active node is not a `storage`:

```c
for (size_t i = 0; i < inst->node_count; i++) {
  if (inst->nodes[i].active && inst->nodes[i].type != GSSK_NODE_STORAGE)
    return 0.0; /* open system — skip */
}
```

Every transaction diamond has an `exchange` node, and almost every ledger has a source or a terminal bucket. `GSSK_GetConservationError` therefore reports a perfect score, by construction, on exactly the models this ADR is about. Confirmed on `examples/exchange_price_node.json`: `0.000e+00`.

**2. The per-carrier conservation diagnostic cannot see a sink, so it reports a false positive on money that is exactly conserved.** `update_carrier_conservation_errors` (`src/gssk.c:2604`) sums only storage nodes:

```c
if (!inst->nodes[ni].active || inst->nodes[ni].type != GSSK_NODE_STORAGE) continue;
```

In the diamond, money is debited from the buyer (a `storage`) and credited to `spent` (a `sink`), which the sum skips. On the same model the money carrier reports a conservation error of `4.99e-03` while money is in fact conserved *exactly*: buyer `951.22942450096275` plus spent `48.77057549903725` sums to `1000.0` with a residual of `0.0` in double precision — the opening balance to the bit, not merely to tolerance. The metric is measuring storage-only money, not money. The symmetric failure is the dangerous one: a genuine leak into or out of a terminal bucket would not register either.

**3. `model_hash` is never computed.** It is read from the document (`src/gssk.c:3852`) and re-emitted (`src/gssk.c:6264`), and derived nowhere. `GSSK_GetModelHash` returns `""` for any model that does not declare one — confirmed on the same example. `gssk.schema.json:53` describes it as "SHA256 hex hash of the topology (nodes+edges) for reproducibility check. Auto-computed if not provided", which is false, and ADR 0004 already says so in passing ("the kernel round-trips that field and never computes it"). An accepted ADR and the published schema currently contradict each other.

**4. An absent price is indistinguishable from an authored price of zero.** `node_price` is zero-initialised and assigned only when the key is present and numeric (`src/gssk.c:4129`). An `exchange` with no `price` at all and one with `"price": 0.0` produce the same trajectory, silently, and that trajectory posts nothing.

### What the kernel already gets right

The double entry itself is **exact by construction**, and this is the strongest property GSSK has. `apply_transaction_coupling` (`src/gssk.c:1569`) debits and credits one computed `F_money` in a single statement:

```c
double F_money = price * F_primary;
if (money_from >= 0) deriv[money_from] -= F_money;
if (money_to   >= 0) deriv[money_to]   += F_money;
```

RK4 then applies identical stage weights to both entries, so the integrated debit equals the integrated credit up to floating-point summation — which is why finding 2's model balances to the last digit. Debits equal credits per transaction, not merely in aggregate, and no diagnostic is required to make that true. What the diagnostics fail to do is *observe* it.

## Decision

### D1 — The trajectory is evidence for postings, not the book of record

A continuous trajectory in IEEE doubles cannot be a general ledger. A ledger balances exactly in the minor unit of the presentation currency; a solver balances to tolerance. Asserting otherwise would require either discretising the physics to the cent — destroying the method — or claiming an exactness the integrator does not have.

The *Conceptual Framework* makes this a legitimate position rather than a concession. "Free from error" (CF 2.18) means no errors in the description of the phenomenon and none in *selecting and applying the process* that produced the information; it explicitly does not mean perfectly accurate in every respect. A documented, deterministic integration with a stated tolerance is such a process. An undocumented one is not.

**So: GSSK is the measurement engine, the ledger is downstream, and the boundary between them is an explicit posting step that quantises to the presentation currency's minor unit.** The quantisation residual is posted to a rounding account, never dropped — a dropped residual is precisely the silent imbalance the whole arrangement exists to prevent.

This is the move that reconciles the two systems. Odum's method stays continuous, and the books stay exact, because they are different artefacts related by a stated procedure.

### D2 — A continuous diamond is over-time recognition; point-in-time recognition needs a declared event

IFRS 15 §31 recognises revenue when (or as) a performance obligation is satisfied by transferring control. §35 permits recognition **over time** where the customer simultaneously receives and consumes the benefit, and §39 requires a single measure of progress toward complete satisfaction.

An integrated flow through a transaction diamond is exactly an output-method measure of progress. A continuously running diamond therefore maps onto over-time recognition with no distortion at all — the physics and the standard are describing the same thing in different vocabularies.

Point-in-time recognition (§38) has no continuous analogue: it requires an instant at which control transfers. GSSK has an event log (`emit_event`, `src/gssk.c:2238`), but it records **edge threshold crossings** — a time, an edge id and a direction — not transactions. Recognising at a point therefore requires the model to declare it, and requires a construct that produces a discrete recognition event.

**So: a model must declare, per diamond, whether it recognises over time or at a point in time.** Over-time is the default and needs nothing new. Point-in-time is not expressible today and must not be faked by reading a continuous trajectory at a chosen instant, which is a measurement of progress dressed as a control transfer.

### D3 — Emergy is never a measurement basis

Odum's transformity and emergy are a deliberate *alternative* to monetary valuation: their entire purpose is to value what price does not. IFRS measurement bases are historical cost and current value (fair value, value in use, current cost). Emergy is neither, and no reconciliation is available that would make it one.

**So: emergy quantities are supplementary information and never the measurement basis of a recognised amount.** Two consequences follow, one in each direction. No recognised amount may be derived from a transformity. And the money leg must not contribute emergy inflow — already tracked as `b3-coupling-quality-directionality`, which this ADR promotes from a modelling nicety to a conformance requirement: counting money as an energy input double-counts the transaction in the one system that is supposed to be independent of price.

### D4 — Endogenous price prices the future, never the past

Phase C makes price a state variable so that inflation emerges from M/W rather than being dialled in. Under IFRS 15 §47 the transaction price is the consideration to which the entity expects to be entitled under the contract; once a performance obligation is satisfied, the recognised amount is not re-measured because a later simulated price differs.

**So: a diamond in a forecast period may take its price from a price node; a diamond in a reported period trades at its historical transaction price and is immutable.** A model that re-prices history is not compliant however good its physics is.

This is the sharpest point where Odum's method must yield — and it yields by *scoping*, not by amputation. The endogenous-price machinery is untouched and stays exactly as useful for the forward-looking half, which is where a simulation earns its keep. What it may not do is reach backwards.

### D5 — A defaulted price is a load-time error; an authored zero is valid

The general form of the GIP-0002 defect is finding 4: the kernel cannot distinguish "no price was stated" from "the price is zero", and both post nothing.

**So: an `exchange` whose price resolves to a default rather than to an authored value must fail `GSSK_Init`.** GIP-0002 settled the archetype case — a template `price_node` naming no member is now rejected. The general rule requires the parser to track whether `price` was present, which it does not today.

**An authored zero stays valid.** A zero-consideration transfer is a real economic event; it is simply not revenue under IFRS 15, and is recognised — if at all — under another standard. The distinction the kernel must be able to draw is authored versus defaulted, not zero versus non-zero. Collapsing those two is what made the original defect invisible.

### D6 — The snapshot form is the ledger's serialisation; the model hash is computed or withdrawn

`GSSK_SerializeModel` emits each node's `initial_value` — the topology's initial condition — while `snapshot.state` patches live state only. A per-instance price delivered the way GIP-0002 prescribes therefore survives `GSSK_SerializeSnapshot` and **not** `GSSK_SerializeModel`: serialise, store, reload, and every diamond silently trades at its template default.

`GSSK_SerializeModel` is not wrong; it correctly serialises topology, and changing it would confuse an initial condition with a state. **So: `GSSK_SerializeSnapshot` is designated the book-of-record serialisation, and the documentation must say so where a consumer will read it before choosing.**

Model identity is the weaker half. The claim that a content hash identifies the model a forecast came from — the reason GIP-0002 rejected a `GSSK_SetNodePrice` setter, and a correct reason — currently rests on a self-declared, unverified string that is usually empty (finding 3). **So: `model_hash` is either computed by the kernel over the document that determines the trajectory, or the schema's "auto-computed" claim is withdrawn and the field documented as consumer-supplied and unverified.** Either is defensible. Leaving the claim standing while the kernel ignores it is the one option that is not, because it invites a consumer to treat an unverified string as an audit anchor.

### D7 — The conservation diagnostics may not be cited as evidence that money balances

Findings 1 and 2 are not edge cases; they are the normal case for a ledger. `GSSK_GetConservationError` returns zero for any open system, and `GSSK_GetCarrierConservationError` reports a false positive on an exactly-conserved money carrier because it cannot see a sink.

**So: neither metric may be presented, in documentation or to a consumer, as evidence that a model's money conserves.** The fix direction — a per-carrier sum over every node that can hold the carrier, with sources and sinks as explicit boundary terms — is recorded as a follow-up rather than decided here, because it changes a published diagnostic's meaning and deserves its own ADR if the numbers move.

## Consequences

**Four follow-up tasks**, tracked in crux, none of which this ADR implements:

| Task | What it closes |
| --- | --- |
| `exchange-price-authored-vs-defaulted` | D5 — the parser must distinguish an absent `price` from `0.0` |
| `ledger-conservation-diagnostic` | D7 — a conservation metric that counts terminal buckets |
| `model-hash-compute-or-withdraw` | D6 — the kernel computes the hash, or the schema stops claiming it |
| `recognition-basis-declaration` | D2 — per-diamond over-time versus point-in-time |

**What a consumer must do that the kernel will not do for them.** Quantise to the minor unit at posting time and post the residual (D1). Hold reported periods at their historical transaction prices (D4). Treat emergy as supplementary (D3). None of these can be enforced in a C99 kernel with no notion of a reporting period, and pretending otherwise would put a compliance claim in the wrong place.

**What does not change.** The physics: no derivative, no logic type and no solver behaviour is altered by anything above. The endogenous-price work stands in full, scoped to forecast periods. `GSSK_SerializeModel` keeps its current meaning. And the exact per-transaction double entry — the property that already holds — is now written down as a requirement rather than left as an implementation detail that a future refactor could quietly lose.

**The general shape of the risk.** Every finding here has the same signature: a model that loads, runs and reports success while being wrong. It is the signature of GIP-0002, of the unresolved node type ADR 0004 closed, and of the ignored `forcing` block ADR 0004 describes. For a simulation that is a bug of ordinary severity. For a ledger it is the only bug that matters, because every other kind announces itself.

**Cross-references.** ADR 0001 (transaction diamond — the shared coupling primitive whose exactness D1 depends on), ADR 0004 (the schema is advisory; silence is the failure mode), `snapshot-node-params-v5` (the schema-v5 alternative for per-instance node params), `b3-coupling-quality-directionality` (D3's money-leg exclusion), and GIP-0002 (`docs/gip/gip-0002-price-node-expansion.md`).
