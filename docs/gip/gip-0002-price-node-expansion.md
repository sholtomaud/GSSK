# GIP 0002 — Resolve `price_node` for expanded archetype members

- **Status**: proposed
- **Target**: [sholtomaud/GSSK](https://github.com/sholtomaud/GSSK)
- **Raised from**: gssk-budget, task `p0a-exchange-price-per-instance`
- **Affects**: GSSK v5.1.0 (`dist-v5.1.0`, commit `3ca217e4`)
- **Kind**: defect in composite expansion, not a feature request

## Summary

An `exchange` node inside an archetype cannot have a per-instance price. Every
instance of the archetype shares the template's constant, and `price_node` —
the mechanism that exists precisely to make price a state variable rather than
a constant — does not resolve for expanded members.

The consequence for a consumer is a model that validates, loads, runs to
completion and reports success while moving no money across any of its
transaction diamonds.

## What GSSK does today

`GSSK_Init` resolves `price_node` in a second pass, after all nodes exist
(`src/gssk.c`, "Second pass: resolve `price_node` references"):

```c
for (int i = 0; i < n_json_nodes; i++) {
  cJSON *node = cJSON_GetArrayItem(nodes_arr, i);
  cJSON *nid  = cJSON_GetObjectItem(node, "id");
  cJSON *np   = cJSON_GetObjectItem(node, "params");
  if (!cJSON_IsString(nid) || !cJSON_IsObject(np)) continue;
  cJSON *pn = cJSON_GetObjectItem(np, "price_node");
  if (!cJSON_IsString(pn)) continue;
  int slot = find_node_idx(inst, nid->valuestring);
  if (slot >= 0)
    inst->nodes[slot].price_idx = find_node_idx(inst, pn->valuestring);
}
```

The loop iterates `nodes_arr` — the **top-level** `nodes` array of the model
document. For a composite, the entry there is the instance (`{"id": "groceries",
"type": "purchase_consumed"}`), whose `params` carry no `price_node`. The
expanded members, which do, are in `inst->nodes` and are never visited.

So a `price_node` written inside an archetype template is silently ignored,
`price_idx` stays `-1`, and the comment in the source is accurate about the
consequence: it "falls back to the constant price".

## Why the workarounds do not work

- **The template's `price` is shared.** A template's `params` belong to the
  template, so every instance gets the same number. That is correct for `k` on
  a shape, and wrong for a price, which is a property of the item.
- **`snapshot` has no channel for node params.** `snapshot.state` sets a node's
  `Q` and `snapshot.edge_k` sets an edge's `k`. There is no equivalent for
  `params.price`, so a consumer cannot supply it in the model document — which
  is where it has to be, because the model content hash is what identifies the
  model a forecast came from.
- **No setter exists.** The WASM surface has `GSSK_SetEdgeK` and
  `GSSK_SetSeed`, but nothing for a node parameter. Even if it did, setting the
  price after `GSSK_Init` would put it outside the hashed document, so two
  models differing only in price would hash identically.

The result is that an archetype containing an `exchange` node is only usable
when every instance of it transacts at the same price, which is not a case
anybody has.

## Proposal

Rewrite `price_node` during composite expansion, exactly as edge endpoints are
already rewritten.

Expansion composes member ids as `"%.29s__%.29s"` and already rewrites an
edge's `origin` and `target` through that same composition. A template's
`price_node` names a sibling member by its template-local id, so the identical
rewrite applies:

```
archetype purchase_consumed:
  nodes:
    - { id: price, type: constant, value: 0 }
    - { id: deal,  type: exchange, params: { price_node: "price" } }

instance "groceries" expands to:
    groceries__price   (constant)
    groceries__deal    (exchange, price_node -> groceries__price)
```

The per-instance price is then supplied through `snapshot.state` on
`groceries__price`, which is already applied by `GSSK_Init`, already inside the
hashed document, and needs no new mechanism.

Two implementation notes:

1. The rewrite belongs in expansion, where `def` and the instance id are both
   in hand — not in the second pass, which by then cannot tell a top-level id
   from an expanded one.
2. The second pass should keep working unchanged for top-level nodes. This adds
   a case; it does not replace one.

## Alternatives considered

- **A `snapshot.node_params` block.** More general, and it would cover `k`, `C`
  and `threshold` on nodes too. But it is a schema change (schema v5), where the
  proposal above is a parser fix against schema v4. Worth doing eventually;
  disproportionate for this.
- **`GSSK_SetNodePrice`.** Puts the value outside the hashed model, which breaks
  the property that a content hash identifies the model a forecast came from.
  Rejected for that reason rather than for effort.

## Compatibility

Additive. A template with no `price_node` behaves exactly as now, and a
top-level `price_node` resolves exactly as now. The only behaviour that changes
is a case that is currently silently broken, so nothing can depend on it.

Any model relying on the current behaviour would be relying on its price being
ignored, which is not a behaviour worth preserving.

## Suggested test

`tests/` should gain a model with one archetype containing an `exchange` and a
`constant` price member, instantiated twice with different `snapshot.state`
values for the two price nodes, asserting the two instances move different
amounts of money. Today that test fails with both moving zero.

## Consequence for gssk-budget if this is declined

`purchase_to_stock` and `purchase_consumed` would have to be hand-assembled as
top-level primitives, which contradicts §3.1 of the requirements ("the
application must use [the archetypes block] rather than hand-assembling
primitives per item") and would need that requirement amended. The founder
model currently uses `transfer_expense` as a stopgap, and says so in a comment.
