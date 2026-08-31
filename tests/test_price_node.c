/* tests/test_price_node.c — Phase C.0
 *
 * Covers the parts of `price_node` that the CSV regression fixtures cannot:
 * serialization round-trip, forward-reference resolution, and the fallback to
 * the constant price when no reference is given.
 *
 * Build: make test-price-node
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gssk.h"

static int failures = 0;

static void check(int cond, const char *what) {
  printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}

/* price node declared AFTER the exchange node that reads it */
static const char *MODEL_FORWARD_REF =
  "{\"metadata\":{\"schema_version\":4,\"name\":\"pn\"},"
  "\"carriers\":[{\"id\":\"money\",\"unit\":\"AUD\",\"conserved\":true},"
                "{\"id\":\"goods\",\"unit\":\"kg\",\"conserved\":true}],"
  "\"nodes\":["
    "{\"id\":\"seller\",\"type\":\"source\",\"value\":1.0,\"carrier\":\"goods\"},"
    "{\"id\":\"buyer\",\"type\":\"storage\",\"value\":1000.0,\"carrier\":\"money\"},"
    "{\"id\":\"ex\",\"type\":\"exchange\",\"value\":0.0,\"carrier\":\"goods\","
      "\"params\":{\"k\":0.01,\"price\":999.0,\"price_node\":\"price\"}},"
    "{\"id\":\"inventory\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"goods\"},"
    "{\"id\":\"spent\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"money\"},"
    "{\"id\":\"price\",\"type\":\"constant\",\"value\":5.0,\"carrier\":\"money\"}],"
  "\"edges\":["
    "{\"id\":\"e1\",\"origin\":\"seller\",\"target\":\"ex\",\"carrier\":\"goods\"},"
    "{\"id\":\"e2\",\"origin\":\"buyer\",\"target\":\"ex\",\"carrier\":\"money\"},"
    "{\"id\":\"e3\",\"origin\":\"ex\",\"target\":\"inventory\",\"carrier\":\"goods\"},"
    "{\"id\":\"e4\",\"origin\":\"ex\",\"target\":\"spent\",\"carrier\":\"money\"}],"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

/* identical, but with a plain scalar price and no reference */
static const char *MODEL_SCALAR =
  "{\"metadata\":{\"schema_version\":4,\"name\":\"scalar\"},"
  "\"carriers\":[{\"id\":\"money\",\"unit\":\"AUD\",\"conserved\":true},"
                "{\"id\":\"goods\",\"unit\":\"kg\",\"conserved\":true}],"
  "\"nodes\":["
    "{\"id\":\"seller\",\"type\":\"source\",\"value\":1.0,\"carrier\":\"goods\"},"
    "{\"id\":\"buyer\",\"type\":\"storage\",\"value\":1000.0,\"carrier\":\"money\"},"
    "{\"id\":\"ex\",\"type\":\"exchange\",\"value\":0.0,\"carrier\":\"goods\","
      "\"params\":{\"k\":0.01,\"price\":5.0}},"
    "{\"id\":\"inventory\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"goods\"},"
    "{\"id\":\"spent\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"money\"}],"
  "\"edges\":["
    "{\"id\":\"e1\",\"origin\":\"seller\",\"target\":\"ex\",\"carrier\":\"goods\"},"
    "{\"id\":\"e2\",\"origin\":\"buyer\",\"target\":\"ex\",\"carrier\":\"money\"},"
    "{\"id\":\"e3\",\"origin\":\"ex\",\"target\":\"inventory\",\"carrier\":\"goods\"},"
    "{\"id\":\"e4\",\"origin\":\"ex\",\"target\":\"spent\",\"carrier\":\"money\"}],"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

/* Two instances of one archetype, each transacting at its own price.  The
 * template's scalar price is a poison 999.0 and its `price` member starts at
 * 0.0, so a template price_node that never resolves shows up as either a
 * runaway spend or no spend at all — never as a plausible number.  The two
 * per-instance prices arrive through snapshot.state on the expanded member
 * ids, which is the only channel that keeps price inside the hashed model
 * document. */
static const char *MODEL_ARCH_FMT =
  "{\"metadata\":{\"schema_version\":4,\"name\":\"arch_pn\"},"
  "\"carriers\":[{\"id\":\"money\",\"unit\":\"AUD\",\"conserved\":true},"
                "{\"id\":\"goods\",\"unit\":\"kg\",\"conserved\":true}],"
  "\"archetypes\":{\"purchase\":{"
    "\"nodes\":["
      "{\"id\":\"price\",\"type\":\"constant\",\"value\":0.0,\"carrier\":\"money\"},"
      "{\"id\":\"deal\",\"type\":\"exchange\",\"value\":0.0,\"carrier\":\"goods\","
        "\"params\":{\"k\":0.01,\"price\":999.0,\"price_node\":\"price\"}},"
      "{\"id\":\"inventory\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"goods\"},"
      "{\"id\":\"spent\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"money\"}],"
    "\"edges\":["
      "{\"id\":\"to_inv\",\"origin\":\"deal\",\"target\":\"inventory\",\"carrier\":\"goods\"},"
      "{\"id\":\"to_spent\",\"origin\":\"deal\",\"target\":\"spent\",\"carrier\":\"money\"}],"
    "\"ports\":{\"in\":\"deal\",\"out\":\"inventory\"}}},"
  "\"nodes\":["
    "{\"id\":\"seller\",\"type\":\"source\",\"value\":1.0,\"carrier\":\"goods\"},"
    "{\"id\":\"buyer\",\"type\":\"storage\",\"value\":1000.0,\"carrier\":\"money\"},"
    "{\"id\":\"a\",\"type\":\"purchase\",\"value\":0.0},"
    "{\"id\":\"b\",\"type\":\"purchase\",\"value\":0.0}],"
  "\"edges\":["
    "{\"id\":\"ga\",\"origin\":\"seller\",\"target\":\"a__deal\",\"carrier\":\"goods\"},"
    "{\"id\":\"ma\",\"origin\":\"buyer\",\"target\":\"a__deal\",\"carrier\":\"money\"},"
    "{\"id\":\"gb\",\"origin\":\"seller\",\"target\":\"b__deal\",\"carrier\":\"goods\"},"
    "{\"id\":\"mb\",\"origin\":\"buyer\",\"target\":\"b__deal\",\"carrier\":\"money\"}],"
  "\"snapshot\":{\"state\":[{\"id\":\"a__price\",\"Q\":%g},{\"id\":\"b__price\",\"Q\":%g}]},"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

/* Same archetype, but the template's price_node names a member that does not
 * exist.  A name matching nothing is a schema error, not a silent fallback to
 * the constant — ADR-0004's rule, and the property a ledger depends on. */
static const char *MODEL_ARCH_BAD_REF =
  "{\"metadata\":{\"schema_version\":4,\"name\":\"arch_pn_bad\"},"
  "\"carriers\":[{\"id\":\"money\",\"unit\":\"AUD\",\"conserved\":true},"
                "{\"id\":\"goods\",\"unit\":\"kg\",\"conserved\":true}],"
  "\"archetypes\":{\"purchase\":{"
    "\"nodes\":["
      "{\"id\":\"price\",\"type\":\"constant\",\"value\":5.0,\"carrier\":\"money\"},"
      "{\"id\":\"deal\",\"type\":\"exchange\",\"value\":0.0,\"carrier\":\"goods\","
        "\"params\":{\"k\":0.01,\"price\":999.0,\"price_node\":\"nope\"}},"
      "{\"id\":\"inventory\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"goods\"},"
      "{\"id\":\"spent\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"money\"}],"
    "\"edges\":["
      "{\"id\":\"to_inv\",\"origin\":\"deal\",\"target\":\"inventory\",\"carrier\":\"goods\"},"
      "{\"id\":\"to_spent\",\"origin\":\"deal\",\"target\":\"spent\",\"carrier\":\"money\"}],"
    "\"ports\":{\"in\":\"deal\",\"out\":\"inventory\"}}},"
  "\"nodes\":["
    "{\"id\":\"seller\",\"type\":\"source\",\"value\":1.0,\"carrier\":\"goods\"},"
    "{\"id\":\"buyer\",\"type\":\"storage\",\"value\":1000.0,\"carrier\":\"money\"},"
    "{\"id\":\"a\",\"type\":\"purchase\",\"value\":0.0}],"
  "\"edges\":["
    "{\"id\":\"ga\",\"origin\":\"seller\",\"target\":\"a__deal\",\"carrier\":\"goods\"},"
    "{\"id\":\"ma\",\"origin\":\"buyer\",\"target\":\"a__deal\",\"carrier\":\"money\"}],"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

/* Post-run values read back from the two-instance model. */
enum { A_SPENT, B_SPENT, A_INV, B_INV, BUYER, N_ARCH_VALS };
static const char *ARCH_IDS[N_ARCH_VALS] = {
  "a__spent", "b__spent", "a__inventory", "b__inventory", "buyer"
};

static int run_arch(double price_a, double price_b, double *vals) {
  char json[4096];
  GSSK_Instance *inst = NULL;
  snprintf(json, sizeof(json), MODEL_ARCH_FMT, price_a, price_b);
  if (GSSK_Init(json, &inst) != GSSK_SUCCESS) return 0;
  for (int i = 0; i < 10; i++) GSSK_Step(inst, 0.1);
  const double *st = GSSK_GetState(inst);
  for (int j = 0; j < N_ARCH_VALS; j++) {
    int idx = GSSK_FindNodeIdx(inst, ARCH_IDS[j]);
    vals[j] = (idx >= 0) ? st[idx] : -1.0;
  }
  GSSK_Free(inst);
  return 1;
}

static double run_to_end(const char *json, const char *node_id) {
  GSSK_Instance *inst = NULL;
  if (GSSK_Init(json, &inst) != GSSK_SUCCESS) return -1.0;
  for (int i = 0; i < 10; i++) GSSK_Step(inst, 0.1);
  int idx = GSSK_FindNodeIdx(inst, node_id);
  double v = (idx >= 0) ? GSSK_GetState(inst)[idx] : -1.0;
  GSSK_Free(inst);
  return v;
}

int main(void) {
  printf("Phase C.0 — price_node\n");

  /* 1. Forward reference: price node declared after the exchange that reads it.
   *    The scalar price is a poison 999.0, so if resolution failed the money
   *    spend would be ~200x larger. */
  double spent_ref    = run_to_end(MODEL_FORWARD_REF, "spent");
  double spent_scalar = run_to_end(MODEL_SCALAR,      "spent");
  check(spent_ref > 0.0, "forward-referenced price_node model runs");
  check(spent_scalar > 0.0, "scalar-price control model runs");

  /* 2. Equivalence: a price_node held at 5.0 must match a scalar price of 5.0
   *    bit-for-bit — same physics, different authoring. */
  check(spent_ref == spent_scalar,
        "price_node(5.0) identical to scalar price 5.0");

  /* 3. The poison value proves the reference is actually consulted. */
  check(spent_ref < 100.0, "poison scalar price 999.0 was not used");

  /* 4. Round-trip: price_node must survive serialization, not silently
   *    collapse to the constant fallback. */
  {
    GSSK_Instance *inst = NULL;
    char *out = NULL;
    check(GSSK_Init(MODEL_FORWARD_REF, &inst) == GSSK_SUCCESS, "init for serialize");
    if (inst && GSSK_SerializeModel(inst, &out) == GSSK_SUCCESS && out) {
      check(strstr(out, "\"price_node\"") != NULL,
            "serialized model emits price_node");
      check(strstr(out, "\"price_node\":\"price\"") != NULL ||
            strstr(out, "\"price_node\":\t\"price\"") != NULL,
            "price_node round-trips to the referenced id");
      GSSK_FreeString(out);
    } else {
      check(0, "serialize succeeded");
      check(0, "price_node round-trips to the referenced id");
    }
    if (inst) GSSK_Free(inst);
  }

  /* 5. Absent price_node falls back to the constant, and must NOT be treated
   *    as a reference to node index 0 (the zero-init trap). */
  {
    GSSK_Instance *inst = NULL;
    check(GSSK_Init(MODEL_SCALAR, &inst) == GSSK_SUCCESS, "init scalar model");
    if (inst) {
      char *out = NULL;
      if (GSSK_SerializeModel(inst, &out) == GSSK_SUCCESS && out) {
        check(strstr(out, "\"price_node\"") == NULL,
              "scalar model emits no price_node key");
        GSSK_FreeString(out);
      } else {
        check(0, "scalar model emits no price_node key");
      }
      GSSK_Free(inst);
    }
  }

  /* 6. Per-instance price across expanded archetype members (GIP-0002).
   *    A template's price_node names a sibling member; expansion rewrites it
   *    to <instance>__<member>, so two instances of one archetype can trade
   *    at two different prices.  Before the rewrite existed, price_idx stayed
   *    -1 on every member and both instances moved the poison constant. */
  {
    double diff[N_ARCH_VALS], same[N_ARCH_VALS];
    int ok_diff = run_arch(5.0, 10.0, diff);
    int ok_same = run_arch(5.0,  5.0, same);
    check(ok_diff, "archetype model with per-instance prices runs");
    check(ok_same, "archetype model with equal prices runs");

    if (ok_diff && ok_same) {
      check(diff[A_SPENT] > 0.0 && diff[B_SPENT] > 0.0,
            "both instances move money");
      check(diff[A_SPENT] != diff[B_SPENT],
            "instances at different prices spend different amounts");
      check(same[A_SPENT] == same[B_SPENT],
            "instances at the same price spend the same amount");
      check(diff[A_SPENT] < 100.0 && diff[B_SPENT] < 100.0,
            "template poison price 999.0 was not used by either instance");

      /* The double entry: money credited to each instance's spent sink is
       * price x goods delivered to that instance's inventory.  Asserted per
       * instance, because an aggregate that balances can still hide two
       * instances transacting at the wrong prices. */
      check(fabs(diff[A_SPENT] / diff[A_INV] -  5.0) < 1e-9,
            "instance a: spent/inventory == its own price (5.0)");
      check(fabs(diff[B_SPENT] / diff[B_INV] - 10.0) < 1e-9,
            "instance b: spent/inventory == its own price (10.0)");

      /* Money is conserved across the two diamonds: every unit debited from
       * the shared buyer is credited to exactly one instance's sink. */
      check(fabs((1000.0 - diff[BUYER]) - (diff[A_SPENT] + diff[B_SPENT])) < 1e-9,
            "buyer debit equals the sum of the two instances' credits");
    } else {
      check(0, "both instances move money");
      check(0, "instances at different prices spend different amounts");
      check(0, "instances at the same price spend the same amount");
      check(0, "template poison price 999.0 was not used by either instance");
      check(0, "instance a: spent/inventory == its own price (5.0)");
      check(0, "instance b: spent/inventory == its own price (10.0)");
      check(0, "buyer debit equals the sum of the two instances' credits");
    }
  }

  /* 7. A template price_node naming no sibling member is a schema error.  The
   *    silent fallback is what made this defect invisible in the first place;
   *    for an accounting model a mispriced entry must not load at all. */
  {
    GSSK_Instance *inst = NULL;
    check(GSSK_Init(MODEL_ARCH_BAD_REF, &inst) == GSSK_ERR_SCHEMA_VIOLATION,
          "unresolvable template price_node is rejected");
    if (inst) GSSK_Free(inst);
  }

  /* 8. Round-trip of an expanded member.  The serialiser writes the resolved
   *    node's id, which post-expansion is the namespaced `a__price`, and the
   *    snapshot form carries the per-instance prices in its state block —
   *    GSSK_SerializeModel emits initial_value (the topology IC), so a price
   *    delivered through snapshot.state lives in the snapshot form, which is
   *    the one that round-trips state by definition. */
  {
    char json[4096];
    GSSK_Instance *inst = NULL;
    char *out = NULL;
    double first[N_ARCH_VALS];

    (void)run_arch(5.0, 10.0, first);
    snprintf(json, sizeof(json), MODEL_ARCH_FMT, 5.0, 10.0);
    check(GSSK_Init(json, &inst) == GSSK_SUCCESS, "init archetype model for serialize");

    /* Serialised before stepping, so the re-initialised instance starts where
     * this one does and the two trajectories are comparable. */
    if (inst && GSSK_SerializeSnapshot(inst, &out) == GSSK_SUCCESS && out) {
      check(strstr(out, "\"price_node\":\t\"a__price\"") != NULL ||
            strstr(out, "\"price_node\":\"a__price\"") != NULL,
            "expanded member serialises its namespaced price_node");

      GSSK_Instance *round = NULL;
      if (GSSK_Init(out, &round) == GSSK_SUCCESS) {
        for (int i = 0; i < 10; i++) GSSK_Step(round, 0.1);
        int ia = GSSK_FindNodeIdx(round, "a__spent");
        int ib = GSSK_FindNodeIdx(round, "b__spent");
        const double *st = GSSK_GetState(round);
        check(ia >= 0 && ib >= 0 &&
              st[ia] == first[A_SPENT] && st[ib] == first[B_SPENT],
              "re-initialised snapshot reproduces both instances' spend");
        GSSK_Free(round);
      } else {
        check(0, "re-initialised snapshot reproduces both instances' spend");
      }
      GSSK_FreeString(out);
    } else {
      check(0, "expanded member serialises its namespaced price_node");
      check(0, "re-initialised snapshot reproduces both instances' spend");
    }
    if (inst) GSSK_Free(inst);
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
