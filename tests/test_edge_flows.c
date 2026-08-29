/* Per-edge flow accessors (GIP-0001 G4) — GSSK_GetFlows / GSSK_GetFlowCount.
 *
 * Until now a consumer could read every node quantity and not one rate.
 * GSSK_GetState reports the storages; nothing reported the flows between
 * them, because flow was step-local: computed inside compute_derivatives to
 * build deriv[], then discarded. That makes a diagram whose entire subject
 * is flow impossible to annotate, and leaves the heat-sink budget and any
 * pathway-level emergy display with nothing to read.
 *
 * There WAS a per-edge flow array, inside the quality pass — but it was
 * allocated and freed within the call, only ran when quality accounting was
 * enabled, and what it exposed through GSSK_GetEdgeQualityFlow is Tr x flow,
 * not flow. So the tests below pin the two properties that separate the new
 * cache from that one: it is populated with quality accounting OFF, and when
 * quality is ON the two agree exactly once Tr is divided out.
 *
 * The arithmetic is checked against a closed form rather than a golden file.
 * A linear edge carries k*Q_origin by definition, so the test can compute the
 * expected rate itself at every step, from the state the kernel reports —
 * which catches a cache written at the wrong time (a stage state, or the
 * pre-step state) that a golden file recorded once would happily enshrine.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("  FAIL: ");                            \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            failures++;                                    \
        }                                                  \
    } while (0)

#define TOL 1e-9

/* Two nodes, one linear edge, no quality_input anywhere. The absence is the
 * point: this model must still report flows. */
static const char *LINEAR_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 100.0 },"
    "    { \"id\": \"sink\", \"type\": \"storage\", \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"drain\", \"origin\": \"tank\", \"target\": \"sink\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.25 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

/* Three edges so an index mapping can actually be wrong, and a source so one
 * of them has a constant rate that does not follow the state. */
static const char *MULTI_EDGE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
    "    { \"id\": \"a\",    \"type\": \"storage\", \"value\": 50.0 },"
    "    { \"id\": \"b\",    \"type\": \"storage\", \"value\": 20.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"feed\", \"origin\": \"src\", \"target\": \"a\","
    "      \"logic\": \"constant\", \"params\": { \"k\": 3.0 } },"
    "    { \"id\": \"a_to_b\", \"origin\": \"a\", \"target\": \"b\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.4 } },"
    "    { \"id\": \"b_out\", \"origin\": \"b\", \"target\": \"a\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.1 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 3, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

/* Same shape, but the source carries quality_input, which turns the quality
 * pass on. Deliberately no forcing anywhere — compute_quality_pass reads
 * e->k rather than the forced k, so a forced edge is the one case where the
 * two legitimately disagree (tracked as quality-pass-ignores-edge-forcing). */
static const char *QUALITY_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"sun\",  \"type\": \"source\",  \"value\": 10.0,"
    "      \"quality_input\": 1.0 },"
    "    { \"id\": \"plant\", \"type\": \"storage\", \"value\": 5.0 },"
    "    { \"id\": \"soil\",  \"type\": \"storage\", \"value\": 1.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"photo\", \"origin\": \"sun\", \"target\": \"plant\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.3 } },"
    "    { \"id\": \"litter\", \"origin\": \"plant\", \"target\": \"soil\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.2 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 3, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

static GSSK_Instance *load(const char *json, const char *what) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("FATAL: %s failed to load: %d (%s)\n", what, (int)st,
               inst ? GSSK_GetErrorDescription(inst) : "");
        GSSK_Free(inst);
        exit(1);
    }
    return inst;
}

/* AC: GSSK_GetFlows()[0] == k*Q_origin at EVERY step, not just one.
 *
 * Recomputed from the state the kernel reports rather than from a recorded
 * value, so a cache refreshed at the wrong moment fails here: the post-step
 * state and the pre-step state differ by more than TOL from step 1 onward. */
static void test_linear_flow_every_step(void) {
    printf("Linear edge: flow == k * Q_origin at every step\n");
    GSSK_Instance *inst = load(LINEAR_MODEL, "LINEAR_MODEL");

    const double k = 0.25;
    CHECK(GSSK_GetFlowCount(inst) == 1, "flow count %zu, expected 1",
          GSSK_GetFlowCount(inst));

    /* Before the first step the cache is zero: no flow has been computed. */
    CHECK(fabs(GSSK_GetFlows(inst)[0]) < TOL,
          "pre-step flow %g, expected 0.0", GSSK_GetFlows(inst)[0]);

    int steps = 0;
    double last = 0.0;
    for (int i = 0; i < 50; i++) {
        if (GSSK_Step(inst, 0.1) != GSSK_SUCCESS) break;
        const double *flows = GSSK_GetFlows(inst);
        const double *state = GSSK_GetState(inst);
        double expected = k * state[0];
        CHECK(fabs(flows[0] - expected) < TOL,
              "step %d: flow %.15g, expected k*Q = %.15g (diff %g)",
              i, flows[0], expected, fabs(flows[0] - expected));
        last = flows[0];
        steps++;
    }
    CHECK(steps == 50, "only %d of 50 steps ran", steps);

    /* The trajectory has to actually move, or the assertion above is vacuous:
     * a cache stuck at its step-1 value would pass a constant-flow model. */
    CHECK(last < k * 100.0 * 0.5,
          "flow %g barely decayed from its initial %g — the model is not "
          "exercising the cache", last, k * 100.0);

    /* Reset returns the cache to zero along with the state. */
    GSSK_Reset(inst);
    CHECK(fabs(GSSK_GetFlows(inst)[0]) < TOL,
          "post-Reset flow %g, expected 0.0", GSSK_GetFlows(inst)[0]);

    GSSK_Free(inst);
}

/* AC: GSSK_GetFlowCount() == GSSK_GetEdgeCount() == edges in the input JSON,
 * and index i is the edge at position i — checked through GSSK_GetEdgeID so a
 * transposed mapping cannot pass. */
static void test_count_and_index_mapping(void) {
    printf("Flow vector length and index mapping\n");
    GSSK_Instance *inst = load(MULTI_EDGE_MODEL, "MULTI_EDGE_MODEL");

    CHECK(GSSK_GetFlowCount(inst) == 3, "flow count %zu, expected 3",
          GSSK_GetFlowCount(inst));
    CHECK(GSSK_GetFlowCount(inst) == GSSK_GetEdgeCount(inst),
          "flow count %zu != edge count %zu",
          GSSK_GetFlowCount(inst), GSSK_GetEdgeCount(inst));

    for (int i = 0; i < 10; i++)
        if (GSSK_Step(inst, 0.1) != GSSK_SUCCESS) break;

    const double *flows = GSSK_GetFlows(inst);
    const double *state = GSSK_GetState(inst);

    /* feed: constant, so k regardless of state. */
    CHECK(strcmp(GSSK_GetEdgeID(inst, 0), "feed") == 0,
          "edge 0 is '%s', expected 'feed'", GSSK_GetEdgeID(inst, 0));
    CHECK(fabs(flows[0] - 3.0) < TOL,
          "constant edge flow %.15g, expected 3.0", flows[0]);

    /* a_to_b and b_out are both linear but on different nodes with different
     * k, so swapping any two of the three indices fails at least one. */
    CHECK(strcmp(GSSK_GetEdgeID(inst, 1), "a_to_b") == 0,
          "edge 1 is '%s', expected 'a_to_b'", GSSK_GetEdgeID(inst, 1));
    CHECK(fabs(flows[1] - 0.4 * state[1]) < TOL,
          "a_to_b flow %.15g, expected %.15g", flows[1], 0.4 * state[1]);

    CHECK(strcmp(GSSK_GetEdgeID(inst, 2), "b_out") == 0,
          "edge 2 is '%s', expected 'b_out'", GSSK_GetEdgeID(inst, 2));
    CHECK(fabs(flows[2] - 0.1 * state[2]) < TOL,
          "b_out flow %.15g, expected %.15g", flows[2], 0.1 * state[2]);

    /* The three must be genuinely distinct, or an index mix-up is invisible. */
    CHECK(fabs(flows[0] - flows[1]) > 1e-6 && fabs(flows[1] - flows[2]) > 1e-6,
          "flows %g/%g/%g are too close to distinguish the indices",
          flows[0], flows[1], flows[2]);

    GSSK_Free(inst);
}

/* AC: flows are populated with quality accounting disabled.
 *
 * This is the property the pre-existing quality flow[] array could not
 * provide, so it is asserted directly rather than inferred from the tests
 * above happening to use quality-free models. */
static void test_populated_without_quality(void) {
    printf("Flows are populated with quality accounting disabled\n");
    GSSK_Instance *inst = load(MULTI_EDGE_MODEL, "MULTI_EDGE_MODEL");

    /* No node carries quality_input, so the quality pass never runs. */
    CHECK(GSSK_GetTransformationRatio(inst) == NULL,
          "quality accounting is enabled — this model cannot prove the point");
    CHECK(GSSK_GetEdgeQualityFlow(inst, 0) == 0.0,
          "edge quality flow is non-zero with quality disabled");

    GSSK_Step(inst, 0.1);

    const double *flows = GSSK_GetFlows(inst);
    CHECK(flows != NULL, "GSSK_GetFlows returned NULL with quality disabled");
    int nonzero = 0;
    for (size_t i = 0; i < GSSK_GetFlowCount(inst); i++)
        if (fabs(flows[i]) > TOL) nonzero++;
    CHECK(nonzero == 3, "%d of 3 edges report a flow with quality disabled",
          nonzero);

    GSSK_Free(inst);
}

/* AC: inactive edges report 0.0. */
static void test_inactive_edge_reports_zero(void) {
    printf("A deactivated edge reports 0.0\n");
    GSSK_Instance *inst = load(MULTI_EDGE_MODEL, "MULTI_EDGE_MODEL");

    GSSK_Step(inst, 0.1);
    CHECK(fabs(GSSK_GetFlows(inst)[1]) > TOL,
          "a_to_b already reads 0 before deactivation — the test proves nothing");

    int idx = GSSK_FindEdgeIdx(inst, "a_to_b");
    CHECK(idx == 1, "a_to_b resolved to index %d, expected 1", idx);
    GSSK_Status dst = GSSK_DeactivateEdge(inst, "a_to_b");
    CHECK(dst == GSSK_SUCCESS, "GSSK_DeactivateEdge failed: %d", (int)dst);
    GSSK_Step(inst, 0.1);

    const double *flows = GSSK_GetFlows(inst);
    CHECK(fabs(flows[1]) < TOL,
          "deactivated edge reports %.15g, expected 0.0", flows[1]);
    /* Its neighbours must be unaffected: zeroing the wrong slot, or zeroing
     * everything, would otherwise pass the line above. */
    CHECK(fabs(flows[0] - 3.0) < TOL,
          "deactivating a_to_b changed feed's flow to %.15g", flows[0]);
    CHECK(fabs(flows[2]) > TOL, "deactivating a_to_b silenced b_out too");

    GSSK_Free(inst);
}

/* AC: GSSK_GetFlows returns NULL for a NULL instance. */
static void test_null_instance(void) {
    printf("NULL instance\n");
    CHECK(GSSK_GetFlows(NULL) == NULL, "GSSK_GetFlows(NULL) is not NULL");
    CHECK(GSSK_GetFlowCount(NULL) == 0, "GSSK_GetFlowCount(NULL) is not 0");
}

/* AC: values agree with GSSK_GetEdgeQualityFlow / Tr[origin] when quality is
 * enabled — the check that stops the cache and the quality pass drifting
 * apart now that they share one flow expression. */
static void test_agrees_with_quality_flow(void) {
    printf("Flows agree with GSSK_GetEdgeQualityFlow / Tr[origin]\n");
    GSSK_Instance *inst = load(QUALITY_MODEL, "QUALITY_MODEL");

    const double *Tr = GSSK_GetTransformationRatio(inst);
    CHECK(Tr != NULL, "quality accounting is disabled — the model is wrong");
    if (!Tr) { GSSK_Free(inst); return; }

    int compared = 0;
    for (int step = 0; step < 30; step++) {
        if (GSSK_Step(inst, 0.1) != GSSK_SUCCESS) break;
        const double *flows = GSSK_GetFlows(inst);
        for (size_t i = 0; i < GSSK_GetFlowCount(inst); i++) {
            double qf = GSSK_GetEdgeQualityFlow(inst, i);
            /* Tr[origin] can legitimately be 0 (a node reached by no quality
             * input), and dividing by it proves nothing. */
            int origin = (i == 0) ? 0 : 1;
            if (Tr[origin] < 1e-9) continue;
            double implied = qf / Tr[origin];
            CHECK(fabs(implied - flows[i]) < 1e-7,
                  "step %d edge %zu: qflow/Tr = %.15g but flow = %.15g",
                  step, i, implied, flows[i]);
            compared++;
        }
    }
    CHECK(compared > 20,
          "only %d comparisons made — Tr was zero almost everywhere and the "
          "agreement was never actually tested", compared);

    GSSK_Free(inst);
}

/* An edge added at runtime has to get a cache slot, or GSSK_GetFlows reads
 * off the end of the allocation — which ASan would catch in CI and a plain
 * build would not. */
static void test_added_edge_gets_a_slot(void) {
    printf("An edge added by GSSK_AddEdge is cached too\n");
    GSSK_Instance *inst = load(LINEAR_MODEL, "LINEAR_MODEL");

    GSSK_Status st = GSSK_AddEdge(
        inst, "{ \"id\": \"back\", \"origin\": \"sink\", \"target\": \"tank\","
              "  \"logic\": \"linear\", \"params\": { \"k\": 0.05 } }");
    CHECK(st == GSSK_SUCCESS, "GSSK_AddEdge failed: %d (%s)", (int)st,
          GSSK_GetErrorDescription(inst));
    if (st != GSSK_SUCCESS) { GSSK_Free(inst); return; }

    CHECK(GSSK_GetFlowCount(inst) == 2, "flow count %zu after AddEdge, expected 2",
          GSSK_GetFlowCount(inst));
    /* Not yet stepped: the new slot reads 0.0 rather than uninitialised memory. */
    CHECK(fabs(GSSK_GetFlows(inst)[1]) < TOL,
          "new edge reads %.15g before any step, expected 0.0",
          GSSK_GetFlows(inst)[1]);

    GSSK_Step(inst, 0.1);
    const double *flows = GSSK_GetFlows(inst);
    const double *state = GSSK_GetState(inst);
    CHECK(fabs(flows[1] - 0.05 * state[1]) < TOL,
          "added edge flow %.15g, expected %.15g", flows[1], 0.05 * state[1]);

    GSSK_Free(inst);
}

/* The adaptive path is a separate step function with its own tail, so the
 * cache refresh has to be wired into it too. */
static void test_adaptive_path(void) {
    printf("The adaptive solver refreshes the cache too\n");
    GSSK_Instance *inst = load(
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 100.0 },"
        "    { \"id\": \"sink\", \"type\": \"storage\", \"value\": 0.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"id\": \"drain\", \"origin\": \"tank\", \"target\": \"sink\","
        "      \"logic\": \"linear\", \"params\": { \"k\": 0.25 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1,"
        "                \"method\": \"adaptive\" }"
        "}",
        "ADAPTIVE_MODEL");

    for (int i = 0; i < 10; i++)
        if (GSSK_StepAdaptive(inst) != GSSK_SUCCESS) break;

    const double *flows = GSSK_GetFlows(inst);
    const double *state = GSSK_GetState(inst);
    CHECK(fabs(flows[0]) > TOL, "adaptive path left the cache at zero");
    CHECK(fabs(flows[0] - 0.25 * state[0]) < TOL,
          "adaptive flow %.15g, expected k*Q = %.15g", flows[0],
          0.25 * state[0]);

    GSSK_Free(inst);
}

int main(void) {
    printf("=== Per-edge flow accessor tests (GIP-0001 G4) ===\n\n");

    test_linear_flow_every_step();
    test_count_and_index_mapping();
    test_populated_without_quality();
    test_inactive_edge_reports_zero();
    test_null_instance();
    test_agrees_with_quality_flow();
    test_added_edge_gets_a_slot();
    test_adaptive_path();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL EDGE FLOW TESTS PASSED ===\n");
    return 0;
}
