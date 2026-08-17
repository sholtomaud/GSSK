#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>

void test_calibration() {
    printf("Testing Parameter Calibration...\n");

    const char *model_json = "{"
        "\"nodes\": ["
        "  {\"id\": \"A\", \"type\": \"source\", \"value\": 10.0},"
        "  {\"id\": \"B\", \"type\": \"storage\", \"value\": 0.0}"
        "],"
        "\"edges\": ["
        "  {\"origin\": \"A\", \"target\": \"B\", \"logic\": \"linear\", \"params\": {\"k\": 0.5}}"
        "],"
        "\"config\": {\"t_start\": 0, \"t_end\": 10, \"dt\": 1.0}"
        "}";

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(model_json, &inst) == GSSK_SUCCESS);

    // Target: We want B to reach ~50 at t=10.
    // Analytical solution for B' = k*A (where A is constant source 10):
    // B(t) = 10 * k * t
    // For B(10) = 50, 10 * k * 10 = 50 => 100k = 50 => k = 0.5.
    // Let's set a target that corresponds to k=0.8 => B(10) = 80.

    GSSK_Observation obs_data[] = {
        {5.0, 40.0},  // 10 * 0.8 * 5 = 40
        {10.0, 80.0}  // 10 * 0.8 * 10 = 80
    };

    GSSK_NodeObservations node_obs = {
        .node_id = "B",
        .data = obs_data,
        .count = 2
    };

    printf("  Initial k: %f\n", GSSK_GetEdgeK(inst, 0));
    GSSK_Status status = GSSK_Calibrate(inst, &node_obs, 1, 100);
    assert(status == GSSK_SUCCESS);

    double calibrated_k = GSSK_GetEdgeK(inst, 0);
    printf("  Calibrated k: %f (Expected ~0.8)\n", calibrated_k);

    assert(fabs(calibrated_k - 0.8) < 0.1);

    GSSK_Free(inst);
    printf("  Calibration test PASSED\n");
}

void test_ensemble() {
    printf("Testing Ensemble Forecasting...\n");

    const char *model_json = "{"
        "\"nodes\": ["
        "  {\"id\": \"Source\", \"type\": \"source\", \"value\": 10.0},"
        "  {\"id\": \"Stock\", \"type\": \"storage\", \"value\": 0.0}"
        "],"
        "\"edges\": ["
        "  {\"origin\": \"Source\", \"target\": \"Stock\", \"logic\": \"linear\", \"params\": {\"k\": 1.0}}"
        "],"
        "\"config\": {\"t_start\": 0, \"t_end\": 10, \"dt\": 1.0}"
        "}";

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(model_json, &inst) == GSSK_SUCCESS);

    GSSK_SetSeed(inst, 42); // Seed for deterministic test (libc srand no longer applies)
    GSSK_EnsembleResult *res = GSSK_EnsembleForecast(inst, 10, 0.2); // 20% perturbation
    assert(res != NULL);
    assert(res->node_count == 2);
    assert(res->step_count == 11);

    // Stock is at index 1
    size_t stock_idx = 1;
    size_t final_step_idx = 10 * res->node_count + stock_idx;

    printf("  t=10 Mean: %f, Min: %f, Max: %f\n",
           res->mean_envelope[final_step_idx], res->min_envelope[final_step_idx], res->max_envelope[final_step_idx]);

    assert(res->max_envelope[final_step_idx] >= res->mean_envelope[final_step_idx]);
    assert(res->mean_envelope[final_step_idx] >= res->min_envelope[final_step_idx]);

    GSSK_FreeEnsembleResult(res);
    GSSK_Free(inst);
    printf("  Ensemble test PASSED\n");
}

/* Phase 7 — interaction node smoke test.  Uses examples/interaction_model.json
 * topology inline: A + B → gate (interaction) → C; src → converter (loop_limited)
 * → D.  Both C and D should monotonically increase from 0 once a few RK4
 * steps run. */
static const char *PHASE7_MODEL =
    "{\n"
    "  \"metadata\": { \"schema_version\": 4 },\n"
    "  \"nodes\": [\n"
    "    { \"id\": \"A\",    \"type\": \"source\",       \"value\": 10.0 },\n"
    "    { \"id\": \"B\",    \"type\": \"source\",       \"value\": 5.0  },\n"
    "    { \"id\": \"gate\", \"type\": \"interaction\",  \"value\": 0.0, \"params\": { \"k\": 0.01 } },\n"
    "    { \"id\": \"C\",    \"type\": \"storage\",      \"value\": 0.0  },\n"
    "    { \"id\": \"src\",  \"type\": \"source\",       \"value\": 20.0 },\n"
    "    { \"id\": \"converter\", \"type\": \"loop_limited\", \"value\": 0.0, \"params\": { \"k\": 3.0, \"C\": 5.0 } },\n"
    "    { \"id\": \"D\",    \"type\": \"storage\",      \"value\": 0.0  },\n"
    "    { \"id\": \"heat\", \"type\": \"sink\",         \"value\": 0.0  }\n"
    "  ],\n"
    "  \"edges\": [\n"
    "    { \"origin\": \"A\",        \"target\": \"gate\"      },\n"
    "    { \"origin\": \"B\",        \"target\": \"gate\"      },\n"
    "    { \"origin\": \"gate\",     \"target\": \"C\"         },\n"
    "    { \"origin\": \"C\",        \"target\": \"heat\",       \"logic\": \"linear\", \"params\": { \"k\": 0.05 } },\n"
    "    { \"origin\": \"src\",      \"target\": \"converter\" },\n"
    "    { \"origin\": \"converter\",\"target\": \"D\"         },\n"
    "    { \"origin\": \"D\",        \"target\": \"heat\",       \"logic\": \"linear\", \"params\": { \"k\": 0.02 } }\n"
    "  ],\n"
    "  \"config\": { \"t_start\": 0, \"t_end\": 50, \"dt\": 0.1, \"method\": \"rk4\" }\n"
    "}";

void test_interaction_node() {
    printf("Testing Phase 7 interaction node...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE7_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    int c_idx = GSSK_FindNodeIdx(inst, "C");
    assert(c_idx >= 0);

    double C0 = GSSK_GetState(inst)[c_idx];
    for (int i = 0; i < 10; i++) {
        st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }
    double C1 = GSSK_GetState(inst)[c_idx];
    printf("  C: %.4f -> %.4f\n", C0, C1);
    assert(C1 > C0);

    GSSK_Free(inst);
    printf("  Interaction node test PASSED\n");
}

void test_loop_limited_node() {
    printf("Testing Phase 7 loop_limited node...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE7_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    int d_idx = GSSK_FindNodeIdx(inst, "D");
    assert(d_idx >= 0);

    double D0 = GSSK_GetState(inst)[d_idx];
    for (int i = 0; i < 10; i++) {
        st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }
    double D1 = GSSK_GetState(inst)[d_idx];
    printf("  D: %.4f -> %.4f\n", D0, D1);
    assert(D1 > D0);
    /* Saturation: F_max = k*Q/(1+Q/C) = 3*20/(1+20/5) = 12; over 10 steps
     * (dt=0.1) that's ≤ 12.  Allow generous upper bound. */
    assert(D1 < 25.0);

    GSSK_Free(inst);
    printf("  Loop-limited node test PASSED\n");
}

/* =========================================================================
 * Phase 8 — Composite Node Types & Archetype System
 * ========================================================================= */

static const char *PHASE8_PRODUCER_MODEL =
    "{\n"
    "  \"metadata\": { \"schema_version\": 4 },\n"
    "  \"nodes\": [\n"
    "    { \"id\": \"sun\", \"type\": \"source\", \"value\": 1.0 },\n"
    "    { \"id\": \"plant\", \"type\": \"producer\", \"value\": 50.0,\n"
    "      \"params\": { \"k_production\": 0.01, \"k_respiration\": 0.02 } },\n"
    "    { \"id\": \"heat\", \"type\": \"sink\", \"value\": 0.0 }\n"
    "  ],\n"
    "  \"edges\": [\n"
    "    { \"origin\": \"sun\", \"target\": \"plant\", \"logic\": \"linear\","
    "      \"params\": { \"k\": 5.0 } }\n"
    "  ],\n"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1, \"method\": \"rk4\" }\n"
    "}";

void test_producer_composite() {
    printf("Testing Phase 8 producer composite...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE8_PRODUCER_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    /* 4 built-in archetypes registered */
    size_t arch_count = GSSK_GetArchetypeCount(inst);
    printf("  archetype count = %zu\n", arch_count);
    assert(arch_count >= 4);

    /* Exactly one composite expansion (plant) */
    assert(GSSK_GetCompositeCount(inst) == 1);
    const char *cid = GSSK_GetCompositeID(inst, 0);
    assert(cid && strcmp(cid, "plant") == 0);

    /* plant expanded to plant__body, plant__gate, plant__heat */
    int body_idx = GSSK_FindNodeIdx(inst, "plant__body");
    int gate_idx = GSSK_FindNodeIdx(inst, "plant__gate");
    int phid_idx = GSSK_FindNodeIdx(inst, "plant__heat");
    assert(body_idx >= 0);
    assert(gate_idx >= 0);
    assert(phid_idx >= 0);

    /* Run 10 steps; production (k·Q²) > respiration (0.02·Q) for Q=50 ⇒ growth.
     * k_production = 0.01: F_prod = 0.01·50·50 = 25; F_resp = 0.02·50 = 1.
     * Net body inflow is dominated by production. */
    double Q0 = GSSK_GetState(inst)[body_idx];
    for (int i = 0; i < 10; i++) {
        st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }
    double Q1 = GSSK_GetState(inst)[body_idx];
    printf("  plant__body: %.4f -> %.4f\n", Q0, Q1);
    assert(Q1 > Q0);

    GSSK_Free(inst);
    printf("  Producer composite test PASSED\n");
}

static const char *PHASE8_CONSUMER_MODEL =
    "{\n"
    "  \"metadata\": { \"schema_version\": 4 },\n"
    "  \"nodes\": [\n"
    "    { \"id\": \"plant\", \"type\": \"producer\", \"value\": 100.0,\n"
    "      \"params\": { \"k_production\": 0.01, \"k_respiration\": 0.02 } },\n"
    "    { \"id\": \"deer\", \"type\": \"consumer\", \"value\": 5.0,\n"
    "      \"params\": { \"k_metabolism\": 0.1 } },\n"
    "    { \"id\": \"heat\", \"type\": \"sink\", \"value\": 0.0 }\n"
    "  ],\n"
    "  \"edges\": [\n"
    "    { \"origin\": \"plant\", \"target\": \"deer\", \"logic\": \"linear\","
    "      \"params\": { \"k\": 0.5 } }\n"
    "  ],\n"
    "  \"config\": { \"t_start\": 0, \"t_end\": 20, \"dt\": 0.1, \"method\": \"rk4\" }\n"
    "}";

void test_consumer_composite() {
    printf("Testing Phase 8 consumer composite...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE8_CONSUMER_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    /* Two composites: plant + deer */
    assert(GSSK_GetCompositeCount(inst) == 2);

    int deer_body = GSSK_FindNodeIdx(inst, "deer__body");
    assert(deer_body >= 0);

    double Q0 = GSSK_GetState(inst)[deer_body];
    for (int i = 0; i < 20; i++) {
        st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }
    double Q1 = GSSK_GetState(inst)[deer_body];
    printf("  deer__body: %.4f -> %.4f\n", Q0, Q1);
    /* Inflow from plant ≈ 0.5·100 ≈ 50/step; metabolism 0.1·Q.
     * Either grows or stays positive — assert it changed. */
    assert(fabs(Q1 - Q0) > 1e-6);

    GSSK_Free(inst);
    printf("  Consumer composite test PASSED\n");
}

static const char *PHASE8_USER_ARCHETYPE_MODEL =
    "{\n"
    "  \"metadata\": { \"schema_version\": 4 },\n"
    "  \"archetypes\": {\n"
    "    \"relay\": {\n"
    "      \"nodes\": [ { \"id\": \"buf\", \"type\": \"storage\", \"value\": 0.0 } ],\n"
    "      \"edges\": [],\n"
    "      \"ports\": { \"in\": \"buf\", \"out\": \"buf\" }\n"
    "    }\n"
    "  },\n"
    "  \"nodes\": [\n"
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },\n"
    "    { \"id\": \"r\",    \"type\": \"relay\",   \"value\": 5.0  },\n"
    "    { \"id\": \"sink\", \"type\": \"sink\",    \"value\": 0.0  }\n"
    "  ],\n"
    "  \"edges\": [\n"
    "    { \"origin\": \"src\", \"target\": \"r\",    \"logic\": \"constant\", \"params\": { \"k\": 1.0 } },\n"
    "    { \"origin\": \"r\",   \"target\": \"sink\", \"logic\": \"linear\",   \"params\": { \"k\": 0.5 } }\n"
    "  ],\n"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }\n"
    "}";

void test_user_archetype() {
    printf("Testing Phase 8 user-defined archetype...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE8_USER_ARCHETYPE_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    /* 4 built-ins + 1 user = 5 archetypes */
    size_t arch_count = GSSK_GetArchetypeCount(inst);
    printf("  archetype count = %zu\n", arch_count);
    assert(arch_count == 5);

    /* Verify relay is present */
    bool found_relay = false;
    for (size_t i = 0; i < arch_count; i++) {
        const char *n = GSSK_GetArchetypeName(inst, i);
        if (n && strcmp(n, "relay") == 0) { found_relay = true; break; }
    }
    assert(found_relay);

    /* One composite expansion (r) */
    assert(GSSK_GetCompositeCount(inst) == 1);

    /* Expanded storage node is r__buf */
    int buf_idx = GSSK_FindNodeIdx(inst, "r__buf");
    assert(buf_idx >= 0);

    double Q0 = GSSK_GetState(inst)[buf_idx];
    for (int i = 0; i < 5; i++) {
        st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }
    double Q1 = GSSK_GetState(inst)[buf_idx];
    printf("  r__buf: %.4f -> %.4f\n", Q0, Q1);
    /* Inflow 1.0/step (constant), outflow 0.5·Q.  Should change. */
    assert(fabs(Q1 - Q0) > 1e-6);

    GSSK_Free(inst);
    printf("  User archetype test PASSED\n");
}

void test_phase9_motif_detection() {
    printf("Testing Phase 9 motif detection...\n");

    /* Simple 3-node model: source → storage → sink.
     * After 10+ steps the source→storage and storage→sink 2-node motifs
     * each appear once per step, so their stable_steps should reach ≥ 10
     * and both should become candidates (occurrence 1 < MIN_COUNT=3,
     * so they remain NOT candidates — but we can still verify detection). */

    /* Use a model with multiple instances of the same motif type.
     * Two parallel source→storage drain chains gives occurrence=2 per step.
     * occurrence must reach MIN_COUNT=3 to increment stable_steps,
     * so use THREE parallel chains. */
    const char *model_json =
        "{"
        "  \"nodes\": ["
        "    {\"id\": \"s1\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b1\", \"type\": \"storage\", \"value\": 0.0},"
        "    {\"id\": \"s2\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b2\", \"type\": \"storage\", \"value\": 0.0},"
        "    {\"id\": \"s3\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b3\", \"type\": \"storage\", \"value\": 0.0}"
        "  ],"
        "  \"edges\": ["
        "    {\"origin\": \"s1\", \"target\": \"b1\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}},"
        "    {\"origin\": \"s2\", \"target\": \"b2\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}},"
        "    {\"origin\": \"s3\", \"target\": \"b3\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}}"
        "  ],"
        "  \"config\": {\"t_start\": 0, \"t_end\": 20, \"dt\": 1.0}"
        "}";

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(model_json, &inst) == GSSK_SUCCESS);

    /* Run 15 steps — past MIN_STEPS=10 */
    for (int i = 0; i < 15; i++) {
        GSSK_Status st = GSSK_Step(inst, GSSK_GetDt(inst));
        assert(st == GSSK_SUCCESS || st == GSSK_WARN_SOLVER_DIVERGENCE);
    }

    size_t mc = GSSK_GetMotifCount(inst);
    printf("  Motifs detected: %zu\n", mc);
    assert(mc > 0);

    /* Find the source→storage motif and verify it became a candidate */
    bool found_candidate = false;
    for (size_t mi = 0; mi < mc; mi++) {
        const char *canon = GSSK_GetMotifCanon(inst, mi);
        size_t stable = GSSK_GetMotifStableSteps(inst, mi);
        printf("  motif[%zu]: %s  stable=%zu  occ=%zu  candidate=%s\n",
               mi, canon ? canon : "?", stable,
               GSSK_GetMotifOccurrence(inst, mi),
               GSSK_IsMotifCandidate(inst, mi) ? "YES" : "no");
        if (GSSK_IsMotifCandidate(inst, mi)) found_candidate = true;
    }
    assert(found_candidate);

    /* Generativity index may be 0 at end (no *new* candidates last step) */
    double g = GSSK_GetGenerativityIndex(inst);
    printf("  Generativity index: %.6f\n", g);

    GSSK_Free(inst);
    printf("  Motif detection test PASSED\n");
}

void test_phase9_propose_archetype() {
    printf("Testing Phase 9 ProposeArchetype...\n");

    /* Same 3-chain model, run 15 steps so the motif is a candidate */
    const char *model_json =
        "{"
        "  \"nodes\": ["
        "    {\"id\": \"s1\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b1\", \"type\": \"storage\", \"value\": 0.0},"
        "    {\"id\": \"s2\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b2\", \"type\": \"storage\", \"value\": 0.0},"
        "    {\"id\": \"s3\", \"type\": \"source\",  \"value\": 10.0},"
        "    {\"id\": \"b3\", \"type\": \"storage\", \"value\": 0.0}"
        "  ],"
        "  \"edges\": ["
        "    {\"origin\": \"s1\", \"target\": \"b1\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}},"
        "    {\"origin\": \"s2\", \"target\": \"b2\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}},"
        "    {\"origin\": \"s3\", \"target\": \"b3\", \"logic\": \"linear\", \"params\": {\"k\": 0.1}}"
        "  ],"
        "  \"config\": {\"t_start\": 0, \"t_end\": 20, \"dt\": 1.0}"
        "}";

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(model_json, &inst) == GSSK_SUCCESS);
    for (int i = 0; i < 15; i++) GSSK_Step(inst, GSSK_GetDt(inst));

    /* Find first candidate motif */
    size_t candidate_idx = (size_t)-1;
    for (size_t mi = 0; mi < GSSK_GetMotifCount(inst); mi++) {
        if (GSSK_IsMotifCandidate(inst, mi)) { candidate_idx = mi; break; }
    }
    assert(candidate_idx != (size_t)-1);

    size_t arch_before = GSSK_GetArchetypeCount(inst);
    GSSK_Status st = GSSK_ProposeArchetype(inst, candidate_idx, "drain_pair");
    assert(st == GSSK_SUCCESS);
    assert(GSSK_GetArchetypeCount(inst) == arch_before + 1);

    /* Duplicate name should fail */
    st = GSSK_ProposeArchetype(inst, candidate_idx, "drain_pair");
    assert(st == GSSK_ERR_SCHEMA_VIOLATION);

    /* Mutation log should contain an archetype_proposal record */
    size_t mut_count = GSSK_GetMutationCount(inst);
    bool found_proposal = false;
    for (size_t mi = 0; mi < mut_count; mi++) {
        const GSSK_MutationRecord *r = GSSK_GetMutationRecord(inst, mi);
        if (r && r->op == GSSK_MUT_ARCHETYPE_PROPOSAL) { found_proposal = true; break; }
    }
    assert(found_proposal);
    printf("  ProposeArchetype test PASSED (archetype 'drain_pair' registered)\n");

    GSSK_Free(inst);
}

/* GH #29 item 1 — membership must be recorded at expansion time, not inferred
 * from the "{instance}__{member}" id.  `lim__c` below is declared directly and
 * prefix-matches composite `lim`; string inference misattributes it. */
static const char *PHASE8_MEMBERSHIP_MODEL =
    "{\n"
    "  \"metadata\": { \"schema_version\": 4 },\n"
    "  \"archetypes\": {\n"
    "    \"self_limiter\": {\n"
    "      \"nodes\": [ { \"id\": \"a\", \"type\": \"storage\", \"value\": 5.0 },\n"
    "                  { \"id\": \"b\", \"type\": \"storage\", \"value\": 0.0 } ],\n"
    "      \"edges\": [ { \"id\": \"ab\", \"origin\": \"a\", \"target\": \"b\", \"logic\": \"linear\", \"params\": { \"k\": 0.2 } },\n"
    "                  { \"id\": \"ba\", \"origin\": \"b\", \"target\": \"a\", \"logic\": \"linear\", \"params\": { \"k\": 0.1 } } ],\n"
    "      \"ports\": { \"in\": \"a\", \"out\": \"b\" }\n"
    "    }\n"
    "  },\n"
    "  \"nodes\": [\n"
    "    { \"id\": \"sun\",    \"type\": \"source\",       \"value\": 10.0 },\n"
    "    { \"id\": \"plant\",  \"type\": \"producer\",     \"value\": 50.0,\n"
    "      \"params\": { \"k_production\": 0.01, \"k_respiration\": 0.02 } },\n"
    "    { \"id\": \"lim\",    \"type\": \"self_limiter\", \"value\": 5.0 },\n"
    "    { \"id\": \"lim__c\", \"type\": \"storage\",      \"value\": 1.0 },\n"
    "    { \"id\": \"plain\",  \"type\": \"storage\",      \"value\": 0.0 }\n"
    "  ],\n"
    "  \"edges\": [\n"
    "    { \"origin\": \"sun\", \"target\": \"plant\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } }\n"
    "  ],\n"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }\n"
    "}";

/* No public node-count accessor; the convention is to walk ids until NULL. */
static size_t count_nodes(GSSK_Instance *inst) {
    size_t n = 0;
    while (GSSK_GetNodeID(inst, n) != NULL) n++;
    return n;
}

void test_composite_membership() {
    printf("Testing Phase 8 composite membership API (GH #29)...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(PHASE8_MEMBERSHIP_MODEL, &inst);
    assert(st == GSSK_SUCCESS);

    /* Two composite instances: plant (built-in) and lim (user-defined) */
    assert(GSSK_GetCompositeCount(inst) == 2);

    size_t plant_c = (size_t)-1, lim_c = (size_t)-1;
    for (size_t c = 0; c < GSSK_GetCompositeCount(inst); c++) {
        const char *cid = GSSK_GetCompositeID(inst, c);
        assert(cid);
        if (strcmp(cid, "plant") == 0) plant_c = c;
        if (strcmp(cid, "lim")   == 0) lim_c   = c;
    }
    assert(plant_c != (size_t)-1 && lim_c != (size_t)-1);

    /* Archetype each instance was expanded from */
    assert(strcmp(GSSK_GetCompositeArchetype(inst, plant_c), "producer") == 0);
    assert(strcmp(GSSK_GetCompositeArchetype(inst, lim_c), "self_limiter") == 0);

    /* Forward direction: node -> owning composite, and role within template */
    struct { const char *id; const char *role; } members[] = {
        { "plant__body", "body" },
        { "plant__gate", "gate" },
        { "plant__heat", "heat" },
    };
    for (size_t i = 0; i < 3; i++) {
        int ni = GSSK_FindNodeIdx(inst, members[i].id);
        assert(ni >= 0);
        assert(strcmp(GSSK_GetNodeComposite(inst, (size_t)ni), "plant") == 0);
        assert(strcmp(GSSK_GetNodeRole(inst, (size_t)ni), members[i].role) == 0);
    }

    /* Directly-declared nodes report "" — including `lim__c`, which a
     * prefix-match would wrongly attribute to composite `lim`. */
    const char *plain_ids[] = { "sun", "lim__c", "plain" };
    for (size_t i = 0; i < 3; i++) {
        int ni = GSSK_FindNodeIdx(inst, plain_ids[i]);
        assert(ni >= 0);
        assert(strcmp(GSSK_GetNodeComposite(inst, (size_t)ni), "") == 0);
        assert(strcmp(GSSK_GetNodeRole(inst, (size_t)ni), "") == 0);
    }

    /* Inverse direction round-trips: every member of a composite reports that
     * same composite, and member ids carry the expected namespace prefix. */
    assert(GSSK_GetCompositeMemberCount(inst, plant_c) == 3);
    assert(GSSK_GetCompositeMemberCount(inst, lim_c) == 2);
    for (size_t c = 0; c < GSSK_GetCompositeCount(inst); c++) {
        const char *cid = GSSK_GetCompositeID(inst, c);
        for (size_t m = 0; m < GSSK_GetCompositeMemberCount(inst, c); m++) {
            size_t ni = GSSK_GetCompositeMemberIndex(inst, c, m);
            assert(ni != (size_t)-1 && ni < count_nodes(inst));
            assert(strcmp(GSSK_GetNodeComposite(inst, ni), cid) == 0);
            assert(GSSK_GetNodeRole(inst, ni)[0] != '\0');
        }
    }

    /* Out-of-range indices fail loudly rather than reading past the arrays */
    size_t nc = count_nodes(inst), cc = GSSK_GetCompositeCount(inst);
    assert(GSSK_GetNodeComposite(inst, nc) == NULL);
    assert(GSSK_GetNodeRole(inst, nc) == NULL);
    assert(GSSK_GetCompositeArchetype(inst, cc) == NULL);
    assert(GSSK_GetCompositeMemberCount(inst, cc) == 0);
    assert(GSSK_GetCompositeMemberIndex(inst, cc, 0) == (size_t)-1);
    assert(GSSK_GetCompositeMemberIndex(inst, plant_c, 99) == (size_t)-1);
    assert(GSSK_GetNodeComposite(NULL, 0) == NULL);
    assert(GSSK_GetCompositeMemberCount(NULL, 0) == 0);

    /* Membership survives a node added at runtime (AddNode must not inherit
     * composite 0 from the zeroed struct). */
    st = GSSK_AddNode(inst, "{\"id\":\"late\",\"type\":\"storage\",\"value\":0.0}");
    assert(st == GSSK_SUCCESS);
    int late_idx = GSSK_FindNodeIdx(inst, "late");
    assert(late_idx >= 0);
    assert(strcmp(GSSK_GetNodeComposite(inst, (size_t)late_idx), "") == 0);

    GSSK_Free(inst);
    printf("  Composite membership test PASSED\n");
}

/* GH #29 item 5 — the stochastic entry points must be reproducible from a
 * recorded seed, and must not depend on process-global libc state. */
static const char *RNG_MODEL =
    "{"
    "\"nodes\": ["
    "  {\"id\": \"Source\", \"type\": \"source\", \"value\": 10.0},"
    "  {\"id\": \"Stock\", \"type\": \"storage\", \"value\": 0.0}"
    "],"
    "\"edges\": ["
    "  {\"id\": \"e0\", \"origin\": \"Source\", \"target\": \"Stock\", \"logic\": \"linear\", \"params\": {\"k\": 1.0}}"
    "],"
    "\"config\": {\"t_start\": 0, \"t_end\": 10, \"dt\": 1.0}"
    "}";

/* Run an ensemble under `seed` and copy out the mean envelope. */
static double *ensemble_under_seed(uint64_t seed, size_t *out_len) {
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(RNG_MODEL, &inst) == GSSK_SUCCESS);
    GSSK_SetSeed(inst, seed);
    GSSK_EnsembleResult *res = GSSK_EnsembleForecast(inst, 10, 0.2);
    assert(res != NULL);
    size_t n = res->node_count * res->step_count;
    double *copy = malloc(n * sizeof(double));
    memcpy(copy, res->mean_envelope, n * sizeof(double));
    *out_len = n;
    GSSK_FreeEnsembleResult(res);
    GSSK_Free(inst);
    return copy;
}

void test_rng_seeding() {
    printf("Testing RNG seeding / reproducibility (GH #29)...\n");

    /* Default seed: two fresh instances agree with no seeding call at all. */
    size_t n_d1 = 0, n_d2 = 0;
    double *d1 = ensemble_under_seed(GSSK_DEFAULT_SEED, &n_d1);
    GSSK_Instance *bare = NULL;
    assert(GSSK_Init(RNG_MODEL, &bare) == GSSK_SUCCESS);
    assert(GSSK_GetSeed(bare) == GSSK_DEFAULT_SEED);
    GSSK_EnsembleResult *rb = GSSK_EnsembleForecast(bare, 10, 0.2);
    assert(rb != NULL);
    n_d2 = rb->node_count * rb->step_count;
    assert(n_d1 == n_d2);
    for (size_t i = 0; i < n_d1; i++) assert(d1[i] == rb->mean_envelope[i]);
    GSSK_FreeEnsembleResult(rb);
    GSSK_Free(bare);
    free(d1);
    printf("  default seed is reproducible without an explicit GSSK_SetSeed\n");

    /* Same seed => bit-identical across two independent instances. */
    size_t n_a = 0, n_b = 0;
    double *a = ensemble_under_seed(12345, &n_a);
    double *b = ensemble_under_seed(12345, &n_b);
    assert(n_a == n_b && n_a > 0);
    for (size_t i = 0; i < n_a; i++) assert(a[i] == b[i]);
    printf("  same seed -> bit-identical over %zu values\n", n_a);

    /* Different seed => a different stream. */
    size_t n_c = 0;
    double *c = ensemble_under_seed(99999, &n_c);
    assert(n_c == n_a);
    bool differs = false;
    for (size_t i = 0; i < n_a; i++) if (a[i] != c[i]) { differs = true; break; }
    assert(differs);
    printf("  different seed -> different trajectory\n");
    free(a); free(b); free(c);

    /* libc srand must no longer influence anything. */
    size_t n_e = 0, n_f = 0;
    srand(1);
    double *e = ensemble_under_seed(777, &n_e);
    srand(999999);
    double *f = ensemble_under_seed(777, &n_f);
    assert(n_e == n_f);
    for (size_t i = 0; i < n_e; i++) assert(e[i] == f[i]);
    free(e); free(f);
    printf("  libc srand() no longer perturbs results\n");

    /* Two live instances must not share a stream: interleaved draws on one
     * must not shift the other. */
    GSSK_Instance *i1 = NULL, *i2 = NULL;
    assert(GSSK_Init(RNG_MODEL, &i1) == GSSK_SUCCESS);
    assert(GSSK_Init(RNG_MODEL, &i2) == GSSK_SUCCESS);
    GSSK_SetSeed(i1, 4242);
    GSSK_SetSeed(i2, 4242);
    for (int k = 0; k < 5; k++) GSSK_NextRandom(i2);   /* disturb i2 only */
    GSSK_SetSeed(i2, 4242);                            /* rewind i2 */
    for (int k = 0; k < 8; k++)
        assert(GSSK_NextRandom(i1) == GSSK_NextRandom(i2));
    printf("  instances hold independent streams\n");

    /* Seeding rewinds the stream. */
    GSSK_SetSeed(i1, 31337);
    uint64_t first = GSSK_NextRandom(i1);
    GSSK_NextRandom(i1);
    GSSK_SetSeed(i1, 31337);
    assert(GSSK_NextRandom(i1) == first);
    assert(GSSK_GetSeed(i1) == 31337);

    /* Snapshot captures seed AND stream position, and restores both. */
    GSSK_SetSeed(i1, 0xDEADBEEFCAFEULL);
    for (int k = 0; k < 3; k++) GSSK_NextRandom(i1);
    uint64_t expect_next = GSSK_NextRandom(i1);
    GSSK_SetSeed(i1, 0xDEADBEEFCAFEULL);
    for (int k = 0; k < 3; k++) GSSK_NextRandom(i1);   /* back to pre-draw point */

    char *snap = NULL;
    assert(GSSK_SerializeSnapshot(i1, &snap) == GSSK_SUCCESS);
    assert(strstr(snap, "\"rng_state\"") != NULL);
    assert(strstr(snap, "\"seed\"") != NULL);

    GSSK_Instance *restored = NULL;
    assert(GSSK_Init(snap, &restored) == GSSK_SUCCESS);
    assert(GSSK_GetSeed(restored) == 0xDEADBEEFCAFEULL);
    assert(GSSK_NextRandom(restored) == expect_next);
    printf("  snapshot round-trip preserves seed and stream position\n");

    GSSK_FreeString(snap);
    GSSK_Free(restored);
    GSSK_Free(i1);
    GSSK_Free(i2);
    printf("  RNG seeding test PASSED\n");
}

int main() {
    test_calibration();
    test_ensemble();
    test_interaction_node();
    test_loop_limited_node();
    test_producer_composite();
    test_consumer_composite();
    test_user_archetype();
    test_composite_membership();
    test_rng_seeding();
    test_phase9_motif_detection();
    test_phase9_propose_archetype();
    return 0;
}
