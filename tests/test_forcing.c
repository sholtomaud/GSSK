/* Forcing functions — one waveform vocabulary, two attachment points.
 *
 * A source node held its declared value for the whole run, and whether that
 * constant meant anything depended on the edge reading it: `linear` gives
 * flow = k*value, `constant` gives flow = k and ignores the value entirely. So
 * GSSK expressed two of Odum's eleven forcing functions (Fig. 7-2) — constant
 * force and constant flow — and had no representation for the other nine.
 *
 * The four things the upstream proposal called out as easy to get wrong are
 * the four things this file is mostly about:
 *
 *   1. Evaluate at STAGE times, not once per step. A once-per-step
 *      implementation passes every other test here and shows second-order
 *      convergence instead of fourth. test_fourth_order_convergence is the
 *      only check that can tell the difference.
 *   2. HOLD jitter across the stages, so the trajectory does not depend on
 *      solver internals.
 *   3. sin/exp stay in the one pinned WASM artifact (checked by the JS-side
 *      parity check, not here).
 *   4. EXPOSE the evaluator, and prove it is not a second implementation.
 *
 * Formulas are asserted against HAND-COMPUTED values, never golden numbers.
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

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static const double PI = 3.14159265358979323846;

/* A source with `forcing`, feeding a tank through a linear edge. The source is
 * the forced element; the tank integrates it. */
static GSSK_Instance *load_forced_source(const char *forcing_json,
                                         const char *cfg_extra) {
    char json[2048];
    snprintf(json, sizeof(json),
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"sun\",  \"type\": \"source\",  \"value\": 1.0, \"forcing\": %s },"
        "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
        "      \"logic\": \"linear\", \"params\": { \"k\": 1.0 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1%s }"
        "}", forcing_json, cfg_extra ? cfg_extra : "");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("  FATAL: load failed (%d): %s\n", (int)st,
               inst ? GSSK_GetErrorDescription(inst) : "");
        printf("  json: %s\n", json);
        exit(1);
    }
    return inst;
}

/* ================================================================
 * 1. Each waveform against a hand-computed value
 * ================================================================ */

/* Evaluated through the PUBLIC evaluator, which is the same code the
 * derivative path runs — see test_evaluator_matches_kernel below. */
static void expect_wave(const char *forcing_json, double t, double want,
                        double tol, const char *what) {
    GSSK_Instance *inst = load_forced_source(forcing_json, NULL);
    double got = GSSK_EvaluateNodeForcing(inst, 0, t);
    CHECK(NEAR(got, want, tol), "%s at t=%g: got %.12g, hand-computed %.12g",
          what, t, got, want);
    GSSK_Free(inst);
}

static void test_step(void) {
    printf("Testing step...\n");
    const char *f = "{ \"waveform\": \"step\", \"t_on\": 3.0, \"v0\": 2.0, \"v1\": 7.0 }";
    expect_wave(f, 0.0,   2.0, 0.0, "step before t_on");
    expect_wave(f, 2.999, 2.0, 0.0, "step just before t_on");
    /* At t_on exactly the value is v1: the boundary is closed on the right. */
    expect_wave(f, 3.0,   7.0, 0.0, "step AT t_on");
    expect_wave(f, 9.0,   7.0, 0.0, "step after t_on");
    printf("  step OK\n");
}

/* The impulse criterion: the INTEGRAL is amplitude-independent of dt. Height
 * is area/dt on a window of width dt, so halving dt doubles the height and
 * halves the width. */
static void test_impulse_area_is_dt_independent(void) {
    printf("Testing impulse area is independent of dt...\n");
    const char *f = "{ \"waveform\": \"impulse\", \"t_on\": 1.0, \"area\": 5.0 }";

    const double dts[] = { 0.1, 0.05, 0.025 };
    for (size_t i = 0; i < 3; i++) {
        char cfg[128];
        snprintf(cfg, sizeof(cfg), ", \"dtx\": 0");   /* placeholder, replaced below */
        (void)cfg;
        char json[2048];
        snprintf(json, sizeof(json),
            "{ \"metadata\": { \"schema_version\": 4 },"
            "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 1.0, \"forcing\": %s },"
            "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
            "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
            "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
            "  \"config\": { \"t_start\": 0, \"t_end\": 3, \"dt\": %g } }", f, dts[i]);
        GSSK_Instance *inst = NULL;
        assert(GSSK_Init(json, &inst) == GSSK_SUCCESS);

        double h = GSSK_EvaluateNodeForcing(inst, 0, 1.0);
        CHECK(NEAR(h, 5.0 / dts[i], 1e-9),
              "dt=%g: impulse height %.12g, expected area/dt = %.12g",
              dts[i], h, 5.0 / dts[i]);
        /* Outside the window it is zero, at any dt. */
        CHECK(GSSK_EvaluateNodeForcing(inst, 0, 0.5) == 0.0, "before window");
        CHECK(GSSK_EvaluateNodeForcing(inst, 0, 1.0 + dts[i] + 1e-9) == 0.0,
              "dt=%g: after window", dts[i]);
        GSSK_Free(inst);
    }
    printf("  impulse OK (height scales as area/dt, so the integral is `area`)\n");
}

static void test_ramp(void) {
    printf("Testing ramp...\n");
    const char *f = "{ \"waveform\": \"ramp\", \"t_on\": 2.0, \"v0\": 1.0, \"slope\": 3.0 }";
    expect_wave(f, 0.0,  1.0, 0.0, "ramp before t_on");
    expect_wave(f, 2.0,  1.0, 0.0, "ramp at t_on");
    expect_wave(f, 4.0,  7.0, 1e-12, "ramp: 1 + 3*(4-2)");
    /* Clamped variant. */
    const char *fc = "{ \"waveform\": \"ramp\", \"t_on\": 0, \"v0\": 0, \"slope\": 1, \"max\": 2.5 }";
    expect_wave(fc, 2.0, 2.0, 1e-12, "ramp under the clamp");
    expect_wave(fc, 9.0, 2.5, 0.0,   "ramp clamped at max");
    printf("  ramp OK\n");
}

static void test_sine(void) {
    printf("Testing sine...\n");
    /* mean + amplitude*sin(2*PI*(tau - phase)/period), tau = t - t_on */
    const char *f = "{ \"waveform\": \"sine\", \"mean\": 10.0, \"amplitude\": 4.0,"
                    "  \"period\": 8.0, \"phase\": 0.0 }";
    expect_wave(f, 0.0, 10.0,             1e-12, "sine at 0");
    expect_wave(f, 2.0, 14.0,             1e-12, "sine at quarter period (peak)");
    expect_wave(f, 4.0, 10.0,             1e-12, "sine at half period");
    expect_wave(f, 6.0,  6.0,             1e-12, "sine at three-quarter (trough)");
    expect_wave(f, 1.0, 10.0 + 4.0*sin(2*PI*1.0/8.0), 1e-12, "sine at t=1");

    /* PHASE IS A TIME OFFSET AND IS SUBTRACTED, so a positive phase DELAYS.
     * phase = 2 (a quarter period) moves the peak from t=2 to t=4. */
    const char *fp = "{ \"waveform\": \"sine\", \"mean\": 10.0, \"amplitude\": 4.0,"
                     "  \"period\": 8.0, \"phase\": 2.0 }";
    expect_wave(fp, 4.0, 14.0, 1e-12, "phase=2 delays the peak to t=4");
    expect_wave(fp, 2.0, 10.0, 1e-12, "phase=2: t=2 is now the rising zero");
    printf("  sine OK (phase is a delay in time units)\n");
}

static void test_square_sawtooth(void) {
    printf("Testing square and sawtooth...\n");
    /* square: high while frac((tau-phase)/period) < duty */
    const char *sq = "{ \"waveform\": \"square\", \"mean\": 5.0, \"amplitude\": 2.0,"
                     "  \"period\": 4.0, \"duty\": 0.25 }";
    expect_wave(sq, 0.0, 7.0, 0.0, "square: start of period is high");
    expect_wave(sq, 0.9, 7.0, 0.0, "square: still high inside duty");
    expect_wave(sq, 1.0, 3.0, 0.0, "square: low at exactly duty (half-open)");
    expect_wave(sq, 3.9, 3.0, 0.0, "square: low to the end of the period");
    expect_wave(sq, 4.0, 7.0, 0.0, "square: high again next period");

    /* sawtooth: mean + amplitude*(2*frac(...) - 1) — rises across the period */
    const char *sw = "{ \"waveform\": \"sawtooth\", \"mean\": 0.0, \"amplitude\": 1.0,"
                     "  \"period\": 2.0 }";
    expect_wave(sw, 0.0, -1.0, 1e-12, "sawtooth starts at mean-amplitude");
    expect_wave(sw, 1.0,  0.0, 1e-12, "sawtooth is at mean mid-period");
    expect_wave(sw, 1.5,  0.5, 1e-12, "sawtooth three-quarters up");
    expect_wave(sw, 2.0, -1.0, 1e-12, "sawtooth resets");
    printf("  square and sawtooth OK\n");
}

static void test_exponential(void) {
    printf("Testing exponential...\n");
    const char *f = "{ \"waveform\": \"exponential\", \"t_on\": 1.0, \"v0\": 3.0, \"rate\": 0.5 }";
    expect_wave(f, 0.0, 3.0, 0.0, "exponential before t_on");
    expect_wave(f, 1.0, 3.0, 1e-12, "exponential at t_on");
    expect_wave(f, 3.0, 3.0 * exp(0.5 * 2.0), 1e-12, "exponential: 3*exp(0.5*2)");
    printf("  exponential OK\n");
}

/* ================================================================
 * 2. Requirement 1 — stage times, caught by convergence order
 * ================================================================ */

/* d(tank)/dt = k * sun(t) with k = 1 and sun(t) = mean + A*sin(2*pi*t/P).
 * Integrating from 0 to T with tank(0) = 0:
 *   tank(T) = mean*T + (A*P / (2*pi)) * (1 - cos(2*pi*T/P))
 * A closed form, so the error is a real error and not a comparison against a
 * finer numerical run. */
static double analytic_tank(double T, double mean, double A, double P) {
    return mean * T + (A * P / (2.0 * PI)) * (1.0 - cos(2.0 * PI * T / P));
}

static void test_fourth_order_convergence(void) {
    printf("Testing RK4 is FOURTH-order on a sine-forced source...\n");
    printf("    (a once-per-step implementation shows ~2x here, not ~16x)\n");

    const double mean = 3.0, A = 2.0, P = 5.0, T = 4.0;
    const double dts[] = { 0.2, 0.1, 0.05, 0.025 };
    double err[4];

    for (size_t i = 0; i < 4; i++) {
        char json[2048];
        snprintf(json, sizeof(json),
            "{ \"metadata\": { \"schema_version\": 4 },"
            "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0,"
            "                 \"forcing\": { \"waveform\": \"sine\", \"mean\": %g,"
            "                                \"amplitude\": %g, \"period\": %g } },"
            "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
            "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
            "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
            "  \"config\": { \"t_start\": 0, \"t_end\": %g, \"dt\": %g, \"method\": \"rk4\" } }",
            mean, A, P, T, dts[i]);

        GSSK_Instance *inst = NULL;
        assert(GSSK_Init(json, &inst) == GSSK_SUCCESS);
        size_t steps = (size_t)(T / dts[i] + 0.5);
        for (size_t s = 0; s < steps; s++) GSSK_Step(inst, dts[i]);

        double got = GSSK_GetState(inst)[1];
        err[i] = fabs(got - analytic_tank(T, mean, A, P));
        printf("    dt=%-6g  tank=%.12f  err=%.3e\n", dts[i], got, err[i]);
        GSSK_Free(inst);
    }

    /* Three refinements, each expected to cut the error by ~2^4 = 16. Accept
     * 8x as the floor: below that the implementation is not fourth-order, and
     * a once-per-step forcing lands near 4x (second order in the forcing
     * dominates). Above ~40x we are into round-off, so allow generous room. */
    for (size_t i = 0; i + 1 < 4; i++) {
        double ratio = err[i] / err[i + 1];
        printf("    dt %g -> %g : error ratio %.2fx\n", dts[i], dts[i+1], ratio);
        CHECK(ratio > 8.0,
              "error ratio %.2fx between dt=%g and dt=%g is below fourth order "
              "— forcing is probably sampled once per STEP, not per STAGE",
              ratio, dts[i], dts[i + 1]);
    }
    printf("  Fourth-order convergence OK\n");
}

/* ================================================================
 * 3. Requirement 2 — jitter is latched, and deterministic
 * ================================================================ */

/* Records BOTH the trajectory and the latched jitter value after each step.
 * The requirement is about the RNG stream — "drawn exactly once per accepted
 * step" — so the draws are what must be bit-identical across solvers. The
 * trajectory is compared too, but only to a tolerance: RK4 and DOPRI5 sum the
 * same constant with different weights, so they differ in the last bits for
 * reasons that have nothing to do with the stream. */
static void run_traj(const char *method, double *out, size_t n_out) {
    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0,"
        "                 \"forcing\": { \"waveform\": \"jitter\", \"mean\": 5.0,"
        "                                \"amplitude\": 2.0 } },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
        "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1, \"method\": \"%s\" } }",
        method);
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(json, &inst) == GSSK_SUCCESS);
    for (size_t i = 0; i < n_out; i++) {
        GSSK_Step(inst, GSSK_GetDt(inst));
        out[i] = GSSK_GetState(inst)[1];
    }
    GSSK_Free(inst);
}

static void run_draws(const char *method, double *out, size_t n_out) {
    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0,"
        "                 \"forcing\": { \"waveform\": \"jitter\", \"mean\": 5.0,"
        "                                \"amplitude\": 2.0 } },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
        "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1, \"method\": \"%s\" } }",
        method);
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(json, &inst) == GSSK_SUCCESS);
    for (size_t i = 0; i < n_out; i++) {
        GSSK_Step(inst, GSSK_GetDt(inst));
        /* After the step: the draw latched at its start is the one it used. */
        out[i] = GSSK_EvaluateNodeForcing(inst, 0, GSSK_GetCurrentTime(inst));
    }
    GSSK_Free(inst);
}

static void test_jitter_determinism(void) {
    printf("Testing jitter is latched per step and solver-independent...\n");
    enum { N = 40 };
    double a[N], b[N], c[N], d[N];

    run_traj("rk4", a, N);
    run_traj("rk4", b, N);
    for (size_t i = 0; i < N; i++)
        CHECK(a[i] == b[i], "two runs in one process diverged at step %zu: %.17g vs %.17g",
              i, a[i], b[i]);

    /* THE criterion, asserted on the DRAWS rather than the trajectory. If
     * jitter were drawn per stage, rk4 (4 stages), dopri5 (7) and the IDC path
     * would consume the stream at different rates and the sequences would
     * diverge at once. Rejected adaptive sub-steps would do the same. */
    double da[N], dc[N], dd[N];
    run_draws("rk4",       da, N);
    run_draws("incipient", dc, N);
    run_draws("adaptive",  dd, N);
    for (size_t i = 0; i < N; i++) {
        CHECK(da[i] == dc[i], "rk4 and incipient drew different jitter at step %zu: "
              "%.17g vs %.17g — the stream is consumed per STAGE, not per step",
              i, da[i], dc[i]);
        CHECK(da[i] == dd[i], "rk4 and adaptive drew different jitter at step %zu: "
              "%.17g vs %.17g — the stream is consumed per stage, or on REJECTED "
              "adaptive sub-steps", i, da[i], dd[i]);
    }

    /* The trajectories then agree to a numerical tolerance. Not bit-exactly:
     * with the forcing constant across a step, RK4 and DOPRI5 both integrate
     * it exactly but sum their stage weights differently, so the last bits
     * differ for reasons that are round-off and not stream position. */
    run_traj("incipient", c, N);
    run_traj("adaptive",  d, N);
    for (size_t i = 0; i < N; i++) {
        CHECK(NEAR(a[i], c[i], 1e-9), "rk4 vs incipient at step %zu: %.17g vs %.17g",
              i, a[i], c[i]);
        CHECK(NEAR(a[i], d[i], 1e-9), "rk4 vs adaptive at step %zu: %.17g vs %.17g",
              i, a[i], d[i]);
    }

    /* And it is actually varying, or the test above is vacuous. */
    int distinct = 0;
    for (size_t i = 2; i < N; i++)
        if (fabs((a[i] - a[i-1]) - (a[i-1] - a[i-2])) > 1e-12) distinct++;
    CHECK(distinct > N / 3, "jitter increments barely varied (%d of %d) — is it "
          "actually drawing?", distinct, (int)N - 2);
    printf("  Jitter determinism OK (rk4 == incipient == adaptive)\n");
}

static void test_jitter_snapshot_resume(void) {
    printf("Testing a jitter run resumes identically from a snapshot...\n");
    const char *MODEL =
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0,"
        "                 \"forcing\": { \"waveform\": \"jitter\", \"mean\": 5.0,"
        "                                \"amplitude\": 2.0 } },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
        "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1, \"method\": \"rk4\" } }";

    GSSK_Instance *a = NULL;
    assert(GSSK_Init(MODEL, &a) == GSSK_SUCCESS);
    for (int i = 0; i < 20; i++) GSSK_Step(a, GSSK_GetDt(a));

    char *snap = NULL;
    assert(GSSK_SerializeSnapshot(a, &snap) == GSSK_SUCCESS);
    GSSK_Instance *b = NULL;
    GSSK_Status st = GSSK_Init(snap, &b);
    CHECK(st == GSSK_SUCCESS, "snapshot must reload: %d (%s)", (int)st,
          b ? GSSK_GetErrorDescription(b) : "");
    GSSK_FreeString(snap);

    if (st == GSSK_SUCCESS) {
        for (int i = 0; i < 20; i++) { GSSK_Step(a, GSSK_GetDt(a)); GSSK_Step(b, GSSK_GetDt(b)); }
        CHECK(GSSK_GetState(a)[1] == GSSK_GetState(b)[1],
              "resumed run diverged: %.17g vs %.17g — the RNG stream position "
              "did not survive the snapshot",
              GSSK_GetState(a)[1], GSSK_GetState(b)[1]);
    }
    GSSK_Free(a); GSSK_Free(b);
    printf("  Snapshot resume OK\n");
}

/* ================================================================
 * 4. Requirement 4 — the evaluator is not a second implementation
 * ================================================================ */

/* Drive a source with each waveform and check that what the kernel INTEGRATED
 * matches what the evaluator REPORTS. With logic=constant on the edge the
 * tank's derivative is exactly the edge's k, and with logic=linear it is
 * exactly the source's value — so a single Euler step of size dt reveals the
 * value the derivative path actually used. */
static void assert_evaluator_matches_kernel(const char *forcing_json,
                                            const char *what) {
    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0,"
        "                 \"forcing\": %s },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
        "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.25, \"method\": \"euler\" } }",
        forcing_json);
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(json, &inst) == GSSK_SUCCESS);

    for (int s = 0; s < 8; s++) {
        double t      = GSSK_GetCurrentTime(inst);
        double dt     = GSSK_GetDt(inst);
        double before = GSSK_GetState(inst)[1];
        GSSK_Step(inst, dt);
        double did    = (GSSK_GetState(inst)[1] - before) / dt;
        /* Read the evaluator AFTER the step, at the step's own t. For the
         * seven deterministic waveforms this is the same answer either side of
         * the call. For jitter it must be: the draw is latched at the START of
         * GSSK_Step, so before the call the latch still holds the PREVIOUS
         * step's value. Asking after is asking about the step that just ran. */
        double say    = GSSK_EvaluateNodeForcing(inst, 0, t);
        CHECK(NEAR(say, did, 1e-9),
              "%s at t=%g: evaluator says %.12g, kernel integrated %.12g "
              "— these are two implementations", what, t, say, did);
    }
    GSSK_Free(inst);
}

static void test_evaluator_matches_kernel(void) {
    printf("Testing the evaluator and the derivative path agree...\n");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"step\", \"t_on\": 1.0, \"v0\": 1.0, \"v1\": 4.0 }", "step");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"ramp\", \"t_on\": 0.5, \"v0\": 1.0, \"slope\": 2.0 }", "ramp");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"sine\", \"mean\": 3.0, \"amplitude\": 1.5, \"period\": 4.0 }", "sine");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"square\", \"mean\": 2.0, \"amplitude\": 1.0, \"period\": 2.0, \"duty\": 0.5 }", "square");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"sawtooth\", \"mean\": 2.0, \"amplitude\": 1.0, \"period\": 3.0 }", "sawtooth");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"exponential\", \"v0\": 1.0, \"rate\": 0.2 }", "exponential");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"impulse\", \"t_on\": 1.0, \"area\": 2.0 }", "impulse");
    assert_evaluator_matches_kernel(
        "{ \"waveform\": \"jitter\", \"mean\": 3.0, \"amplitude\": 1.0 }", "jitter");
    printf("  Evaluator agrees with the kernel for all 8 waveforms\n");
}

/* ================================================================
 * 5. Attachment: edges, and what must be rejected
 * ================================================================ */

static void test_edge_forcing_drives_k(void) {
    printf("Testing forcing on an EDGE drives its rate k (Odum J)...\n");
    /* logic=constant means flow == k exactly, so the tank's slope IS the
     * forced k and nothing else can be responsible for it. */
    const char *MODEL =
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 1.0 },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\","
        "                 \"logic\": \"constant\", \"params\": { \"k\": 99.0 },"
        "                 \"forcing\": { \"waveform\": \"step\", \"t_on\": 1.0,"
        "                                \"v0\": 1.0, \"v1\": 3.0 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 4, \"dt\": 0.25, \"method\": \"euler\" } }";
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(MODEL, &inst);
    CHECK(st == GSSK_SUCCESS, "edge-forced model must load: %s",
          inst ? GSSK_GetErrorDescription(inst) : "");
    if (st == GSSK_SUCCESS) {
        CHECK(GSSK_GetEdgeForcingKind(inst, 0) == 1 /* STEP */,
              "edge forcing kind should be step, got %d", GSSK_GetEdgeForcingKind(inst, 0));
        /* params.k = 99 is a poison value: if forcing were ignored the slope
         * would be 99 and this test would fail loudly rather than subtly. */
        for (int s = 0; s < 8; s++) {
            double t = GSSK_GetCurrentTime(inst), dt = GSSK_GetDt(inst);
            double before = GSSK_GetState(inst)[1];
            GSSK_Step(inst, dt);
            double slope = (GSSK_GetState(inst)[1] - before) / dt;
            double want  = (t < 1.0) ? 1.0 : 3.0;
            CHECK(NEAR(slope, want, 1e-9),
                  "edge k at t=%g: flow %.12g, expected %.12g", t, slope, want);
        }
    }
    GSSK_Free(inst);
    printf("  Edge forcing OK\n");
}

static void expect_reject(const char *json, const char *what, const char *in_msg) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s should be GSSK_ERR_SCHEMA_VIOLATION, got %d", what, (int)st);
    if (inst && st == GSSK_ERR_SCHEMA_VIOLATION) {
        const char *msg = GSSK_GetErrorDescription(inst);
        CHECK(strstr(msg, in_msg) != NULL,
              "%s: message should mention \"%s\", got \"%s\"", what, in_msg, msg);
        printf("    %s\n", msg);
    }
    GSSK_Free(inst);
}

/* A storage node's value is the INTEGRAL of its flows, so forcing it is a
 * contradiction. Silently ignoring the block is precisely the failure mode
 * h8b exists to remove. */
static void test_storage_forcing_rejected(void) {
    printf("Testing forcing a storage node is rejected, not ignored...\n");
    expect_reject(
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 1.0 },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0,"
        "                 \"forcing\": { \"waveform\": \"sine\", \"mean\": 1, \"amplitude\": 1, \"period\": 2 } } ],"
        "  \"edges\": [ { \"origin\": \"src\", \"target\": \"tank\", \"logic\": \"linear\", \"params\": { \"k\": 1 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 } }",
        "forced storage node", "storage node");

    /* And at runtime, where GSSK_AddNode is a separate parser. */
    const char *BASE =
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 1.0 },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"origin\": \"src\", \"target\": \"tank\", \"logic\": \"linear\", \"params\": { \"k\": 1 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 } }";
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(BASE, &inst) == GSSK_SUCCESS);
    size_t n_before = GSSK_GetStateSize(inst);
    GSSK_Status st = GSSK_AddNode(inst,
        "{\"id\":\"t2\",\"type\":\"storage\",\"value\":1.0,"
        " \"forcing\":{\"waveform\":\"sine\",\"mean\":1,\"amplitude\":1,\"period\":2}}");
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION, "AddNode must reject a forced storage node, got %d", (int)st);
    CHECK(GSSK_GetStateSize(inst) == n_before, "rejected add must be a no-op");
    CHECK(GSSK_Step(inst, GSSK_GetDt(inst)) == GSSK_SUCCESS, "instance must still step");
    GSSK_Free(inst);
    printf("  Storage forcing rejected on both parsers\n");
}

static void test_authoring_mistakes_rejected(void) {
    printf("Testing forcing authoring mistakes are rejected...\n");
    char json[2048];
    const char *TMPL =
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 1.0, \"forcing\": %s },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"origin\": \"src\", \"target\": \"tank\", \"logic\": \"linear\", \"params\": { \"k\": 1 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 } }";

    snprintf(json, sizeof(json), TMPL, "{ \"waveform\": \"sinusoid\" }");
    expect_reject(json, "unknown waveform name", "sinusoid");

    snprintf(json, sizeof(json), TMPL, "{ \"amplitude\": 1 }");
    expect_reject(json, "missing waveform", "waveform");

    /* A periodic waveform with no period is an authoring mistake, not a slow
     * waveform — treating it as constant is the quiet-wrong-model failure. */
    snprintf(json, sizeof(json), TMPL, "{ \"waveform\": \"sine\", \"amplitude\": 1 }");
    expect_reject(json, "sine without a period", "period");

    snprintf(json, sizeof(json), TMPL,
             "{ \"waveform\": \"square\", \"period\": 2, \"duty\": 1.5 }");
    expect_reject(json, "duty outside [0,1]", "duty");

    snprintf(json, sizeof(json), TMPL,
             "{ \"waveform\": \"sine\", \"period\": 2, \"amplitude\": 1, \"freq\": 3 }");
    expect_reject(json, "unknown forcing key", "freq");

    snprintf(json, sizeof(json), TMPL,
             "{ \"waveform\": \"ramp\", \"slope\": 1, \"min\": 5, \"max\": 2 }");
    expect_reject(json, "min above max", "min");
    printf("  Authoring mistakes rejected\n");
}

/* ================================================================
 * 6. Round-trip — the C.3 defects were both missing serialiser cases
 * ================================================================ */

static void assert_round_trips(const char *forcing_json, const char *what) {
    GSSK_Instance *a = load_forced_source(forcing_json, ", \"method\": \"rk4\"");
    for (int i = 0; i < 10; i++) GSSK_Step(a, GSSK_GetDt(a));

    char *model = NULL;
    assert(GSSK_SerializeModel(a, &model) == GSSK_SUCCESS);
    CHECK(strstr(model, "\"forcing\"") != NULL,
          "%s: serialised model must carry the forcing block", what);

    GSSK_Instance *b = NULL;
    GSSK_Status st = GSSK_Init(model, &b);
    CHECK(st == GSSK_SUCCESS, "%s: serialised model must reload: %s", what,
          b ? GSSK_GetErrorDescription(b) : "");
    GSSK_FreeString(model);

    if (st == GSSK_SUCCESS) {
        CHECK(GSSK_GetNodeForcingKind(b, 0) == GSSK_GetNodeForcingKind(a, 0),
              "%s: waveform kind did not round-trip (%d -> %d)", what,
              GSSK_GetNodeForcingKind(a, 0), GSSK_GetNodeForcingKind(b, 0));
        /* Every parameter, via the evaluator, at several times — this is what
         * catches a field the serialiser forgot.
         *
         * Skipped for jitter, and deliberately: the evaluator reports the
         * LATCHED draw, and `a` has been stepped while `b` has not, so their
         * stream positions differ. Comparing them would be comparing run
         * history, not the parameters that round-tripped. The trajectory check
         * below covers jitter properly, from a common reset. */
        if (GSSK_GetNodeForcingKind(a, 0) != 8 /* JITTER */) {
            for (double t = 0.0; t <= 10.0; t += 0.7) {
                double va = GSSK_EvaluateNodeForcing(a, 0, t);
                double vb = GSSK_EvaluateNodeForcing(b, 0, t);
                CHECK(va == vb, "%s: forcing differs after round-trip at t=%g: %.17g vs %.17g",
                      what, t, va, vb);
            }
        }
        /* And the reloaded model reproduces the trajectory.
         *
         * GSSK_Reset deliberately does NOT rewind the RNG stream — ensemble
         * forecasting depends on it not doing so — so a jitter model needs the
         * stream rewound explicitly. This is the documented way to repeat a
         * stochastic run, and asserting it here pins that contract. */
        GSSK_SetSeed(a, GSSK_GetSeed(a));
        GSSK_SetSeed(b, GSSK_GetSeed(b));
        GSSK_Reset(a);
        GSSK_Reset(b);
        for (int i = 0; i < 30; i++) { GSSK_Step(a, GSSK_GetDt(a)); GSSK_Step(b, GSSK_GetDt(b)); }
        CHECK(NEAR(GSSK_GetState(a)[1], GSSK_GetState(b)[1], 1e-12),
              "%s: trajectory diverged after round-trip: %.17g vs %.17g",
              what, GSSK_GetState(a)[1], GSSK_GetState(b)[1]);
    }
    GSSK_Free(a); GSSK_Free(b);
}

static void test_round_trip(void) {
    printf("Testing every waveform round-trips through the serialiser...\n");
    assert_round_trips("{ \"waveform\": \"step\", \"t_on\": 2, \"v0\": 1, \"v1\": 5 }", "step");
    assert_round_trips("{ \"waveform\": \"impulse\", \"t_on\": 2, \"area\": 7 }", "impulse");
    assert_round_trips("{ \"waveform\": \"ramp\", \"t_on\": 1, \"v0\": 2, \"slope\": 0.5, \"max\": 6 }", "ramp");
    assert_round_trips("{ \"waveform\": \"sawtooth\", \"mean\": 3, \"amplitude\": 1, \"period\": 4, \"phase\": 0.5 }", "sawtooth");
    assert_round_trips("{ \"waveform\": \"square\", \"mean\": 3, \"amplitude\": 1, \"period\": 4, \"duty\": 0.3 }", "square");
    assert_round_trips("{ \"waveform\": \"sine\", \"mean\": 3, \"amplitude\": 1, \"period\": 4, \"phase\": 1.25, \"min\": 2.5 }", "sine");
    assert_round_trips("{ \"waveform\": \"exponential\", \"v0\": 1, \"rate\": 0.1, \"max\": 4 }", "exponential");
    assert_round_trips("{ \"waveform\": \"jitter\", \"mean\": 3, \"amplitude\": 1 }", "jitter");
    printf("  All 8 waveforms round-trip\n");
}

/* An unforced model must behave EXACTLY as before. */
static void test_unforced_unchanged(void) {
    printf("Testing an unforced model is untouched...\n");
    const char *MODEL =
        "{ \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 2.0 },"
        "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
        "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\","
        "                 \"logic\": \"linear\", \"params\": { \"k\": 0.5 } } ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 4, \"dt\": 0.1, \"method\": \"rk4\" } }";
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(MODEL, &inst) == GSSK_SUCCESS);
    CHECK(GSSK_GetNodeForcingKind(inst, 0) == 0, "unforced node must report kind 0");
    CHECK(GSSK_GetEdgeForcingKind(inst, 0) == 0, "unforced edge must report kind 0");
    /* The evaluator falls back to the declared value/k, so a consumer can plot
     * every element without first asking which are forced. */
    CHECK(GSSK_EvaluateNodeForcing(inst, 0, 3.3) == 2.0, "unforced node evaluates to its value");
    CHECK(GSSK_EvaluateEdgeForcing(inst, 0, 3.3) == 0.5, "unforced edge evaluates to its k");
    /* flow = k*Q = 0.5*2 = 1.0 constant, so tank(t) = t. */
    for (int i = 0; i < 40; i++) GSSK_Step(inst, GSSK_GetDt(inst));
    CHECK(NEAR(GSSK_GetState(inst)[1], 4.0, 1e-9),
          "unforced trajectory changed: tank=%.12g, expected 4.0", GSSK_GetState(inst)[1]);
    /* Out-of-range is quiet and safe. */
    CHECK(GSSK_GetNodeForcingKind(inst, 99) == 0, "OOB node kind is 0");
    CHECK(GSSK_EvaluateNodeForcing(inst, 99, 1.0) == 0.0, "OOB node evaluates to 0");
    CHECK(GSSK_EvaluateEdgeForcing(NULL, 0, 1.0) == 0.0, "NULL instance evaluates to 0");
    GSSK_Free(inst);
    printf("  Unforced models unchanged\n");
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== Forcing function tests ===\n\n");

    test_step();
    test_impulse_area_is_dt_independent();
    test_ramp();
    test_sine();
    test_square_sawtooth();
    test_exponential();
    test_fourth_order_convergence();
    test_jitter_determinism();
    test_jitter_snapshot_resume();
    test_evaluator_matches_kernel();
    test_edge_forcing_drives_k();
    test_storage_forcing_rejected();
    test_authoring_mistakes_rejected();
    test_round_trip();
    test_unforced_unchanged();

    printf("\n");
    if (failures) { printf("=== FAILED (%d) ===\n", failures); return 1; }
    printf("=== ALL FORCING TESTS PASSED ===\n");
    return 0;
}
