/* Phase C.1 — ratio (division) edge logic.
 *
 * F = k x Q_origin / max(Q_control, epsilon)
 *
 * Covers the three acceptance criteria: the flow matches a hand-calculated
 * ratio, the denominator floor saturates instead of diverging as the control
 * goes to zero, and the RK4 and IDC paths agree on a smooth case. */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int failures = 0;

static void ok(const char *what, bool cond) {
    printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static void close_to(const char *what, double got, double want, double tol) {
    bool c = fabs(got - want) <= tol;
    printf("  %-58s %s  (got %.10g want %.10g)\n", what, c ? "PASS" : "FAIL",
           got, want);
    if (!c) failures++;
}

/* A ratio edge draining storage `num` into `out`, with `den` as denominator.
 * `den` is a constant node so the quotient is stationary and hand-checkable. */
static char *ratio_model(double num, double den, double k,
                         double threshold, const char *method) {
    static char buf[1400];
    char thr[64] = "";
    if (threshold > 0.0)
        snprintf(thr, sizeof(thr), ",\"threshold\":%.17g", threshold);
    snprintf(buf, sizeof(buf),
      "{\"metadata\":{\"schema_version\":4},"
      "\"nodes\":["
      "  {\"id\":\"num\",\"type\":\"source\",\"value\":%.17g},"
      "  {\"id\":\"den\",\"type\":\"constant\",\"value\":%.17g},"
      "  {\"id\":\"out\",\"type\":\"storage\",\"value\":0.0}"
      "],"
      "\"edges\":[{\"id\":\"r\",\"origin\":\"num\",\"target\":\"out\","
      "  \"logic\":\"ratio\",\"params\":{\"k\":%.17g,\"control_node\":\"den\"%s}}],"
      "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"%s\"}}",
      num, den, k, thr, method);
    return buf;
}

/* `num` is a source, so it is not depleted and the flow is constant. RK4
 * integrates a constant exactly, so out(t) = k*(num/den)*t to machine
 * precision — a hand calculation, not a golden number. */
static double run_out(const char *json, double *out_final) {
    GSSK_Instance *inst = NULL;
    if (GSSK_Init(json, &inst) != GSSK_SUCCESS) {
        printf("    init failed: %s\n", GSSK_GetErrorDescription(inst));
        GSSK_Free(inst);
        return -1.0;
    }
    int oi = GSSK_FindNodeIdx(inst, "out");
    for (int i = 0; i < 10; i++) GSSK_Step(inst, GSSK_GetDt(inst));
    *out_final = GSSK_GetState(inst)[oi];
    GSSK_Free(inst);
    return 0.0;
}

static void test_hand_calculated_ratio(void) {
    printf("P = M/W matches a hand calculation...\n");
    struct { double m, w, k; } cases[] = {
        { 100.0, 4.0,  1.0 },
        { 100.0, 8.0,  1.0 },
        {  50.0, 2.5,  2.0 },
        {   1.0, 3.0,  1.0 },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        double m = cases[i].m, w = cases[i].w, k = cases[i].k;
        double got = 0.0;
        run_out(ratio_model(m, w, k, 0.0, "rk4"), &got);
        char label[96];
        snprintf(label, sizeof(label), "%.4g/%.4g x k=%.4g over t=1", m, w, k);
        /* out = k*(m/w)*1.0 */
        close_to(label, got, k * (m / w), 1e-9);
    }
}

static void test_epsilon_floor(void) {
    printf("denominator floor saturates instead of diverging...\n");

    /* W = 0 exactly. Without the floor this is a division by zero. */
    double got = 0.0;
    run_out(ratio_model(100.0, 0.0, 1.0, 0.0, "rk4"), &got);
    ok("W = 0 produces a finite result", isfinite(got));
    ok("W = 0 does not produce NaN", !isnan(got));
    /* Saturated flow = k*M/eps, integrated over t=1. */
    close_to("W = 0 saturates at k*M/GSSK_RATIO_EPSILON",
             got, 100.0 / GSSK_RATIO_EPSILON, 1.0);

    /* Negative control must not invert the flow: the floor clamps it. */
    got = 0.0;
    run_out(ratio_model(100.0, -5.0, 1.0, 0.0, "rk4"), &got);
    ok("negative W is clamped, flow stays positive", got > 0.0);

    /* params.threshold overrides the floor, bounding the ratio usefully. */
    got = 0.0;
    run_out(ratio_model(100.0, 0.0, 1.0, 0.5, "rk4"), &got);
    close_to("threshold=0.5 floors the denominator at 0.5",
             got, 100.0 / 0.5, 1e-9);

    /* Above the floor the override must not perturb a normal quotient. */
    got = 0.0;
    run_out(ratio_model(100.0, 4.0, 1.0, 0.5, "rk4"), &got);
    close_to("control above the floor is unaffected by threshold",
             got, 100.0 / 4.0, 1e-9);
}

/* Storage numerator so the flow actually decays — a smooth, non-trivial case
 * where IDC has something to integrate rather than a constant. */
static const char *SMOOTH_MODEL =
    "{\"metadata\":{\"schema_version\":4},"
    "\"nodes\":["
    "  {\"id\":\"A\",\"type\":\"storage\",\"value\":100.0},"
    "  {\"id\":\"den\",\"type\":\"constant\",\"value\":4.0},"
    "  {\"id\":\"B\",\"type\":\"storage\",\"value\":0.0}"
    "],"
    "\"edges\":[{\"id\":\"r\",\"origin\":\"A\",\"target\":\"B\","
    "  \"logic\":\"ratio\",\"params\":{\"k\":0.1,\"control_node\":\"den\"}}],"
    "\"config\":{\"t_start\":0,\"t_end\":5,\"dt\":0.1,\"method\":\"%s\"}}";

static void run_smooth(const char *method, double *a_out, double *b_out) {
    char json[900];
    snprintf(json, sizeof(json), SMOOTH_MODEL, method);
    GSSK_Instance *inst = NULL;
    GSSK_Init(json, &inst);
    int ai = GSSK_FindNodeIdx(inst, "A"), bi = GSSK_FindNodeIdx(inst, "B");
    for (int i = 0; i < 50; i++) GSSK_Step(inst, GSSK_GetDt(inst));
    *a_out = GSSK_GetState(inst)[ai];
    *b_out = GSSK_GetState(inst)[bi];
    GSSK_Free(inst);
}

static void test_rk4_idc_agree(void) {
    printf("RK4 and IDC agree on a smooth case...\n");
    double a_rk4, b_rk4, a_idc, b_idc;
    run_smooth("rk4", &a_rk4, &b_rk4);
    run_smooth("incipient", &a_idc, &b_idc);

    /* F = 0.1*A/4 = 0.025*A, so A decays as exp(-0.025 t); at t=5,
     * A = 100*exp(-0.125). Both solvers must land there. */
    double analytic = 100.0 * exp(-0.125);
    close_to("RK4 matches the analytic solution",  a_rk4, analytic, 1e-6);
    close_to("IDC matches the analytic solution",  a_idc, analytic, 1e-6);

    double rel = fabs(a_rk4 - a_idc) / fabs(a_rk4);
    printf("    A: rk4=%.12g idc=%.12g  rel=%.3g\n", a_rk4, a_idc, rel);
    ok("RK4 vs IDC within solver tolerance (1e-6)", rel < 1e-6);

    /* Conservation: what leaves A must arrive at B. */
    close_to("A + B conserved", a_rk4 + b_rk4, 100.0, 1e-6);
}

int main(void) {
    printf("Phase C.1 — ratio logic\n\n");
    test_hand_calculated_ratio();
    printf("\n");
    test_epsilon_floor();
    printf("\n");
    test_rk4_idc_agree();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
