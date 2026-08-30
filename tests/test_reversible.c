/* The barb-less pathway: GSSK_LOGIC_REVERSIBLE (GIP-0001 G3, ADR 0007).
 *
 * Odum draws two pathway kinds and the distinction is in the notation: a barb
 * where the flow depends only on the force behind it, no barb where it depends
 * on the difference between the force at one end and the back force from the
 * other, "and this pathway may flow in either direction" (Modeling for All
 * Scales, p.23). Every other GSSK logic computes forward from origin
 * quantities and never reads the target, so the whole second class — diffusion,
 * exchange across a gradient, any equilibrating process — was inexpressible.
 *
 * F = k * (Q_origin - Q_target), signed.
 *
 * Four properties are worth testing and they are not the same property:
 *
 *   1. It EQUILIBRATES. Two stores converge to equal quantity from any initial
 *      condition, and neither goes negative on the way. The non-negativity
 *      clamp in GSSK_Step would hide an overshoot, so the negativity check is
 *      really a check that the clamp never had to do anything.
 *   2. It is SYMMETRIC. Swapping the declared origin and target gives an
 *      identical trajectory — because for a barb-less line those names are the
 *      two ends, not a from and a to. This is the assertion that distinguishes
 *      a real gradient pathway from a linear edge with clever bookkeeping.
 *   3. It CONSERVES. F is subtracted at one end and added at the other in the
 *      same statement, so the pair's total is invariant under any solver. Any
 *      sign or index error in build_flow_matrix's four entries breaks this
 *      immediately, which makes it the sharpest regression available.
 *   4. It actually RUNS BACKWARDS. Asserted directly, with a model whose
 *      declared target starts richer than its origin, because a build that
 *      clamped the flow at zero would pass 1-3 on a forward-only model.
 *
 * Plus the ABI: the existing GSSK_LogicType values must not have moved.
 *
 * Property 4 is asserted on the RATE via GSSK_GetFlows (GIP-0001 G4, merged
 * while this branch was open) as well as on the stores. A negative flow is
 * the thing no other logic in this kernel can produce, so it is worth naming
 * rather than inferring from which tank grew.
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

/* Two stores, one barb-less pathway, nothing else. `a` starts rich. */
static const char *FWD_TEMPLATE =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\", \"value\": %g },"
    "    { \"id\": \"b\", \"type\": \"storage\", \"value\": %g }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"gradient\", \"origin\": \"a\", \"target\": \"b\","
    "      \"logic\": \"reversible\", \"params\": { \"k\": %g } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 50, \"dt\": %g,"
    "                \"method\": \"%s\" }"
    "}";

/* Identical physics, origin and target swapped in the declaration. */
static const char *REV_TEMPLATE =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\", \"value\": %g },"
    "    { \"id\": \"b\", \"type\": \"storage\", \"value\": %g }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"gradient\", \"origin\": \"b\", \"target\": \"a\","
    "      \"logic\": \"reversible\", \"params\": { \"k\": %g } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 50, \"dt\": %g,"
    "                \"method\": \"%s\" }"
    "}";

static GSSK_Instance *build(const char *tmpl, double qa, double qb, double k,
                            double dt, const char *method) {
    char json[2048];
    snprintf(json, sizeof(json), tmpl, qa, qb, k, dt, method);
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("FATAL: model failed to load: %d (%s)\n", (int)st,
               inst ? GSSK_GetErrorDescription(inst) : "");
        GSSK_Free(inst);
        exit(1);
    }
    return inst;
}

/* 1. Converges to equal quantity from any initial condition, no negatives. */
static void test_equilibrates(void) {
    printf("Two stores on a reversible edge converge to equal quantity\n");

    /* Deliberately varied: a rich origin, a rich target, an already-equal
     * pair, and one where the total is odd so the midpoint is not a round
     * number. A build that merely drained the origin passes only the first. */
    const double cases[][2] = {
        { 100.0,   0.0 },
        {   0.0, 100.0 },
        {  50.0,  50.0 },
        {   7.0,  93.0 },
        {  13.0,  29.0 },
    };
    const int n_cases = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int c = 0; c < n_cases; c++) {
        double qa = cases[c][0], qb = cases[c][1];
        double total = qa + qb;
        GSSK_Instance *inst = build(FWD_TEMPLATE, qa, qb, 0.2, 0.1, "rk4");
        const double *Q = GSSK_GetState(inst);

        for (int i = 0; i < 500; i++) {
            CHECK(GSSK_Step(inst, 0.1) == GSSK_SUCCESS,
                  "case %d step %d failed", c, i);
            /* The clamp in GSSK_Step would silently absorb an overshoot, so
             * this is really asserting the clamp never had to fire. */
            CHECK(Q[0] >= 0.0 && Q[1] >= 0.0,
                  "case %d step %d went negative: a=%.9g b=%.9g", c, i, Q[0], Q[1]);
        }

        CHECK(fabs(Q[0] - Q[1]) < 1e-6,
              "case %d (%.1f, %.1f): converged to a=%.9g b=%.9g, difference %g",
              c, qa, qb, Q[0], Q[1], fabs(Q[0] - Q[1]));
        CHECK(fabs(Q[0] - total / 2.0) < 1e-6,
              "case %d: equilibrium is %.9g, expected the midpoint %.9g",
              c, Q[0], total / 2.0);

        GSSK_Free(inst);
    }
}

/* 2. Swapping the declared origin and target changes nothing.
 *
 * Compared step by step rather than only at equilibrium: every equilibrating
 * model converges to the same place eventually, so an endpoint-only check
 * would pass on a pathway that took a different route to get there. */
static void test_direction_is_symmetric(void) {
    printf("Swapping the declared origin and target is a no-op\n");

    GSSK_Instance *fwd = build(FWD_TEMPLATE, 100.0, 20.0, 0.2, 0.1, "rk4");
    GSSK_Instance *rev = build(REV_TEMPLATE, 100.0, 20.0, 0.2, 0.1, "rk4");

    const double *Qf = GSSK_GetState(fwd);
    const double *Qr = GSSK_GetState(rev);

    /* Node order in the array is unchanged — only the EDGE's endpoints were
     * swapped — so index 0 is `a` in both instances. */
    CHECK(strcmp(GSSK_GetNodeID(fwd, 0), "a") == 0 &&
          strcmp(GSSK_GetNodeID(rev, 0), "a") == 0,
          "node order differs between the two models; the comparison below "
          "would be meaningless");

    double max_diff = 0.0;
    int moved = 0;
    for (int i = 0; i < 200; i++) {
        GSSK_Step(fwd, 0.1);
        GSSK_Step(rev, 0.1);
        double d0 = fabs(Qf[0] - Qr[0]);
        double d1 = fabs(Qf[1] - Qr[1]);
        if (d0 > max_diff) max_diff = d0;
        if (d1 > max_diff) max_diff = d1;
        if (fabs(Qf[0] - 100.0) > 1.0) moved = 1;
    }

    CHECK(max_diff < 1e-12,
          "trajectories diverged by %g at worst — the declared direction is "
          "still load-bearing", max_diff);
    CHECK(moved, "neither trajectory moved; the symmetry check is vacuous");

    GSSK_Free(fwd);
    GSSK_Free(rev);
}

/* 3. Total quantity is conserved under euler AND rk4.
 *
 * Both solvers, because the two take different paths through the kernel and a
 * sign error in build_flow_matrix's four entries would show up in only one. */
static void test_conserves_under_both_solvers(void) {
    printf("Total quantity is conserved under euler and rk4\n");

    const char *methods[] = { "euler", "rk4" };
    for (int m = 0; m < 2; m++) {
        GSSK_Instance *inst = build(FWD_TEMPLATE, 80.0, 5.0, 0.15, 0.05, methods[m]);
        const double *Q = GSSK_GetState(inst);
        double total0 = Q[0] + Q[1];
        double worst = 0.0;

        for (int i = 0; i < 400; i++) {
            GSSK_Step(inst, 0.05);
            double drift = fabs((Q[0] + Q[1]) - total0);
            if (drift > worst) worst = drift;
        }

        CHECK(worst < 1e-9,
              "%s: total drifted by %g (started %.9g, ended %.9g)",
              methods[m], worst, total0, Q[0] + Q[1]);
        /* And the run must have actually done something. */
        CHECK(fabs(Q[0] - Q[1]) < 1.0,
              "%s: did not equilibrate (a=%.9g b=%.9g); conservation over a "
              "run that never moved proves nothing", methods[m], Q[0], Q[1]);

        GSSK_Free(inst);
    }
}

/* 4. It really transports backwards along the declared direction.
 *
 * The declared target starts richer than the declared origin, so a correct
 * build moves substance from `b` to `a` — up the arrow, as a barb-less line
 * is allowed to. A build that clamped the flow at zero, or that read only the
 * origin, leaves `a` where it started. */
static void test_flows_backwards(void) {
    printf("Flow runs backwards when the declared target is richer\n");

    GSSK_Instance *inst = build(FWD_TEMPLATE, 10.0, 90.0, 0.2, 0.1, "rk4");
    const double *Q = GSSK_GetState(inst);

    double a0 = Q[0], b0 = Q[1];
    GSSK_Step(inst, 0.1);

    /* GSSK_GetFlows (G4) landed while this branch was open, so the central
     * claim can be asserted on the RATE rather than inferred from the stores:
     * the flow is NEGATIVE, which is what "may flow in either direction"
     * means and what no other logic in the kernel can produce. */
    double f = GSSK_GetFlows(inst)[0];
    CHECK(f < 0.0,
          "flow reads %.15g; it must be NEGATIVE, because the declared target "
          "is richer than the declared origin", f);
    CHECK(fabs(f - 0.2 * (Q[0] - Q[1])) < 1e-9,
          "flow reads %.15g, expected k*(Q_origin - Q_target) = %.15g",
          f, 0.2 * (Q[0] - Q[1]));

    for (int i = 0; i < 99; i++) GSSK_Step(inst, 0.1);

    CHECK(Q[0] > a0 + 1.0,
          "the declared ORIGIN went from %.9g to %.9g — it should have GAINED, "
          "because its target was richer", a0, Q[0]);
    CHECK(Q[1] < b0 - 1.0,
          "the declared TARGET went from %.9g to %.9g — it should have LOST",
          b0, Q[1]);
    CHECK(fabs((Q[0] + Q[1]) - (a0 + b0)) < 1e-9,
          "the backward transport did not conserve: %.12g vs %.12g",
          Q[0] + Q[1], a0 + b0);

    GSSK_Free(inst);
}

/* ADR 0007 claims reversible is integrated EXACTLY by the incipient/IDC path
 * rather than linearised, unlike limit and ratio. If that is true, the
 * incipient solver and rk4 must agree far more closely on this model than a
 * linearised primitive would allow. */
static void test_incipient_agrees_with_rk4(void) {
    printf("The incipient (IDC) path integrates it exactly, matching rk4\n");

    GSSK_Instance *idc = build(FWD_TEMPLATE, 100.0, 20.0, 0.2, 0.1, "incipient");
    GSSK_Instance *rk4 = build(FWD_TEMPLATE, 100.0, 20.0, 0.2, 0.1, "rk4");

    const double *Qi = GSSK_GetState(idc);
    const double *Qr = GSSK_GetState(rk4);

    double worst = 0.0;
    for (int i = 0; i < 200; i++) {
        GSSK_Step(idc, 0.1);
        GSSK_Step(rk4, 0.1);
        double d = fabs(Qi[0] - Qr[0]);
        if (d > worst) worst = d;
    }

    /* Both are approximating the same exact exponential relaxation, so they
     * agree to RK4's own truncation error rather than to a linearisation
     * error. Loose enough not to be a solver-tuning tripwire, tight enough
     * that a matrix entry left out would blow straight through it. */
    CHECK(worst < 1e-6,
          "incipient and rk4 diverged by %g; ADR 0007's exactness claim is "
          "wrong, or build_flow_matrix is missing an entry", worst);
    /* Not an absolute floor: the gap decays as exp(-2k*t), so after t=20 at
     * k=0.2 it is 80*exp(-8) ~= 0.027 and still falling. What matters is that
     * it collapsed by orders of magnitude from its initial 80, which a run
     * that never moved cannot fake. */
    double gap = fabs(Qi[0] - Qi[1]);
    double predicted = 80.0 * exp(-2.0 * 0.2 * 20.0);
    CHECK(gap < 0.1 * 80.0,
          "the incipient run did not equilibrate: gap is still %.9g of an "
          "initial 80", gap);
    CHECK(fabs(gap - predicted) < 0.05 * predicted,
          "gap is %.9g but the analytic solution 80*exp(-2kt) gives %.9g — "
          "the relaxation RATE is wrong, not just the endpoint", gap, predicted);

    GSSK_Free(idc);
    GSSK_Free(rk4);
}

/* The ABI contract from the task: appending must not renumber anything. */
static void test_enum_values_unchanged(void) {
    printf("Existing GSSK_LogicType values are unchanged\n");
    CHECK((int)GSSK_LOGIC_CONSTANT    == 0, "GSSK_LOGIC_CONSTANT moved");
    CHECK((int)GSSK_LOGIC_LINEAR      == 1, "GSSK_LOGIC_LINEAR moved");
    CHECK((int)GSSK_LOGIC_INTERACTION == 2, "GSSK_LOGIC_INTERACTION moved");
    CHECK((int)GSSK_LOGIC_LIMIT       == 3, "GSSK_LOGIC_LIMIT moved");
    CHECK((int)GSSK_LOGIC_THRESHOLD   == 4, "GSSK_LOGIC_THRESHOLD moved");
    CHECK((int)GSSK_LOGIC_RATIO       == 5, "GSSK_LOGIC_RATIO moved");
    CHECK((int)GSSK_LOGIC_REVERSIBLE  == 6,
          "GSSK_LOGIC_REVERSIBLE must be APPENDED at 6, not inserted");
}

/* The logic string must round-trip, or a serialized model reloads as something
 * else — the exact failure `ratio` hit before it was added to logic_type_str. */
static void test_serialize_round_trip(void) {
    printf("A reversible edge round-trips through GSSK_SerializeModel\n");

    /* Serialized BEFORE stepping: GSSK_SerializeModel emits the model
     * definition with its initial values, not the current state — that is
     * GSSK_SerializeSnapshot's job. Stepping first and then comparing would
     * be testing the wrong function. */
    GSSK_Instance *inst = build(FWD_TEMPLATE, 60.0, 10.0, 0.2, 0.1, "rk4");

    char *json = NULL;
    GSSK_Status ser = GSSK_SerializeModel(inst, &json);
    CHECK(ser == GSSK_SUCCESS && json != NULL,
          "GSSK_SerializeModel failed: %d", (int)ser);
    if (!json) { GSSK_Free(inst); return; }

    CHECK(strstr(json, "\"reversible\"") != NULL,
          "serialized model does not name the logic 'reversible' — it would "
          "reload as something else");

    GSSK_Instance *reloaded = NULL;
    GSSK_Status st = GSSK_Init(json, &reloaded);
    CHECK(st == GSSK_SUCCESS, "reload failed: %d (%s)", (int)st,
          reloaded ? GSSK_GetErrorDescription(reloaded) : "");

    if (st == GSSK_SUCCESS) {
        /* Same physics after the round trip: step both and compare. */
        const double *Qo = GSSK_GetState(inst);
        const double *Qr = GSSK_GetState(reloaded);
        double worst = 0.0;
        for (int i = 0; i < 50; i++) {
            GSSK_Step(inst, 0.1);
            GSSK_Step(reloaded, 0.1);
            double d = fabs(Qo[0] - Qr[0]);
            if (d > worst) worst = d;
        }
        CHECK(worst < 1e-9,
              "the reloaded model diverged by %g — the logic did not survive "
              "serialisation", worst);
    }

    GSSK_FreeString(json);
    GSSK_Free(reloaded);
    GSSK_Free(inst);
}

int main(void) {
    printf("=== Reversible (barb-less) pathway tests (GIP-0001 G3) ===\n\n");

    test_enum_values_unchanged();
    test_equilibrates();
    test_direction_is_symmetric();
    test_conserves_under_both_solvers();
    test_flows_backwards();
    test_incipient_agrees_with_rk4();
    test_serialize_round_trip();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL REVERSIBLE PATHWAY TESTS PASSED ===\n");
    return 0;
}
