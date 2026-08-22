/* Deactivation must survive serialise -> reload.
 *
 * Found while doing the unknown-key work, and deliberately deferred out of it
 * because it is a behavioural change rather than a key-validation one (ADR
 * 0004).
 *
 * GSSK_DeactivateEdge does two things: it clears `active` AND sets k to 0.
 * build_topology_json emitted `"active": false`, and GSSK_Init accepted the
 * key without acting on it. So a round-trip produced an edge that was ACTIVE
 * WITH k = 0 rather than an inactive edge. The trajectory matched either way —
 * k = 0 kills the flow regardless — which is exactly why this survived: the
 * only thing that differed was the flag.
 *
 * The flag is not decorative. It is read at roughly twenty sites in
 * src/gssk.c, and every one of them counts elements rather than summing
 * flows: motif detection skips inactive nodes and edges,
 * network_is_isolated_duet requires exactly one active edge before it will use
 * the Riccati closed form, and the closed-system conservation check sums only
 * active nodes. A reloaded model could therefore be classified differently
 * from the model it was serialised from while producing the same numbers. For
 * a kernel whose case rests on reproducibility, and on the Phase G archival
 * story in particular, "same trajectory, different topology" is the wrong half
 * to preserve.
 *
 * GSSK_DeactivateNode was the worse of the two and was lost outright: a node
 * has no k, and build_topology_json emitted no `active` for nodes at all.
 *
 * Motif count is the observable used throughout below. It is public
 * (GSSK_GetMotifCount), it is computed from the active node and edge sets
 * alone, and it moves when the flag does — which a trajectory assertion, by
 * construction, cannot.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

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

/* Two storages and a source, so there is enough connectivity for motif
 * detection to have something to lose when an element goes inactive. */
static const char *BASE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 },"
    "    { \"id\": \"aux\",  \"type\": \"storage\", \"value\": 5.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"e1\", \"origin\": \"src\",  \"target\": \"tank\","
    "      \"logic\": \"constant\", \"params\": { \"k\": 0.5 } },"
    "    { \"id\": \"e2\", \"origin\": \"aux\",  \"target\": \"tank\","
    "      \"logic\": \"linear\",   \"params\": { \"k\": 0.2 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

#define STEPS 10

static void step_n(GSSK_Instance *inst) {
    for (int s = 0; s < STEPS; s++)
        CHECK(GSSK_Step(inst, GSSK_GetDt(inst)) == GSSK_SUCCESS, "step failed");
}

/* Serialise, reload, and hand back the reloaded instance. Fails loudly rather
 * than returning NULL, because every caller would only assert the same thing. */
static GSSK_Instance *round_trip(GSSK_Instance *inst, const char *what) {
    char *model = NULL;
    if (GSSK_SerializeModel(inst, &model) != GSSK_SUCCESS || !model) {
        CHECK(0, "%s: SerializeModel failed", what);
        return NULL;
    }
    GSSK_Instance *r = NULL;
    GSSK_Status st = GSSK_Init(model, &r);
    CHECK(st == GSSK_SUCCESS, "%s: reload failed (%d: %s)", what, (int)st,
          r ? GSSK_GetErrorDescription(r) : "no instance");
    GSSK_FreeString(model);
    if (st != GSSK_SUCCESS) { GSSK_Free(r); return NULL; }
    return r;
}

static void copy_state(GSSK_Instance *inst, double *out) {
    memcpy(out, GSSK_GetState(inst), GSSK_GetStateSize(inst) * sizeof(double));
}

/* ---------------------------------------------------------------- */

/* (1) A deactivated edge reloads as inactive.
 *
 * The assertion cannot be "the flow is zero" — it was already zero, via k.
 * Restoring k on the reloaded instance is what separates the two states: an
 * edge that is genuinely inactive stays dead when its conductance comes back,
 * and one that merely had k = 0 starts flowing again. */
static void test_deactivated_edge(void) {
    printf("Testing a deactivated edge reloads as inactive...\n");

    GSSK_Instance *orig = NULL;
    assert(GSSK_Init(BASE_MODEL, &orig) == GSSK_SUCCESS);
    assert(GSSK_DeactivateEdge(orig, "e1") == GSSK_SUCCESS);

    char *model = NULL;
    assert(GSSK_SerializeModel(orig, &model) == GSSK_SUCCESS);
    CHECK(strstr(model, "\"active\"") != NULL,
          "the serialiser must emit \"active\" for a deactivated edge");
    GSSK_FreeString(model);

    GSSK_Instance *back = round_trip(orig, "deactivated edge");
    if (!back) { GSSK_Free(orig); return; }

    /* Same treatment on both sides, so the comparison is of the flag alone. */
    GSSK_SetEdgeK(orig, 0, 0.5);
    GSSK_SetEdgeK(back, 0, 0.5);
    step_n(orig);
    step_n(back);

    const double *a = GSSK_GetState(orig), *b = GSSK_GetState(back);
    for (size_t i = 0; i < GSSK_GetStateSize(orig); i++)
        CHECK(fabs(a[i] - b[i]) < 1e-12,
              "state[%zu] diverges after restoring k: original %.12f, "
              "reloaded %.12f — the reloaded edge is live, so `active` was "
              "not restored", i, a[i], b[i]);

    CHECK(GSSK_GetMotifCount(orig) == GSSK_GetMotifCount(back),
          "motif count differs: original %zu, reloaded %zu — an inactive edge "
          "is absent from motif detection, so this is the flag, not the flow",
          GSSK_GetMotifCount(orig), GSSK_GetMotifCount(back));

    GSSK_Free(orig);
    GSSK_Free(back);
    printf("  Deactivated edge round-trips\n");
}

/* (2) The node half. Worse than the edge case before the fix: nothing was
 * emitted at all, so the deactivation did not survive in any form. */
static void test_deactivated_node(void) {
    printf("Testing a deactivated node reloads as inactive...\n");

    GSSK_Instance *orig = NULL;
    assert(GSSK_Init(BASE_MODEL, &orig) == GSSK_SUCCESS);
    assert(GSSK_DeactivateNode(orig, "aux") == GSSK_SUCCESS);

    GSSK_Instance *back = round_trip(orig, "deactivated node");
    if (!back) { GSSK_Free(orig); return; }

    step_n(orig);
    step_n(back);

    CHECK(GSSK_GetMotifCount(orig) == GSSK_GetMotifCount(back),
          "motif count differs: original %zu, reloaded %zu — the node reloaded "
          "active", GSSK_GetMotifCount(orig), GSSK_GetMotifCount(back));

    /* And against a model that never had the node deactivated, so a motif
     * count that happens to match for an unrelated reason is caught. */
    GSSK_Instance *live = NULL;
    assert(GSSK_Init(BASE_MODEL, &live) == GSSK_SUCCESS);
    step_n(live);
    CHECK(GSSK_GetMotifCount(live) != GSSK_GetMotifCount(back),
          "deactivating a node changed nothing observable (%zu motifs either "
          "way) — the assertion above proves nothing",
          GSSK_GetMotifCount(live));

    GSSK_Free(orig);
    GSSK_Free(back);
    GSSK_Free(live);
    printf("  Deactivated node round-trips\n");
}

/* (3) The case that separates a real fix from a k == 0 shortcut. An author who
 * writes k: 0 has declared an edge that carries nothing, not an edge that has
 * been removed from the network. Inferring the flag from the conductance would
 * pass tests (1) and (2) and quietly break this. */
static void test_authored_zero_k_is_active(void) {
    printf("Testing an authored k of 0.0 loads as ACTIVE...\n");

    static const char *ZERO_K =
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
        "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 },"
        "    { \"id\": \"aux\",  \"type\": \"storage\", \"value\": 5.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"id\": \"e1\", \"origin\": \"src\",  \"target\": \"tank\","
        "      \"logic\": \"constant\", \"params\": { \"k\": 0.0 } },"
        "    { \"id\": \"e2\", \"origin\": \"aux\",  \"target\": \"tank\","
        "      \"logic\": \"linear\",   \"params\": { \"k\": 0.2 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
        "}";

    GSSK_Instance *zero = NULL;
    assert(GSSK_Init(ZERO_K, &zero) == GSSK_SUCCESS);
    GSSK_Instance *live = NULL;
    assert(GSSK_Init(BASE_MODEL, &live) == GSSK_SUCCESS);

    step_n(zero);
    step_n(live);

    /* Both edges are present, so both models see the same motifs. A kernel
     * that read k == 0 as deactivation would drop e1 here and disagree. */
    CHECK(GSSK_GetMotifCount(zero) == GSSK_GetMotifCount(live),
          "an edge with an authored k of 0.0 is being treated as deactivated: "
          "%zu motifs against %zu for the same topology with k = 0.5",
          GSSK_GetMotifCount(zero), GSSK_GetMotifCount(live));

    /* The direct form: restore k and the edge flows, because it was never
     * deactivated. Contrast with test_deactivated_edge, where it must not. */
    GSSK_Instance *zero2 = NULL;
    assert(GSSK_Init(ZERO_K, &zero2) == GSSK_SUCCESS);
    GSSK_SetEdgeK(zero2, 0, 0.5);
    step_n(zero2);
    CHECK(GSSK_GetState(zero2)[1] > GSSK_GetState(zero)[1] + 1e-9,
          "restoring k on an authored-zero edge produced no flow (tank %.12f "
          "vs %.12f) — the edge loaded inactive",
          GSSK_GetState(zero2)[1], GSSK_GetState(zero)[1]);

    /* And the same model with an explicit `active: false` DOES stay dead, so
     * the two are distinguishable in both directions. Identical to ZERO_K but
     * for that one key — spelled out rather than spliced, because a splice
     * that silently missed would make this assertion vacuous. */
    static const char *ZERO_K_INACTIVE =
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
        "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 },"
        "    { \"id\": \"aux\",  \"type\": \"storage\", \"value\": 5.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"id\": \"e1\", \"origin\": \"src\",  \"target\": \"tank\","
        "      \"logic\": \"constant\", \"params\": { \"k\": 0.0 },"
        "      \"active\": false },"
        "    { \"id\": \"e2\", \"origin\": \"aux\",  \"target\": \"tank\","
        "      \"logic\": \"linear\",   \"params\": { \"k\": 0.2 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
        "}";

    GSSK_Instance *off = NULL;
    GSSK_Status st = GSSK_Init(ZERO_K_INACTIVE, &off);
    CHECK(st == GSSK_SUCCESS, "explicit active:false must load, got %d (%s)",
          (int)st, off ? GSSK_GetErrorDescription(off) : "");
    if (st == GSSK_SUCCESS) {
        GSSK_SetEdgeK(off, 0, 0.5);
        step_n(off);
        CHECK(fabs(GSSK_GetState(off)[1] - GSSK_GetState(zero)[1]) < 1e-12,
              "an edge authored with active:false flowed once k was restored "
              "(tank %.12f) — the flag was not read",
              GSSK_GetState(off)[1]);
    }

    GSSK_Free(zero);
    GSSK_Free(zero2);
    GSSK_Free(live);
    GSSK_Free(off);
    printf("  An authored k of 0.0 stays active\n");
}

/* (4) The property that already held and must not regress. Everything above
 * is about the flag; this is the reminder that the numbers were never the
 * problem, and that restoring the flag has not disturbed them. */
static void test_trajectory_still_matches(void) {
    printf("Testing trajectory equality across the round-trip...\n");

    struct { const char *what; int node; const char *id; } cases[] = {
        { "no deactivation",   0, NULL  },
        { "deactivated edge",  0, "e1"  },
        { "deactivated node",  1, "aux" },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        GSSK_Instance *orig = NULL;
        assert(GSSK_Init(BASE_MODEL, &orig) == GSSK_SUCCESS);
        if (cases[c].id) {
            GSSK_Status st = cases[c].node
                ? GSSK_DeactivateNode(orig, cases[c].id)
                : GSSK_DeactivateEdge(orig, cases[c].id);
            assert(st == GSSK_SUCCESS);
        }

        GSSK_Instance *back = round_trip(orig, cases[c].what);
        if (!back) { GSSK_Free(orig); continue; }

        step_n(orig);
        step_n(back);

        size_t n = GSSK_GetStateSize(orig);
        CHECK(GSSK_GetStateSize(back) == n, "%s: state size %zu -> %zu",
              cases[c].what, n, GSSK_GetStateSize(back));
        double *a = malloc(n * sizeof(double));
        copy_state(orig, a);
        for (size_t i = 0; i < n && i < GSSK_GetStateSize(back); i++)
            CHECK(fabs(a[i] - GSSK_GetState(back)[i]) < 1e-12,
                  "%s: state[%zu] %.15f -> %.15f", cases[c].what, i, a[i],
                  GSSK_GetState(back)[i]);
        free(a);

        GSSK_Free(orig);
        GSSK_Free(back);
    }
    printf("  Trajectories match\n");
}

/* (5) The runtime add paths are separate parsers, the same reason
 * test_unknown_keys.c covers both. GSSK_AddEdge/AddNode accept `active` in
 * their fragment, so an editor replaying a serialised element gets the element
 * it serialised. */
static void test_runtime_adds_honour_active(void) {
    printf("Testing GSSK_AddNode / GSSK_AddEdge honour `active`...\n");

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);
    CHECK(GSSK_AddNode(inst,
            "{\"id\":\"spare\",\"type\":\"storage\",\"value\":2.0,\"active\":false}")
          == GSSK_SUCCESS, "AddNode must accept active:false: %s",
          GSSK_GetErrorDescription(inst));
    CHECK(GSSK_AddEdge(inst,
            "{\"id\":\"e3\",\"origin\":\"tank\",\"target\":\"spare\","
            "\"logic\":\"linear\",\"params\":{\"k\":0.4},\"active\":false}")
          == GSSK_SUCCESS, "AddEdge must accept active:false: %s",
          GSSK_GetErrorDescription(inst));
    step_n(inst);

    /* Added inactive, so `spare` took nothing from `tank`. */
    CHECK(fabs(GSSK_GetState(inst)[3] - 2.0) < 1e-12,
          "an edge added with active:false still flowed: spare = %.12f",
          GSSK_GetState(inst)[3]);

    GSSK_Instance *back = round_trip(inst, "after AddNode/AddEdge");
    if (back) {
        step_n(back);
        CHECK(GSSK_GetMotifCount(inst) == GSSK_GetMotifCount(back),
              "motif count differs after a runtime add: %zu -> %zu",
              GSSK_GetMotifCount(inst), GSSK_GetMotifCount(back));
        GSSK_Free(back);
    }
    GSSK_Free(inst);
    printf("  Runtime adds honour `active`\n");
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== Deactivation round-trip tests ===\n\n");

    test_deactivated_edge();
    test_deactivated_node();
    test_authored_zero_k_is_active();
    test_trajectory_still_matches();
    test_runtime_adds_honour_active();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL DEACTIVATION ROUND-TRIP TESTS PASSED ===\n");
    return 0;
}
