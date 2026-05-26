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

int main() {
    test_calibration();
    test_ensemble();
    test_interaction_node();
    test_loop_limited_node();
    return 0;
}
