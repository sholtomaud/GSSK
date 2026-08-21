/* Phase C.3 — price as a relaxation toward Odum's ratio: dP/dt = α(M/W − P).
 *
 * Two things are under test and they are separable:
 *
 *   1. `numerator_node` — the ratio primitive's numerator becomes a NAMED,
 *      non-consumed operand (ADR 0005), completing what ADR 0002 called a
 *      ratio whose numerator and denominator are "both named and
 *      distinguishable". The properties that matter are that the quotient is
 *      formed from the named node, that reading it costs it nothing, and that
 *      every solver path agrees — the Jacobian and the IDC flow matrix must
 *      differentiate with respect to the numerator, not the origin, and that
 *      divergence is invisible in plain RK4.
 *
 *   2. The relaxation itself — that P converges to M/W exactly (not merely
 *      proportional to it, which is all the Tier 1 anchor achieved), that α
 *      is the time constant, and that P rises when W falls.
 *
 * Build: make test-price-dynamics
 */
#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-58s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}

static void close_to(const char *what, double got, double want, double tol) {
  int c = fabs(got - want) <= tol;
  printf("  %-58s %s  (got %.10g want %.10g)\n", what, c ? "PASS" : "FAIL",
         got, want);
  if (!c) failures++;
}

/* ---------------------------------------------------------------------------
 * The relaxation, in isolation and with an analytic solution.
 *
 * M and W are `constant` nodes, so M/W is stationary and dP/dt = α(M/W − P)
 * integrates to P(t) = (M/W)(1 − e^{−αt}) from P(0)=0. Both the fixed point
 * and the time constant are therefore hand-checkable rather than golden.
 * ------------------------------------------------------------------------- */
static char *relax_model(double m, double w, double alpha, const char *method) {
  static char buf[1200];
  snprintf(buf, sizeof(buf),
    "{\"metadata\":{\"schema_version\":4},"
    "\"nodes\":["
    "  {\"id\":\"M\",\"type\":\"constant\",\"value\":%.17g},"
    "  {\"id\":\"W\",\"type\":\"constant\",\"value\":%.17g},"
    "  {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
    "  {\"id\":\"P\",\"type\":\"storage\",\"value\":0.0},"
    "  {\"id\":\"clearing\",\"type\":\"sink\",\"value\":0.0}"
    "],"
    "\"edges\":["
    "  {\"id\":\"p_target\",\"origin\":\"unity\",\"target\":\"P\","
    "   \"logic\":\"ratio\","
    "   \"params\":{\"k\":%.17g,\"numerator_node\":\"M\",\"control_node\":\"W\"}},"
    "  {\"id\":\"p_relax\",\"origin\":\"P\",\"target\":\"clearing\","
    "   \"logic\":\"linear\",\"params\":{\"k\":%.17g}}"
    "],"
    "\"config\":{\"t_start\":0,\"t_end\":50,\"dt\":0.01,\"method\":\"%s\"}}",
    m, w, alpha, alpha, method);
  return buf;
}

/* Steps `n` times and returns P. */
static double run_relax(const char *json, int n, double *p_out) {
  GSSK_Instance *inst = NULL;
  if (GSSK_Init(json, &inst) != GSSK_SUCCESS) {
    printf("    init failed: %s\n", GSSK_GetErrorDescription(inst));
    GSSK_Free(inst);
    return -1.0;
  }
  int pi = GSSK_FindNodeIdx(inst, "P");
  for (int i = 0; i < n; i++) GSSK_Step(inst, GSSK_GetDt(inst));
  *p_out = GSSK_GetState(inst)[pi];
  GSSK_Free(inst);
  return 0.0;
}

static void test_fixed_point_is_the_ratio(void) {
  printf("P converges to M/W itself, not to something proportional to it...\n");
  struct { double m, w; } cases[] = {
    { 100.0, 4.0 }, { 100.0, 8.0 }, { 50.0, 2.5 }, { 7.0, 3.0 },
  };
  for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    double p = -1.0;
    /* 5000 steps x dt=0.01 = t=50; with α=0.8, e^{-40} is ~4e-18. */
    run_relax(relax_model(cases[i].m, cases[i].w, 0.8, "rk4"), 5000, &p);
    char label[96];
    snprintf(label, sizeof(label), "M=%.4g W=%.4g settles at M/W",
             cases[i].m, cases[i].w);
    close_to(label, p, cases[i].m / cases[i].w, 1e-9);
  }
}

static void test_alpha_is_the_time_constant(void) {
  printf("alpha is settable per model and sets the adjustment time constant...\n");
  /* P(t) = (M/W)(1 − e^{−αt}); at t = 1/α that is exactly (M/W)(1 − 1/e). */
  double target = 100.0 / 4.0;
  struct { double alpha; int steps_to_tau; } cases[] = {
    { 0.25, 400 },   /* 1/α = 4.0  → 400 steps of dt=0.01 */
    { 0.5,  200 },   /* 1/α = 2.0 */
    { 2.0,   50 },   /* 1/α = 0.5 */
  };
  for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    double p = -1.0;
    run_relax(relax_model(100.0, 4.0, cases[i].alpha, "rk4"),
              cases[i].steps_to_tau, &p);
    char label[96];
    snprintf(label, sizeof(label), "alpha=%.4g reaches 1−1/e of M/W at t=1/alpha",
             cases[i].alpha);
    close_to(label, p, target * (1.0 - exp(-1.0)), 1e-6);
  }

  /* And the ordering that "time constant" implies: at a fixed early time, a
   * larger α is strictly further along. */
  double p_slow = -1.0, p_fast = -1.0;
  run_relax(relax_model(100.0, 4.0, 0.25, "rk4"), 100, &p_slow);
  run_relax(relax_model(100.0, 4.0, 2.0,  "rk4"), 100, &p_fast);
  printf("    at t=1: alpha=0.25 → P=%.6f, alpha=2.0 → P=%.6f\n", p_slow, p_fast);
  ok("larger alpha adjusts faster", p_fast > p_slow);
}

static void test_rk4_idc_agree(void) {
  printf("RK4 and IDC agree on the relaxation...\n");
  double p_rk4 = -1.0, p_idc = -1.0;
  run_relax(relax_model(100.0, 4.0, 0.8, "rk4"),       200, &p_rk4);
  run_relax(relax_model(100.0, 4.0, 0.8, "incipient"), 200, &p_idc);
  double analytic = 25.0 * (1.0 - exp(-1.6));
  close_to("RK4 matches the analytic solution", p_rk4, analytic, 1e-6);
  /* The IDC path reaches the numerator through build_flow_matrix, which has to
   * put the conductance in the numerator's column. Getting that wrong is
   * invisible under RK4 and shows up only here. */
  close_to("IDC matches the analytic solution", p_idc, analytic, 1e-6);
}

/* ---------------------------------------------------------------------------
 * M falls, W falls faster: P must rise.
 *
 * The acceptance criterion is "M held constant and W falling ⇒ P rises
 * monotonically", so M is a `constant` node here and W is a storage draining
 * at a fixed rate. P starts at the initial M/W and must never step down.
 * ------------------------------------------------------------------------- */
static const char *RISING_PRICE =
  "{\"metadata\":{\"schema_version\":4},"
  "\"nodes\":["
  "  {\"id\":\"M\",\"type\":\"constant\",\"value\":100.0},"
  "  {\"id\":\"W\",\"type\":\"storage\",\"value\":20.0},"
  "  {\"id\":\"W_leak\",\"type\":\"sink\",\"value\":0.0},"
  "  {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
  "  {\"id\":\"P\",\"type\":\"storage\",\"value\":5.0},"
  "  {\"id\":\"clearing\",\"type\":\"sink\",\"value\":0.0}"
  "],"
  "\"edges\":["
  "  {\"id\":\"w_leak\",\"origin\":\"W\",\"target\":\"W_leak\","
  "   \"logic\":\"linear\",\"params\":{\"k\":0.1}},"
  "  {\"id\":\"p_target\",\"origin\":\"unity\",\"target\":\"P\","
  "   \"logic\":\"ratio\","
  "   \"params\":{\"k\":0.5,\"numerator_node\":\"M\",\"control_node\":\"W\"}},"
  "  {\"id\":\"p_relax\",\"origin\":\"P\",\"target\":\"clearing\","
  "   \"logic\":\"linear\",\"params\":{\"k\":0.5}}"
  "],"
  "\"config\":{\"t_start\":0,\"t_end\":20,\"dt\":0.01,\"method\":\"rk4\"}}";

static void test_falling_work_raises_price(void) {
  printf("M constant, W falling: P rises monotonically...\n");
  GSSK_Instance *inst = NULL;
  if (GSSK_Init(RISING_PRICE, &inst) != GSSK_SUCCESS) {
    printf("    init failed: %s\n", GSSK_GetErrorDescription(inst));
    GSSK_Free(inst); failures++; return;
  }
  int pi = GSSK_FindNodeIdx(inst, "P"), wi = GSSK_FindNodeIdx(inst, "W");
  double prev = GSSK_GetState(inst)[pi];
  double p0 = prev, w0 = GSSK_GetState(inst)[wi];
  int monotonic = 1;
  for (int i = 0; i < 2000; i++) {
    GSSK_Step(inst, GSSK_GetDt(inst));
    double p = GSSK_GetState(inst)[pi];
    if (p < prev) monotonic = 0;
    prev = p;
  }
  double w1 = GSSK_GetState(inst)[wi];
  printf("    W: %.4f → %.4f    P: %.4f → %.4f\n", w0, w1, p0, prev);
  ok("W actually fell", w1 < w0);
  ok("P rose", prev > p0);
  ok("P never stepped down", monotonic);
  GSSK_Free(inst);
}

/* ---------------------------------------------------------------------------
 * Reading the numerator costs it nothing.
 *
 * This is the property that makes `numerator_node` worth having: an edge
 * debits its origin, so before this the only way to put a stock into the
 * numerator was to drain it. The control model is the same network with the
 * price subgraph deleted; M must be bit-identical across 2000 steps.
 * ------------------------------------------------------------------------- */
#define NONPERTURB_HEAD \
  "{\"metadata\":{\"schema_version\":4}," \
  "\"nodes\":[" \
  "  {\"id\":\"M\",\"type\":\"storage\",\"value\":1000.0}," \
  "  {\"id\":\"drain\",\"type\":\"sink\",\"value\":0.0}," \
  "  {\"id\":\"W\",\"type\":\"constant\",\"value\":8.0}"

#define NONPERTURB_SPEND \
  "\"edges\":[" \
  "  {\"id\":\"spend\",\"origin\":\"M\",\"target\":\"drain\"," \
  "   \"logic\":\"linear\",\"params\":{\"k\":0.05}}"

#define NONPERTURB_TAIL \
  "\"config\":{\"t_start\":0,\"t_end\":20,\"dt\":0.01,\"method\":\"rk4\"}}"

static const char *WITH_PRICE = NONPERTURB_HEAD
  ",{\"id\":\"unity\",\"type\":\"source\",\"value\":1.0}"
  ",{\"id\":\"P\",\"type\":\"storage\",\"value\":0.0}"
  ",{\"id\":\"clearing\",\"type\":\"sink\",\"value\":0.0}],"
  NONPERTURB_SPEND
  ",{\"id\":\"p_target\",\"origin\":\"unity\",\"target\":\"P\","
  "  \"logic\":\"ratio\","
  "  \"params\":{\"k\":0.5,\"numerator_node\":\"M\",\"control_node\":\"W\"}}"
  ",{\"id\":\"p_relax\",\"origin\":\"P\",\"target\":\"clearing\","
  "  \"logic\":\"linear\",\"params\":{\"k\":0.5}}],"
  NONPERTURB_TAIL;

static const char *WITHOUT_PRICE = NONPERTURB_HEAD "],"
  NONPERTURB_SPEND "],"
  NONPERTURB_TAIL;

static void test_numerator_is_not_consumed(void) {
  printf("the named numerator is read without being debited...\n");
  GSSK_Instance *a = NULL, *b = NULL;
  if (GSSK_Init(WITH_PRICE, &a) != GSSK_SUCCESS ||
      GSSK_Init(WITHOUT_PRICE, &b) != GSSK_SUCCESS) {
    printf("    init failed: %s\n", GSSK_GetErrorDescription(a ? a : b));
    GSSK_Free(a); GSSK_Free(b); failures++; return;
  }
  int ma = GSSK_FindNodeIdx(a, "M"), mb = GSSK_FindNodeIdx(b, "M");
  int pa = GSSK_FindNodeIdx(a, "P");
  double worst = 0.0;
  for (int i = 0; i < 2000; i++) {
    GSSK_Step(a, GSSK_GetDt(a));
    GSSK_Step(b, GSSK_GetDt(b));
    double d = fabs(GSSK_GetState(a)[ma] - GSSK_GetState(b)[mb]);
    if (d > worst) worst = d;
  }
  printf("    max |with_price − without_price| on M = %.3e\n", worst);
  ok("reading M as numerator does not perturb M", worst == 0.0);
  /* ...and the price really was computed, so the above is not vacuous. */
  ok("P tracked a non-trivial value", GSSK_GetState(a)[pa] > 0.0);
  GSSK_Free(a); GSSK_Free(b);
}

/* ---------------------------------------------------------------------------
 * Authoring errors and round-trip.
 * ------------------------------------------------------------------------- */
static int init_fails(const char *json, const char *want_substr) {
  GSSK_Instance *inst = NULL;
  GSSK_Status st = GSSK_Init(json, &inst);
  int bad = (st != GSSK_SUCCESS);
  if (bad && want_substr) {
    const char *msg = GSSK_GetErrorDescription(inst);
    bad = (msg && strstr(msg, want_substr) != NULL);
    if (!bad) printf("    error was: %s\n", msg ? msg : "(none)");
  }
  GSSK_Free(inst);
  return bad;
}

static const char *BAD_UNKNOWN_NUMERATOR =
  "{\"metadata\":{\"schema_version\":4},"
  "\"nodes\":[{\"id\":\"W\",\"type\":\"constant\",\"value\":4.0},"
  "          {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
  "          {\"id\":\"P\",\"type\":\"storage\",\"value\":0.0}],"
  "\"edges\":[{\"id\":\"r\",\"origin\":\"unity\",\"target\":\"P\","
  "  \"logic\":\"ratio\","
  "  \"params\":{\"k\":1.0,\"numerator_node\":\"nope\",\"control_node\":\"W\"}}],"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

static const char *BAD_NUMERATOR_ON_LINEAR =
  "{\"metadata\":{\"schema_version\":4},"
  "\"nodes\":[{\"id\":\"M\",\"type\":\"constant\",\"value\":100.0},"
  "          {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
  "          {\"id\":\"P\",\"type\":\"storage\",\"value\":0.0}],"
  "\"edges\":[{\"id\":\"r\",\"origin\":\"unity\",\"target\":\"P\","
  "  \"logic\":\"linear\",\"params\":{\"k\":1.0,\"numerator_node\":\"M\"}}],"
  "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

static void test_authoring_errors(void) {
  printf("authoring mistakes are rejected, not silently reinterpreted...\n");
  ok("unknown numerator_node id is a linkage error",
     init_fails(BAD_UNKNOWN_NUMERATOR, "numerator_node"));
  /* Ignoring this would leave a model that reads as though the quotient were
   * wired up while the kernel quietly used Q_origin. */
  ok("numerator_node on non-ratio logic is a logic error",
     init_fails(BAD_NUMERATOR_ON_LINEAR, "numerator_node"));
}

static void test_round_trip(void) {
  printf("numerator_node and the ratio floor survive serialization...\n");
  /* threshold on a `ratio` edge is the denominator floor. It used to be
   * emitted only for `threshold` logic, so a round-trip silently replaced a
   * deliberate floor with GSSK_RATIO_EPSILON — a 1e7x change in the saturated
   * price. Fixed alongside this task; asserted here so it stays fixed. */
  static const char *MODEL =
    "{\"metadata\":{\"schema_version\":4},"
    "\"nodes\":[{\"id\":\"M\",\"type\":\"constant\",\"value\":100.0},"
    "          {\"id\":\"W\",\"type\":\"constant\",\"value\":4.0},"
    "          {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
    "          {\"id\":\"P\",\"type\":\"storage\",\"value\":0.0}],"
    "\"edges\":[{\"id\":\"r\",\"origin\":\"unity\",\"target\":\"P\","
    "  \"logic\":\"ratio\",\"params\":{\"k\":0.5,\"numerator_node\":\"M\","
    "  \"control_node\":\"W\",\"threshold\":0.25}}],"
    "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";

  GSSK_Instance *inst = NULL;
  char *out = NULL;
  if (GSSK_Init(MODEL, &inst) != GSSK_SUCCESS ||
      GSSK_SerializeModel(inst, &out) != GSSK_SUCCESS || !out) {
    ok("serialize succeeded", 0);
    ok("numerator_node round-trips", 0);
    ok("ratio threshold round-trips", 0);
    GSSK_Free(inst);
    return;
  }
  ok("serialize succeeded", 1);
  ok("numerator_node round-trips", strstr(out, "\"numerator_node\"") != NULL &&
                                   strstr(out, "\"M\"") != NULL);
  ok("ratio threshold round-trips", strstr(out, "\"threshold\"") != NULL);

  /* Reloading the emitted model must reproduce the same trajectory. */
  GSSK_Instance *re = NULL;
  if (GSSK_Init(out, &re) == GSSK_SUCCESS) {
    int p1 = GSSK_FindNodeIdx(inst, "P"), p2 = GSSK_FindNodeIdx(re, "P");
    for (int i = 0; i < 10; i++) {
      GSSK_Step(inst, GSSK_GetDt(inst));
      GSSK_Step(re, GSSK_GetDt(re));
    }
    ok("reloaded model reproduces the trajectory",
       GSSK_GetState(inst)[p1] == GSSK_GetState(re)[p2]);
  } else {
    ok("reloaded model reproduces the trajectory", 0);
  }
  GSSK_Free(re);
  GSSK_FreeString(out);
  GSSK_Free(inst);
}

/* Backward compatibility: an omitted numerator_node must still mean Q_origin,
 * bit-for-bit. test_ratio.c covers the primitive; this covers the claim
 * directly against a model that differs only in that key. */
static void test_omitted_numerator_is_origin(void) {
  printf("omitting numerator_node still means Q_origin...\n");
  static const char *FMT =
    "{\"metadata\":{\"schema_version\":4},"
    "\"nodes\":[{\"id\":\"A\",\"type\":\"source\",\"value\":100.0},"
    "          {\"id\":\"W\",\"type\":\"constant\",\"value\":4.0},"
    "          {\"id\":\"B\",\"type\":\"storage\",\"value\":0.0}],"
    "\"edges\":[{\"id\":\"r\",\"origin\":\"A\",\"target\":\"B\","
    "  \"logic\":\"ratio\",\"params\":{\"k\":1.0%s,\"control_node\":\"W\"}}],"
    "\"config\":{\"t_start\":0,\"t_end\":1,\"dt\":0.1,\"method\":\"rk4\"}}";
  char implicit[900], explicit_[900];
  snprintf(implicit,  sizeof(implicit),  FMT, "");
  snprintf(explicit_, sizeof(explicit_), FMT, ",\"numerator_node\":\"A\"");

  GSSK_Instance *a = NULL, *b = NULL;
  GSSK_Init(implicit, &a);
  GSSK_Init(explicit_, &b);
  if (!a || !b) { ok("both models init", 0); GSSK_Free(a); GSSK_Free(b); return; }
  int ba = GSSK_FindNodeIdx(a, "B"), bb = GSSK_FindNodeIdx(b, "B");
  for (int i = 0; i < 10; i++) {
    GSSK_Step(a, GSSK_GetDt(a));
    GSSK_Step(b, GSSK_GetDt(b));
  }
  close_to("implicit numerator gives k*A/W", GSSK_GetState(a)[ba], 25.0, 1e-9);
  ok("naming the origin explicitly is bit-identical",
     GSSK_GetState(a)[ba] == GSSK_GetState(b)[bb]);
  GSSK_Free(a); GSSK_Free(b);
}

/* GSSK_AddEdge has its own parser with no archetype dispatch, so it is a
 * separate code path from GSSK_Init and has to be checked separately — the
 * same reason test_node_type_validation.c covers both. A rejected add must
 * also be a true no-op: the instance a drag-and-drop editor is mutating has to
 * be left exactly as it was, and still steppable. */
static const char *ADD_EDGE_BASE =
  "{\"metadata\":{\"schema_version\":4},"
  "\"nodes\":[{\"id\":\"M\",\"type\":\"constant\",\"value\":100.0},"
  "          {\"id\":\"W\",\"type\":\"constant\",\"value\":4.0},"
  "          {\"id\":\"unity\",\"type\":\"source\",\"value\":1.0},"
  "          {\"id\":\"P\",\"type\":\"storage\",\"value\":0.0},"
  "          {\"id\":\"clearing\",\"type\":\"sink\",\"value\":0.0}],"
  "\"edges\":[{\"id\":\"p_relax\",\"origin\":\"P\",\"target\":\"clearing\","
  "  \"logic\":\"linear\",\"params\":{\"k\":0.8}}],"
  "\"config\":{\"t_start\":0,\"t_end\":50,\"dt\":0.01,\"method\":\"rk4\"}}";

static void test_add_edge_at_runtime(void) {
  printf("GSSK_AddEdge honours and validates numerator_node...\n");
  GSSK_Instance *inst = NULL;
  if (GSSK_Init(ADD_EDGE_BASE, &inst) != GSSK_SUCCESS) {
    printf("    init failed: %s\n", GSSK_GetErrorDescription(inst));
    GSSK_Free(inst); failures++; return;
  }
  size_t ec0 = GSSK_GetEdgeCount(inst);

  ok("numerator_node on non-ratio logic is rejected",
     GSSK_AddEdge(inst,
       "{\"id\":\"bad1\",\"origin\":\"unity\",\"target\":\"P\","
       " \"logic\":\"linear\",\"params\":{\"k\":1.0,\"numerator_node\":\"M\"}}")
     != GSSK_SUCCESS);
  ok("unknown numerator_node is rejected",
     GSSK_AddEdge(inst,
       "{\"id\":\"bad2\",\"origin\":\"unity\",\"target\":\"P\","
       " \"logic\":\"ratio\",\"params\":{\"k\":1.0,\"numerator_node\":\"nope\","
       " \"control_node\":\"W\"}}")
     != GSSK_SUCCESS);
  ok("a rejected add leaves the edge count unchanged",
     GSSK_GetEdgeCount(inst) == ec0);

  /* Completing the relaxation at runtime must give the same fixed point the
   * authored model reaches. */
  ok("a valid ratio edge with numerator_node is accepted",
     GSSK_AddEdge(inst,
       "{\"id\":\"p_target\",\"origin\":\"unity\",\"target\":\"P\","
       " \"logic\":\"ratio\",\"params\":{\"k\":0.8,\"numerator_node\":\"M\","
       " \"control_node\":\"W\"}}")
     == GSSK_SUCCESS);

  int pi = GSSK_FindNodeIdx(inst, "P");
  for (int i = 0; i < 5000; i++) GSSK_Step(inst, GSSK_GetDt(inst));
  close_to("the runtime-added relaxation settles at M/W",
           GSSK_GetState(inst)[pi], 25.0, 1e-9);
  GSSK_Free(inst);
}

int main(void) {
  printf("Phase C.3 — price dynamics: dP/dt = alpha (M/W - P)\n\n");
  test_fixed_point_is_the_ratio();       printf("\n");
  test_alpha_is_the_time_constant();     printf("\n");
  test_rk4_idc_agree();                  printf("\n");
  test_falling_work_raises_price();      printf("\n");
  test_numerator_is_not_consumed();      printf("\n");
  test_omitted_numerator_is_origin();    printf("\n");
  test_authoring_errors();               printf("\n");
  test_add_edge_at_runtime();            printf("\n");
  test_round_trip();
  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
