/* The rest of the work gate: the n-ary product and the subtracting action.
 * GIP-0001 G1, ADR 0008.
 *
 * Odum draws ONE interaction glyph and lets it compute several ways (Modeling
 * for All Scales, Fig. 2.6): (a) a product of two inputs, (c) a product of
 * THREE input forces, (d) a divisor action, (e) a subtracting action. GSSK
 * shipped (a) as `interaction` and (d) as `ratio`. This covers (c) and (e).
 *
 *   (c)  F = k * Q_origin * PRODUCT of every control     — params.control_nodes
 *   (e)  F = max(0, k * (Q_origin - Q_control))          — logic "subtract"
 *
 * What is worth asserting, and why each assertion is not the same as its
 * neighbour:
 *
 *   1. The three-input product is the ACTUAL product, checked against a
 *      hand-computed closed form rather than against the kernel's own output.
 *      A build that multiplied only the first control still decays, just at
 *      the wrong rate, and a golden CSV regenerated from that build agrees
 *      with itself.
 *   2. n-ary EQUALS binary when the controls collapse. Two controls of 2.0
 *      and 4.0 must give a bit-identical trajectory to one control of 8.0 —
 *      all three values are exact in binary, so "close enough" is not the
 *      standard here.
 *   3. The one-control form is UNCHANGED, spelled either way. `control_node`
 *      and a single-entry `control_nodes` are the same edge.
 *   4. `subtract` computes k*(Q_origin - Q_control) and NEVER goes negative.
 *      Asserted on the rate through GSSK_GetFlows, and separately on a model
 *      whose control starts above its origin so the clamp is the only thing
 *      under test.
 *   5. The arity rules are ENFORCED, with GSSK_ERR_SCHEMA_VIOLATION — a
 *      quotient or a difference of three things is not defined by the figure.
 *   6. Both forms round-trip through GSSK_SerializeModel, in the spelling
 *      they were written in.
 *   7. The ABI: appending `subtract` must not renumber anything.
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

/* Loads a model that is expected to load. */
static GSSK_Instance *must_load(const char *json) {
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

/* Asserts a model is REJECTED, and rejected for the documented reason. */
static void must_reject(const char *what, const char *json,
                        const char *expect_substr) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st == GSSK_SUCCESS) {
        printf("  FAIL: %s loaded; it must be rejected\n", what);
        failures++;
        GSSK_Free(inst);
        return;
    }
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s was rejected with status %d, expected GSSK_ERR_SCHEMA_VIOLATION (%d)",
          what, (int)st, (int)GSSK_ERR_SCHEMA_VIOLATION);
    const char *msg = inst ? GSSK_GetErrorDescription(inst) : "";
    CHECK(msg && strstr(msg, expect_substr) != NULL,
          "%s was rejected with \"%s\", which does not mention \"%s\" — a "
          "model author cannot act on it", what, msg ? msg : "(none)",
          expect_substr);
    GSSK_Free(inst);
}

/* ── (c) the n-ary product ────────────────────────────────────────────────
 *
 * One store draining through a work gate whose controls are CONSTANT nodes,
 * so the controls hold their values and the whole thing has a closed form:
 *
 *   dQ/dt = -k * Q * c1 * c2   =>   Q(t) = Q0 * exp(-k*c1*c2*t)
 *
 * The sink absorbs, so nothing feeds back. `%s` is the controls clause, which
 * is what varies between the cases below.
 */
static const char *NARY_TEMPLATE =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"q\",  \"type\": \"storage\",  \"value\": 100.0 },"
    "    { \"id\": \"c1\", \"type\": \"constant\", \"value\": 2.0 },"
    "    { \"id\": \"c2\", \"type\": \"constant\", \"value\": 4.0 },"
    "    { \"id\": \"c12\",\"type\": \"constant\", \"value\": 8.0 },"
    "    { \"id\": \"out\",\"type\": \"sink\",     \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"gate\", \"origin\": \"q\", \"target\": \"out\","
    "      \"logic\": \"interaction\","
    "      \"params\": { \"k\": 0.01, %s } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 20, \"dt\": 0.01,"
    "                \"method\": \"%s\" }"
    "}";

static GSSK_Instance *build_nary(const char *controls, const char *method) {
    char json[2048];
    snprintf(json, sizeof(json), NARY_TEMPLATE, controls, method);
    return must_load(json);
}

/* 1. The three-input gate computes k*Q*c1*c2, against the closed form. */
static void test_three_input_product(void) {
    printf("A three-input interaction is k*Q_origin*Q_c1*Q_c2\n");

    const double k = 0.01, c1 = 2.0, c2 = 4.0, Q0 = 100.0;
    const double lambda = k * c1 * c2;          /* 0.08 */
    const double dt = 0.01;

    GSSK_Instance *inst = build_nary("\"control_nodes\": [\"c1\", \"c2\"]", "rk4");
    const double *Q = GSSK_GetState(inst);

    /* The rate on the very first step, before anything has moved, is the one
     * value that pins the FORMULA rather than the trajectory: a build using
     * only c1 reads 2.0 here instead of 8.0. */
    GSSK_Step(inst, dt);
    double f = GSSK_GetFlows(inst)[0];
    CHECK(fabs(f - k * Q[0] * c1 * c2) < 1e-12,
          "flow reads %.15g, expected k*Q*c1*c2 = %.15g (a build multiplying "
          "only the first control would read %.15g)",
          f, k * Q[0] * c1 * c2, k * Q[0] * c1);

    double worst = 0.0;
    for (int i = 1; i <= 2000; i++) {
        CHECK(GSSK_Step(inst, dt) == GSSK_SUCCESS, "step %d failed", i);
        double t = (i + 1) * dt;
        double exact = Q0 * exp(-lambda * t);
        double err = fabs(Q[0] - exact);
        if (err > worst) worst = err;
    }

    /* RK4 on a scalar exponential at dt=0.01 is far tighter than this; the
     * bound is loose enough not to be a solver tripwire and tight enough that
     * a dropped control (lambda 0.02 instead of 0.08) misses by ~60. */
    CHECK(worst < 1e-6,
          "worst deviation from Q0*exp(-k*c1*c2*t) was %g", worst);
    CHECK(Q[0] < 25.0 && Q[0] > 15.0,
          "after t=20 the store reads %.9g; the closed form gives %.9g",
          Q[0], Q0 * exp(-lambda * 20.0));

    GSSK_Free(inst);
}

/* 2. Two controls of 2 and 4 == one control of 8, bit for bit.
 *
 * All three values are exactly representable, so the product loop produces
 * the same double the single control holds and there is no rounding to
 * excuse a difference. Under every solver, because IDC and RK4 read the
 * controls through different code paths (build_flow_matrix vs
 * compute_derivatives) and a build could get one right and the other wrong. */
static void test_nary_equals_collapsed_binary(void) {
    printf("Two controls of 2 and 4 are the same edge as one control of 8\n");

    const char *methods[] = { "rk4", "euler", "incipient", "auto" };
    for (int m = 0; m < 4; m++) {
        GSSK_Instance *nary = build_nary("\"control_nodes\": [\"c1\", \"c2\"]",
                                         methods[m]);
        GSSK_Instance *flat = build_nary("\"control_node\": \"c12\"", methods[m]);
        const double *Qn = GSSK_GetState(nary);
        const double *Qf = GSSK_GetState(flat);

        double worst = 0.0;
        for (int i = 0; i < 500; i++) {
            GSSK_Step(nary, 0.01);
            GSSK_Step(flat, 0.01);
            double d = fabs(Qn[0] - Qf[0]);
            if (d > worst) worst = d;
        }
        CHECK(worst == 0.0,
              "%s: the n-ary and collapsed forms differ by %g; with exact "
              "binary values they must be identical", methods[m], worst);
        CHECK(fabs(Qn[0] - 100.0) > 1.0,
              "%s: neither model moved, so the comparison proves nothing",
              methods[m]);

        GSSK_Free(nary);
        GSSK_Free(flat);
    }
}

/* 3. The one-control form is untouched, in either spelling. */
static void test_single_control_unchanged(void) {
    printf("A single control is the same edge spelled either way\n");

    GSSK_Instance *sing = build_nary("\"control_node\": \"c12\"", "rk4");
    GSSK_Instance *arr  = build_nary("\"control_nodes\": [\"c12\"]", "rk4");
    const double *Qs = GSSK_GetState(sing);
    const double *Qa = GSSK_GetState(arr);

    double worst = 0.0;
    for (int i = 0; i < 500; i++) {
        GSSK_Step(sing, 0.01);
        GSSK_Step(arr, 0.01);
        double d = fabs(Qs[0] - Qa[0]);
        if (d > worst) worst = d;
    }
    CHECK(worst == 0.0,
          "control_node and a one-entry control_nodes differ by %g", worst);

    GSSK_Free(sing);
    GSSK_Free(arr);
}

/* ── (e) the subtracting action ───────────────────────────────────────────
 *
 *   dQa/dt = -k*(Qa - d)   while Qa > d   =>   Qa(t) = d + (Qa0 - d)*exp(-k*t)
 *
 * `d` is a constant node, so it holds. The store relaxes ONTO the control and
 * stops there: it must not undershoot, and the flow must not go negative.
 */
static const char *SUB_TEMPLATE =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\",  \"value\": %g },"
    "    { \"id\": \"d\", \"type\": \"constant\", \"value\": %g },"
    "    { \"id\": \"b\", \"type\": \"storage\",  \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"gate\", \"origin\": \"a\", \"target\": \"b\","
    "      \"logic\": \"subtract\","
    "      \"params\": { \"k\": %g, \"control_node\": \"d\" } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 200, \"dt\": %g,"
    "                \"method\": \"%s\" }"
    "}";

static GSSK_Instance *build_sub(double qa, double d, double k,
                                double dt, const char *method) {
    char json[2048];
    snprintf(json, sizeof(json), SUB_TEMPLATE, qa, d, k, dt, method);
    return must_load(json);
}

/* 4a. The formula, against the closed form. */
static void test_subtract_formula(void) {
    printf("A subtracting interaction is k*(Q_origin - Q_control)\n");

    const double qa0 = 100.0, d = 30.0, k = 0.2, dt = 0.01;
    GSSK_Instance *inst = build_sub(qa0, d, k, dt, "rk4");
    const double *Q = GSSK_GetState(inst);

    GSSK_Step(inst, dt);
    double f = GSSK_GetFlows(inst)[0];
    CHECK(fabs(f - k * (Q[0] - d)) < 1e-12,
          "flow reads %.15g, expected k*(Q_origin - Q_control) = %.15g",
          f, k * (Q[0] - d));
    /* The control is READ, not consumed — the same contract interaction's
     * control has. A build that debited it would have moved it off 30. */
    CHECK(Q[1] == d, "the control node moved to %.15g; it must be read, not "
                     "consumed", Q[1]);

    /* Long enough that (qa0-d)*exp(-k*t) is below the tolerance the
     * settling check uses: at k=0.2, t=100 leaves 70*exp(-20) ~= 1.4e-7. */
    double worst = 0.0;
    for (int i = 1; i <= 10000; i++) {
        GSSK_Step(inst, dt);
        double t = (i + 1) * dt;
        double exact = d + (qa0 - d) * exp(-k * t);
        double err = fabs(Q[0] - exact);
        if (err > worst) worst = err;
    }
    CHECK(worst < 1e-6,
          "worst deviation from d + (Qa0-d)*exp(-k*t) was %g", worst);

    /* It relaxes ONTO the control and stops, rather than through it. */
    CHECK(fabs(Q[0] - d) < 1e-6,
          "settled at %.9g; the subtracting gate stops at the control (%.9g)",
          Q[0], d);
    CHECK(Q[0] >= d - 1e-9,
          "undershot the control: %.15g < %.15g", Q[0], d);

    GSSK_Free(inst);
}

/* 4b. It never yields a negative flow — the clamp, on its own.
 *
 * The origin starts BELOW the control, which is the only configuration where
 * an unclamped k*(Q_origin - Q_control) is negative. Under an unclamped build
 * `b` would drain into `a` along a barbed pathway, which is what the barb
 * asserts cannot happen. Separate from 4a because a model that only ever
 * relaxes downward passes 4a whether the clamp exists or not. */
static void test_subtract_never_negative(void) {
    printf("A subtracting interaction never yields a negative flow\n");

    const char *methods[] = { "rk4", "euler", "incipient", "auto" };
    for (int m = 0; m < 4; m++) {
        /* Origin 10, control 30: the difference is -20 throughout. */
        GSSK_Instance *inst = build_sub(10.0, 30.0, 0.2, 0.05, methods[m]);
        const double *Q = GSSK_GetState(inst);
        double a0 = Q[0], b0 = Q[2];

        for (int i = 0; i < 400; i++) {
            GSSK_Step(inst, 0.05);
            double f = GSSK_GetFlows(inst)[0];
            CHECK(f >= 0.0,
                  "%s step %d: flow is %.15g — a barbed pathway must not carry "
                  "a negative flow", methods[m], i, f);
            CHECK(f == 0.0,
                  "%s step %d: flow is %.15g with the control above the origin; "
                  "the gate must be shut", methods[m], i, f);
        }
        CHECK(Q[0] == a0 && Q[2] == b0,
              "%s: the shut gate still moved substance (a %.15g->%.15g, "
              "b %.15g->%.15g)", methods[m], a0, Q[0], b0, Q[2]);

        GSSK_Free(inst);
    }
}

/* 4c. And it stops at the control rather than reversing once it arrives.
 *
 * A store that starts well above its control, run long past the point where
 * it reaches it. An unclamped build overshoots and then runs backwards; this
 * asserts the trajectory is monotone and lands on the control. */
static void test_subtract_saturates_rather_than_reverses(void) {
    printf("A subtracting interaction saturates at zero rather than reversing\n");

    GSSK_Instance *inst = build_sub(80.0, 25.0, 0.5, 0.05, "rk4");
    const double *Q = GSSK_GetState(inst);
    double prev = Q[0];

    for (int i = 0; i < 2000; i++) {
        GSSK_Step(inst, 0.05);
        CHECK(Q[0] <= prev + 1e-12,
              "step %d: the origin ROSE from %.15g to %.15g — the gate ran "
              "backwards", i, prev, Q[0]);
        prev = Q[0];
    }
    CHECK(fabs(Q[0] - 25.0) < 1e-6,
          "settled at %.9g rather than on the control at 25", Q[0]);
    /* Conservation: what left `a` arrived at `b`. */
    CHECK(fabs((Q[0] + Q[2]) - 80.0) < 1e-9,
          "a+b is %.12g, started at 80", Q[0] + Q[2]);

    GSSK_Free(inst);
}

/* 5. The arity rules. */
static void test_arity_rules(void) {
    printf("Only `interaction` takes more than one control node\n");

    /* Three storages so every logic below has real nodes to name. */
    #define ARITY_HEAD                                                     \
        "{ \"metadata\": { \"schema_version\": 4 },"                       \
        "  \"nodes\": ["                                                   \
        "    { \"id\": \"a\", \"type\": \"storage\",  \"value\": 50.0 },"  \
        "    { \"id\": \"x\", \"type\": \"constant\", \"value\": 2.0 },"   \
        "    { \"id\": \"y\", \"type\": \"constant\", \"value\": 3.0 },"   \
        "    { \"id\": \"b\", \"type\": \"sink\",     \"value\": 0.0 }"    \
        "  ],"                                                             \
        "  \"edges\": [ { \"id\": \"e\", \"origin\": \"a\", "              \
        "                 \"target\": \"b\", \"logic\": "
    #define ARITY_TAIL "} ], \"config\": { \"t_end\": 1, \"dt\": 0.1 } }"

    must_reject("a `subtract` edge with two control nodes",
        ARITY_HEAD "\"subtract\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\", \"y\"] } " ARITY_TAIL,
        "only 'interaction' takes more than one");

    must_reject("a `ratio` edge with two control nodes",
        ARITY_HEAD "\"ratio\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\", \"y\"] } " ARITY_TAIL,
        "only 'interaction' takes more than one");

    must_reject("a `limit` edge with two control nodes",
        ARITY_HEAD "\"limit\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\", \"y\"] } " ARITY_TAIL,
        "only 'interaction' takes more than one");

    /* Both spellings on one edge: rejected, rather than one quietly winning. */
    must_reject("an edge naming both control_node and control_nodes",
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_node\": \"x\", \"control_nodes\": [\"x\", \"y\"] } " ARITY_TAIL,
        "both control_node and control_nodes");

    must_reject("a `subtract` edge with no control node at all",
        ARITY_HEAD "\"subtract\", \"params\": { \"k\": 0.1 } " ARITY_TAIL,
        "requires control_node");

    must_reject("an empty control_nodes array",
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [] } " ARITY_TAIL,
        "empty control_nodes");

    must_reject("a control_nodes naming a node that does not exist",
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\", \"nope\"] } " ARITY_TAIL,
        "unknown control_nodes[1]");

    must_reject("a control_nodes entry that is not a string",
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\", 7] } " ARITY_TAIL,
        "control_nodes[1] is not a node id string");

    must_reject("more control nodes than GSSK_MAX_CONTROL_NODES",
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\",\"y\",\"x\",\"y\",\"x\",\"y\",\"x\",\"y\",\"x\"] } "
        ARITY_TAIL,
        "the limit is 8");

    /* The one that must LOAD: eight is the cap, not the first rejection. */
    GSSK_Instance *ok = NULL;
    GSSK_Status st = GSSK_Init(
        ARITY_HEAD "\"interaction\", \"params\": { \"k\": 0.1, "
        "\"control_nodes\": [\"x\",\"y\",\"x\",\"y\",\"x\",\"y\",\"x\",\"y\"] } "
        ARITY_TAIL, &ok);
    CHECK(st == GSSK_SUCCESS,
          "exactly GSSK_MAX_CONTROL_NODES controls was rejected (%d: %s); the "
          "cap is inclusive", (int)st,
          ok ? GSSK_GetErrorDescription(ok) : "");
    GSSK_Free(ok);

    #undef ARITY_HEAD
    #undef ARITY_TAIL
}

/* 6. Both forms survive serialisation, in the spelling they were written in. */
static void test_round_trip(void) {
    printf("Both new forms round-trip through GSSK_SerializeModel\n");

    /* n-ary: emitted as control_nodes, and reloads to the same trajectory. */
    {
        GSSK_Instance *inst = build_nary("\"control_nodes\": [\"c1\", \"c2\"]",
                                         "rk4");
        char *json = NULL;
        CHECK(GSSK_SerializeModel(inst, &json) == GSSK_SUCCESS && json,
              "GSSK_SerializeModel failed on an n-ary interaction");
        if (json) {
            CHECK(strstr(json, "\"control_nodes\"") != NULL,
                  "the serialised n-ary edge has no control_nodes; it would "
                  "reload as a different model");
            GSSK_Instance *back = must_load(json);
            const double *Qa = GSSK_GetState(inst);
            const double *Qb = GSSK_GetState(back);
            double worst = 0.0;
            for (int i = 0; i < 300; i++) {
                GSSK_Step(inst, 0.01);
                GSSK_Step(back, 0.01);
                double d = fabs(Qa[0] - Qb[0]);
                if (d > worst) worst = d;
            }
            CHECK(worst == 0.0, "the reloaded n-ary model differs by %g", worst);
            GSSK_Free(back);
            GSSK_FreeString(json);
        }
        GSSK_Free(inst);
    }

    /* A one-control edge must still emit the SINGULAR field, so an existing
     * model does not come back rewritten. */
    {
        GSSK_Instance *inst = build_nary("\"control_node\": \"c12\"", "rk4");
        char *json = NULL;
        CHECK(GSSK_SerializeModel(inst, &json) == GSSK_SUCCESS && json,
              "GSSK_SerializeModel failed on a one-control interaction");
        if (json) {
            CHECK(strstr(json, "\"control_node\"") != NULL,
                  "a one-control edge lost its control_node on serialisation");
            CHECK(strstr(json, "\"control_nodes\"") == NULL,
                  "a one-control edge was rewritten into the array form");
            GSSK_FreeString(json);
        }
        GSSK_Free(inst);
    }

    /* subtract: the logic string must survive, or the model reloads as
     * something else entirely. */
    {
        GSSK_Instance *inst = build_sub(100.0, 30.0, 0.2, 0.05, "rk4");
        char *json = NULL;
        CHECK(GSSK_SerializeModel(inst, &json) == GSSK_SUCCESS && json,
              "GSSK_SerializeModel failed on a subtract edge");
        if (json) {
            CHECK(strstr(json, "\"subtract\"") != NULL,
                  "the serialised model does not say `subtract`");
            GSSK_Instance *back = must_load(json);
            const double *Qa = GSSK_GetState(inst);
            const double *Qb = GSSK_GetState(back);
            double worst = 0.0;
            for (int i = 0; i < 300; i++) {
                GSSK_Step(inst, 0.05);
                GSSK_Step(back, 0.05);
                double d = fabs(Qa[0] - Qb[0]);
                if (d > worst) worst = d;
            }
            CHECK(worst == 0.0, "the reloaded subtract model differs by %g", worst);
            GSSK_Free(back);
            GSSK_FreeString(json);
        }
        GSSK_Free(inst);
    }
}

/* 7. The ABI contract: appending must not renumber anything. */
static void test_enum_values_unchanged(void) {
    printf("Existing GSSK_LogicType values are unchanged\n");
    CHECK((int)GSSK_LOGIC_CONSTANT    == 0, "GSSK_LOGIC_CONSTANT moved");
    CHECK((int)GSSK_LOGIC_LINEAR      == 1, "GSSK_LOGIC_LINEAR moved");
    CHECK((int)GSSK_LOGIC_INTERACTION == 2, "GSSK_LOGIC_INTERACTION moved");
    CHECK((int)GSSK_LOGIC_LIMIT       == 3, "GSSK_LOGIC_LIMIT moved");
    CHECK((int)GSSK_LOGIC_THRESHOLD   == 4, "GSSK_LOGIC_THRESHOLD moved");
    CHECK((int)GSSK_LOGIC_RATIO       == 5, "GSSK_LOGIC_RATIO moved");
    CHECK((int)GSSK_LOGIC_REVERSIBLE  == 6, "GSSK_LOGIC_REVERSIBLE moved");
    CHECK((int)GSSK_LOGIC_SUBTRACT    == 7,
          "GSSK_LOGIC_SUBTRACT must be APPENDED at 7, not inserted");
}

int main(void) {
    printf("\n=== n-ary interaction and subtracting action (GIP-0001 G1) ===\n\n");

    test_enum_values_unchanged();
    test_three_input_product();
    test_nary_equals_collapsed_binary();
    test_single_control_unchanged();
    test_subtract_formula();
    test_subtract_never_negative();
    test_subtract_saturates_rather_than_reverses();
    test_arity_rules();
    test_round_trip();

    printf("\n");
    if (failures) {
        printf("=== %d CHECK(S) FAILED ===\n\n", failures);
        return 1;
    }
    printf("=== ALL N-ARY / SUBTRACT TESTS PASSED ===\n\n");
    return 0;
}
