/* Stage times — assert the solver hands each derivative evaluation the RIGHT
 * time, not merely A time.
 *
 * compute_derivatives took no time argument until now. Every flow in GSSK is
 * autonomous, so nothing had ever needed one — but the Butcher c-nodes were
 * already written down in the DOPRI5 stage comments and thrown away, because
 * there was no `t` for them to offset.
 *
 * That is the trap: a forcing function sampled once per STEP instead of once
 * per STAGE makes the forcing first-order while the state is fourth- or
 * fifth-order. The run still completes, the trajectory still looks smooth, and
 * the order loss is invisible without a convergence study. So the stage times
 * are pinned here, before anything consumes them, where the rest of the suite
 * can hold "nothing changed at all" as its acceptance criterion.
 *
 * The times are RECORDED from the solver rather than recomputed in the test.
 * Re-deriving the arithmetic here would just be the same mistake written twice.
 * The recorder is compiled in only under -DGSSK_STAGE_TIME_PROBE, which only
 * this target defines; it is not in gssk.h and not in the WASM export list.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* Defined in src/gssk.c under GSSK_STAGE_TIME_PROBE. */
extern void (*gssk_probe_on_derivative)(double t);
extern void (*gssk_probe_on_adjoint)(double t);

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

#define MAX_REC 4096
static double rec[MAX_REC];
static size_t rec_n = 0;
static void record(double t) { if (rec_n < MAX_REC) rec[rec_n++] = t; }
static void reset_rec(void) { rec_n = 0; }

/* The solver's own dt arithmetic is floating point, so t + 0.5*dt computed
 * here and there can differ in the last bit. 1e-12 is far tighter than any
 * stage-time error that would matter and far looser than that. */
static const double TOL = 1e-12;

static void expect_seq(const double *want, size_t n, const char *what) {
    CHECK(rec_n == n, "%s: expected %zu derivative calls, got %zu", what, n, rec_n);
    size_t m = rec_n < n ? rec_n : n;
    for (size_t i = 0; i < m; i++)
        CHECK(fabs(rec[i] - want[i]) < TOL,
              "%s: stage %zu at t=%.17g, expected %.17g (diff %.3g)",
              what, i, rec[i], want[i], rec[i] - want[i]);
}

/* ---------------------------------------------------------------- */

/* Deliberately has NO threshold edge: threshold sub-stepping calls
 * rk4_step_alloc repeatedly, which would swamp the four stages under test.
 * It gets its own case below. */
static const char *PLAIN_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 1.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\","
    "      \"logic\": \"linear\", \"params\": { \"k\": 0.3 } }"
    "  ],"
    "  \"config\": { \"t_start\": 2.5, \"t_end\": 12.5, \"dt\": 0.4%s }"
    "}";

static GSSK_Instance *load(const char *method_suffix) {
    char json[1024];
    snprintf(json, sizeof(json), PLAIN_MODEL, method_suffix);
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("  FATAL: model failed to load: %d (%s)\n",
               (int)st, inst ? GSSK_GetErrorDescription(inst) : "");
        exit(1);
    }
    return inst;
}

/* t_start is 2.5, not 0. A t threaded as "time since the step began" rather
 * than absolute simulation time passes every test that starts at zero. */
static void test_rk4_nodes(void) {
    printf("Testing classical RK4 c-nodes (0, 1/2, 1/2, 1)...\n");
    GSSK_Instance *inst = load(", \"method\": \"rk4\"");
    double t0 = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);

    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_Step(inst, dt);
    gssk_probe_on_derivative = NULL;

    const double want[] = { t0, t0 + 0.5*dt, t0 + 0.5*dt, t0 + dt };
    expect_seq(want, 4, "rk4 step 1");

    /* Second step: the sequence must have MOVED. A `t` wired to t_start rather
     * than current_t reproduces step 1 forever and passes the test above. */
    double t1 = GSSK_GetCurrentTime(inst);
    CHECK(fabs(t1 - (t0 + dt)) < TOL, "current_t should have advanced to %.17g, got %.17g",
          t0 + dt, t1);
    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_Step(inst, dt);
    gssk_probe_on_derivative = NULL;
    const double want2[] = { t1, t1 + 0.5*dt, t1 + 0.5*dt, t1 + dt };
    expect_seq(want2, 4, "rk4 step 2");

    GSSK_Free(inst);
    printf("  RK4 nodes OK\n");
}

/* Incipient still runs rk4_step_ex as ground truth, so it sees the same four
 * stages; idc_step_ex reaches the derivative surface through build_flow_matrix
 * and the node helpers, not through compute_derivatives. */
static void test_incipient_nodes(void) {
    printf("Testing incipient path still sees the RK4 c-nodes...\n");
    GSSK_Instance *inst = load(", \"method\": \"incipient\"");
    double t0 = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);

    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_Step(inst, dt);
    gssk_probe_on_derivative = NULL;

    const double want[] = { t0, t0 + 0.5*dt, t0 + 0.5*dt, t0 + dt };
    expect_seq(want, 4, "incipient");
    GSSK_Free(inst);
    printf("  Incipient OK\n");
}

static void test_euler_node(void) {
    printf("Testing Euler evaluates once, at the step's start...\n");
    GSSK_Instance *inst = load(", \"method\": \"euler\"");
    double t0 = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);

    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_Step(inst, dt);
    gssk_probe_on_derivative = NULL;

    const double want[] = { t0 };
    expect_seq(want, 1, "euler");
    GSSK_Free(inst);
    printf("  Euler OK\n");
}

/* THE SHARP CASE. Adaptive spans dt with repeated PI-controlled DOPRI5
 * sub-steps. The time passed to stage 1 of sub-step 2 is t + h1, NOT t. A
 * naive threading that passes the step's start time to every sub-step is
 * correct for sub-step 1 and wrong for every one after it. */
static const double C[7] = { 0.0, 1.0/5.0, 3.0/10.0, 4.0/5.0, 8.0/9.0, 1.0, 1.0 };

static void test_adaptive_substeps(void) {
    printf("Testing DOPRI5 c-nodes advance across adaptive sub-steps...\n");

    /* h_max forces the step to be spanned by several sub-steps rather than
     * one, which is what puts sub-step 2 onwards under test at all. */
    GSSK_Instance *inst = load(", \"method\": \"adaptive\", \"h_max\": 0.1");
    double t0 = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);

    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_Step(inst, dt);
    gssk_probe_on_derivative = NULL;

    CHECK(rec_n % 7 == 0,
          "expected a whole number of 7-stage sub-steps, got %zu calls", rec_n);
    size_t subs = rec_n / 7;
    CHECK(subs >= 2, "need at least 2 accepted sub-steps to test advancement, got %zu",
          subs);
    printf("    %zu sub-step(s) over dt=%g\n", subs, dt);

    /* Within each sub-step, the seven stages must sit at base + c_i*h for a
     * single consistent h — recovered from stage 6, whose c is exactly 1. */
    double prev_base = -1.0;
    for (size_t s = 0; s < subs && s * 7 + 6 < rec_n; s++) {
        const double *st = &rec[s * 7];
        double base = st[0];
        double h    = st[5] - base;   /* c6 = 1 */

        CHECK(h > 0.0, "sub-step %zu: recovered h = %g must be positive", s, h);
        for (size_t i = 0; i < 7; i++)
            CHECK(fabs(st[i] - (base + C[i] * h)) < TOL,
                  "sub-step %zu stage %zu at t=%.17g, expected base+c*h=%.17g",
                  s, i, st[i], base + C[i] * h);

        /* The advancement check — this is the whole point of the case. */
        if (s == 0) {
            CHECK(fabs(base - t0) < TOL,
                  "sub-step 0 must start at t0=%.17g, got %.17g", t0, base);
        } else {
            CHECK(base > prev_base + TOL,
                  "sub-step %zu starts at %.17g, which did not advance past %.17g "
                  "— sub-steps are restarting from the step's start time",
                  s, base, prev_base);
        }
        prev_base = base;
    }

    /* And the last sub-step must land on the end of the step. */
    if (rec_n >= 7) {
        double last_end = rec[rec_n - 1];
        CHECK(fabs(last_end - (t0 + dt)) < 1e-9,
              "final stage at %.17g, expected the step end %.17g", last_end, t0 + dt);
    }

    GSSK_Free(inst);
    printf("  Adaptive sub-steps OK\n");
}

/* GSSK_StepAdaptive takes one internally-sized step rather than spanning dt. */
static void test_step_adaptive_entry(void) {
    printf("Testing GSSK_StepAdaptive stages sit on the DOPRI5 nodes...\n");
    GSSK_Instance *inst = load(", \"method\": \"adaptive\", \"h_max\": 0.1");
    double t0 = GSSK_GetCurrentTime(inst);

    reset_rec();
    gssk_probe_on_derivative = record;
    GSSK_StepAdaptive(inst);
    gssk_probe_on_derivative = NULL;

    CHECK(rec_n >= 7, "expected at least one 7-stage sub-step, got %zu", rec_n);
    if (rec_n >= 7) {
        double base = rec[0], h = rec[5] - base;
        CHECK(fabs(base - t0) < TOL, "first stage must be at t0=%.17g, got %.17g",
              t0, base);
        for (size_t i = 0; i < 7; i++)
            CHECK(fabs(rec[i] - (base + C[i] * h)) < TOL,
                  "StepAdaptive stage %zu at %.17g, expected %.17g",
                  i, rec[i], base + C[i] * h);
    }
    GSSK_Free(inst);
    printf("  StepAdaptive OK\n");
}

/* Threshold sub-stepping restarts the integrator from each crossing point, so
 * its rk4_step_alloc calls must be handed the crossing time, not the step's
 * start time. The exact call count depends on the bisection, so assert the
 * invariant instead: every recorded time lies inside the step's own interval. */
static const char *THRESHOLD_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 10.0 },"
    "    { \"id\": \"out\",  \"type\": \"sink\",    \"value\": 0.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"e1\", \"origin\": \"tank\", \"target\": \"out\","
    "      \"logic\": \"threshold\", \"params\": { \"k\": 1.0, \"threshold\": 5.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 2.5, \"t_end\": 12.5, \"dt\": 0.4, \"method\": \"rk4\" }"
    "}";

static void test_threshold_substep_times(void) {
    printf("Testing threshold sub-stepping stays inside its own step...\n");
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(THRESHOLD_MODEL, &inst) == GSSK_SUCCESS);

    /* Step until the crossing is actually exercised. */
    int saw_substep = 0;
    for (int i = 0; i < 40; i++) {
        double t0 = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);
        reset_rec();
        gssk_probe_on_derivative = record;
        GSSK_Step(inst, dt);
        gssk_probe_on_derivative = NULL;

        for (size_t j = 0; j < rec_n; j++)
            CHECK(rec[j] >= t0 - TOL && rec[j] <= t0 + dt + TOL,
                  "step at t0=%g: recorded t=%.17g falls outside [%g, %g]",
                  t0, rec[j], t0, t0 + dt);
        if (rec_n > 4) saw_substep = 1;
    }
    CHECK(saw_substep,
          "no step ever sub-stepped — the threshold was never crossed, so this "
          "test asserted nothing");
    GSSK_Free(inst);
    printf("  Threshold sub-stepping OK\n");
}

/* The adjoint runs time BACKWARDS. It is the one place a sign or direction
 * error stays invisible: the gradient would still be finite and plausible. */
static void test_adjoint_runs_backwards(void) {
    printf("Testing the adjoint's stage times decrease and land on t_start...\n");
    GSSK_Instance *inst = load(", \"method\": \"rk4\"");

    GSSK_AdjointTarget target = { .node_idx = 1, .target_value = 5.0, .weight = 1.0 };
    size_t param_edges[1] = { 0 };
    double grad[1] = { 0.0 };

    reset_rec();
    gssk_probe_on_adjoint = record;
    GSSK_Status st = GSSK_RunAdjoint(inst, &target, 1, param_edges, 1, grad);
    gssk_probe_on_adjoint = NULL;

    CHECK(st == GSSK_SUCCESS, "GSSK_RunAdjoint failed: %d", (int)st);
    CHECK(rec_n >= 2, "expected at least 2 backward steps, got %zu", rec_n);

    for (size_t i = 1; i < rec_n; i++)
        CHECK(rec[i] < rec[i - 1] - TOL,
              "backward step %zu at t=%.17g did not decrease from %.17g",
              i, rec[i], rec[i - 1]);

    if (rec_n) {
        double t_start = GSSK_GetTStart(inst);
        CHECK(fabs(rec[rec_n - 1] - t_start) < 1e-12,
              "backward integration ended at %.17g, expected t_start=%.17g",
              rec[rec_n - 1], t_start);
        CHECK(fabs(rec[0] - GSSK_GetTEnd(inst)) < 1e-9,
              "backward integration started at %.17g, expected t_end=%.17g",
              rec[0], GSSK_GetTEnd(inst));
    }
    GSSK_Free(inst);
    printf("  Adjoint direction OK\n");
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== Stage time tests ===\n\n");

    test_rk4_nodes();
    test_incipient_nodes();
    test_euler_node();
    test_adaptive_substeps();
    test_step_adaptive_entry();
    test_threshold_substep_times();
    test_adjoint_runs_backwards();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL STAGE TIME TESTS PASSED ===\n");
    return 0;
}
