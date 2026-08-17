# ADR 0001 — The transaction diamond: two authoring forms, one physics primitive

- **Status**: proposed
- **Date**: 2026-08-02
- **Task**: `b0-confirm-diamond-design`
- **Supersedes**: nothing
- **Blocks**: `b1-shared-coupling-helper`, `b2-coupled-edge-live`, `c0-price-constant-or-node-ref`

## Context

Odum's small diamond is simultaneously a *junction* (where a forward real flow
meets a backward money flow) and a *coupling ratio* (how much money moves per
unit of goods). GSSK currently offers two half-built expressions of it:

- **`NODE_EXCHANGE`** (`src/gssk.c:47`), computed by `compute_exchange_node`
  (`src/gssk.c:478`) — works, and is exercised by
  `tests/fuzz_corpus/seed_exchange_node.json`.
- **`coupled_edge`** — declared in the schema and resolved to `coupled_idx`
  (`src/gssk.c:2786`), but **inert**. A grep over `src/gssk.c` finds
  `coupled_idx` written at lines 2685, 2786, 2839 and 3694 and **read
  nowhere**. It carries no physics today.

The question this ADR settles: do we keep both authoring forms, or collapse to
one? Reading the implementation before deciding turned up three facts that the
original work-package framing did not account for, and they change the shape of
the answer.

### Finding 1 — the coupling physics is encoded in three places, not one

`compute_exchange_node` is called from two paths:

- `compute_derivatives` (`src/gssk.c:561`) — the RK4 derivative path.
- the IDC forcing path (`src/gssk.c:941`), which reuses the same function as an
  additive forcing term linearised about `Q_in`.

Those two share code, so they cannot drift. But there is a **third**, independent
encoding: the hand-written Jacobian block `case NODE_EXCHANGE`
(`src/gssk.c:1834–1867`). It re-derives the leg discovery *and* re-applies the
price multiplication itself:

```c
if (mi >= 0) J[(size_t)mi * n + (size_t)gi] -= p * dFg_dQg;
if (mo >= 0) J[(size_t)mo * n + (size_t)gi] += p * dFg_dQg;
```

Task B.1 as originally written says only "factor the coupling out of
`compute_exchange_node`". Doing exactly that would leave the Jacobian as a
second, independent statement of the same physics — precisely the divergence
Phase B exists to prevent. Any shared-primitive rule that does not reach the
Jacobian is not actually a shared primitive.

### Finding 2 — leg discovery is by carrier string, and is single-valued

Both `compute_exchange_node` and the Jacobian block find their legs by scanning
every edge and classifying on `strcmp(e->carrier, "money") == 0`, keeping one
origin and one target per class:

```c
if ((size_t)e->target_idx == ni) {
  if (is_money) money_in_orig = e->origin_idx;
  else          goods_in_orig = e->origin_idx;
}
```

This is a **last-wins** assignment. A second goods-in edge silently overwrites
the first. So the premise that the node form "supports >1 in/out" is aspirational
— it describes where we want to go, not what the code does. The node form's real
advantage over an inline edge form today is not arity; it is that it is the only
form that exists at all.

It also means "money" is a magic carrier string, not a declared role. Two edges
both carrying `money` into the same node is a modelling error the kernel cannot
currently detect.

### Finding 3 — money-in gates the goods flow; it is not only a debited stock

```c
double F_goods = inst->nodes[ni].node_k * state[goods_in_orig];
if (money_in_orig >= 0) F_goods *= state[money_in_orig];
double F_money = inst->nodes[ni].node_price * F_goods;
```

The buyer's money stock is an **interaction multiplier on the forward goods
flow**, not merely the account the money is debited from. Money plays two roles
at the node: it limits how fast the transaction can proceed, and it is the
counter-flowing stock.

This is the sharpest constraint on Phase B. An inline `coupled_edge` implemented
as just `F_money = price × F_primary` reproduces role two and **not** role one.
Under that implementation B.2's acceptance criterion — "a two-edge model
reproduces the same trajectory as the equivalent exchange-node model to solver
tolerance" — is unreachable whenever a money-in leg is present, because the two
forms would be computing different forward flows, not merely different
bookkeeping.

## Decision

**Keep both authoring forms. Deprecate neither. Back them with one shared
primitive that owns leg discovery, flow gating, and the coupling ratio
together.**

Rejected alternative: desugar `coupled_edge` into a hidden synthesized
`NODE_EXCHANGE` at parse time. Rejected because a synthesized node leaks
`__ex_*` columns into the state vector and therefore into CSV output, breaks
round-trip fidelity (`serialize` would emit nodes the author never wrote), and
complicates runtime mutation — an author who added an edge would find an extra
node they cannot address. This confirms the work package's recommendation, and
Finding 2 adds a reason it did not have: with leg discovery being carrier-string
and single-valued, a synthesized node inherits that fragility rather than fixing
it.

The decision is refined by the three findings above:

1. **The shared primitive must cover the Jacobian.** `apply_transaction_coupling`
   is not sufficient on its own. B.1 must additionally extract the leg-discovery
   scan into a single helper — call it `resolve_exchange_legs(inst, ni, ...)` —
   used by `compute_exchange_node` *and* the `NODE_EXCHANGE` Jacobian block, so
   the two cannot disagree about which edges form the diamond. The Jacobian's
   price handling must be derived from the same `price` resolution that C.0
   introduces, or a `price_node` will be seen by RK4 and not by IDC.

2. **Flow gating belongs in the primitive, not just the ratio.** The shared
   helper's contract is `(F_primary, price) -> paired counter-flow`, but the
   *computation of `F_primary` itself* — including the money-stock multiplier —
   must be shared too, otherwise the two authoring forms diverge on the forward
   flow. Concretely: the inline `coupled_edge` form must gate on the money stock
   the same way the node form does, or B.2's equivalence criterion must be
   narrowed in writing to "models without a money-in gate".

3. **Both forms take the same `price` primitive** — constant or node reference,
   per C.0. This is unchanged from the work package and is the reason C.0 sits
   on the critical path immediately after B.1.

## Consequences

- **B.1 grows.** It is no longer a pure no-behaviour-change refactor of one
  function; it must also unify leg discovery across the derivative and Jacobian
  paths. The "bit-identical fuzz-seed output" acceptance criterion still holds
  and is now a stronger check, because it must hold for the IDC path too. Budget
  1 day rather than the original estimate; this is on the critical path.
- **B.2 needs its equivalence criterion pinned down** before implementation.
  Either the edge form gains money-gating (more work, true equivalence) or the
  criterion is narrowed (less work, a documented asymmetry between the forms).
  Recommendation: gain the gating — an asymmetry here would be exactly the
  divergence this ADR exists to prevent, and it would surface as a wrong
  trajectory rather than an error.
- **A follow-up task is warranted** for the single-valued, carrier-string leg
  discovery: either detect and reject multiple same-carrier legs into one
  exchange node, or implement the >1 in/out the node form is supposed to offer.
  Filed separately; not a blocker for the critical path.
- **`coupled_edge` stays in the schema** and stops being a no-op. Until B.2
  lands, a model declaring `coupled_edge` silently gets no coupling — worth a
  parse-time warning in the interim.
- The quality/emergy directionality work (B.3) now has a single place to hook:
  whatever `resolve_exchange_legs` identifies as the money leg is what the
  quality pass must exclude from emergy inflow.
