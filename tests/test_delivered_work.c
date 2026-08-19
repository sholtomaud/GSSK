/* Phase C.2 — delivered work (W) as explicit low-pass state, per ADR 0003.
 *
 * Two properties matter and both are asserted here:
 *   1. W tracks the delivered-work inflow and falls when that inflow falls.
 *   2. The tap that feeds W does not perturb the trade it observes. That is
 *      only true because compute_derivatives pins sources/constants/processing
 *      nodes to dQ/dt = 0, so a parallel edge off a pinned origin is free. If
 *      that ever changes, this test fails rather than the error hiding in a
 *      slightly wrong price three tasks downstream. */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int failures = 0;
static void ok(const char *what, int cond) {
    printf("  %-56s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

#define COMMON \
  "{\"metadata\":{\"schema_version\":4}," \
  "\"carriers\":[{\"id\":\"money\",\"unit\":\"AUD\",\"conserved\":true}," \
  "              {\"id\":\"goods\",\"unit\":\"kg\",\"conserved\":true}," \
  "              {\"id\":\"signal\",\"unit\":\"x\",\"conserved\":false}]," \
  "\"nodes\":[" \
  "  {\"id\":\"seller\",\"type\":\"source\",\"value\":1.0,\"carrier\":\"goods\"}," \
  "  {\"id\":\"buyer\",\"type\":\"storage\",\"value\":1000.0,\"carrier\":\"money\"}," \
  "  {\"id\":\"ex\",\"type\":\"exchange\",\"value\":0.0,\"carrier\":\"goods\"," \
  "   \"params\":{\"k\":0.01,\"price\":5.0}}," \
  "  {\"id\":\"inventory\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"goods\"}," \
  "  {\"id\":\"spent\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"money\"}"

#define EDGES_COMMON \
  "\"edges\":[" \
  "  {\"id\":\"e1\",\"origin\":\"seller\",\"target\":\"ex\",\"carrier\":\"goods\"}," \
  "  {\"id\":\"e2\",\"origin\":\"buyer\",\"target\":\"ex\",\"carrier\":\"money\"}," \
  "  {\"id\":\"e3\",\"origin\":\"ex\",\"target\":\"inventory\",\"carrier\":\"goods\"}," \
  "  {\"id\":\"e4\",\"origin\":\"ex\",\"target\":\"spent\",\"carrier\":\"money\"}"

static const char *WITH_TAP = COMMON
  ",{\"id\":\"W\",\"type\":\"storage\",\"value\":0.0,\"carrier\":\"signal\"}"
  ",{\"id\":\"W_leak\",\"type\":\"sink\",\"value\":0.0,\"carrier\":\"signal\"}],"
  EDGES_COMMON
  ",{\"id\":\"w_tap\",\"origin\":\"seller\",\"target\":\"W\",\"carrier\":\"signal\","
  "  \"logic\":\"interaction\",\"params\":{\"k\":0.01,\"control_node\":\"buyer\"}}"
  ",{\"id\":\"w_leak\",\"origin\":\"W\",\"target\":\"W_leak\",\"carrier\":\"signal\","
  "  \"logic\":\"linear\",\"params\":{\"k\":0.5}}],"
  "\"config\":{\"t_start\":0,\"t_end\":20,\"dt\":0.1,\"method\":\"rk4\"}}";

static const char *NO_TAP = COMMON "]," EDGES_COMMON "],"
  "\"config\":{\"t_start\":0,\"t_end\":20,\"dt\":0.1,\"method\":\"rk4\"}}";

int main(void) {
    printf("Phase C.2 — delivered work signal\n\n");

    GSSK_Instance *a = NULL, *b = NULL;
    if (GSSK_Init(WITH_TAP, &a) != GSSK_SUCCESS) {
        printf("init(with tap) failed: %s\n", GSSK_GetErrorDescription(a));
        return 1;
    }
    if (GSSK_Init(NO_TAP, &b) != GSSK_SUCCESS) {
        printf("init(no tap) failed: %s\n", GSSK_GetErrorDescription(b));
        return 1;
    }

    int wi = GSSK_FindNodeIdx(a, "W");
    int inv_a = GSSK_FindNodeIdx(a, "inventory"), inv_b = GSSK_FindNodeIdx(b, "inventory");
    int buy_a = GSSK_FindNodeIdx(a, "buyer"),     buy_b = GSSK_FindNodeIdx(b, "buyer");

    double w_peak = 0.0, w_prev = 0.0, worst_drift = 0.0;
    int fell_after_peak = 1, peaked = 0;

    for (int i = 0; i < 200; i++) {
        GSSK_Step(a, GSSK_GetDt(a));
        GSSK_Step(b, GSSK_GetDt(b));

        double w = GSSK_GetState(a)[wi];
        if (w > w_peak) { w_peak = w; }
        else if (w_peak > 0.0) { peaked = 1; if (w > w_prev + 1e-12) fell_after_peak = 0; }
        w_prev = w;

        double d1 = fabs(GSSK_GetState(a)[inv_a] - GSSK_GetState(b)[inv_b]);
        double d2 = fabs(GSSK_GetState(a)[buy_a] - GSSK_GetState(b)[buy_b]);
        if (d1 > worst_drift) worst_drift = d1;
        if (d2 > worst_drift) worst_drift = d2;
    }

    ok("W rises from zero as delivery begins", w_peak > 0.0);
    ok("W peaks, then falls as the inflow falls", peaked && fell_after_peak);
    printf("    W peak=%.6f final=%.6f\n", w_peak, GSSK_GetState(a)[wi]);

    printf("    max |with_tap - without_tap| = %.3e\n", worst_drift);
    ok("the tap does not perturb the observed trade", worst_drift == 0.0);

    GSSK_Free(a); GSSK_Free(b);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
