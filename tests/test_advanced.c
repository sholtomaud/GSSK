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

    srand(42); // Seed for deterministic test
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

int main() {
    test_calibration();
    test_ensemble();
    test_interaction_node();
    test_loop_limited_node();
    test_producer_composite();
    test_consumer_composite();
    test_user_archetype();
    return 0;
}
