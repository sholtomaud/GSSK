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

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
