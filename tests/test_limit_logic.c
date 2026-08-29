/* Where limit logic gets its saturation constant C (GIP-0001 G6).
 *
 * F = k x Q_origin / (1 + Q_origin/C) has been implemented since the primitive
 * was added and the formula is stated in include/gssk.h, but nothing told a
 * consumer how to SUPPLY C: gssk.schema.json documented control_node as a
 * thing that "modulates the flow" without saying it is the denominator
 * constant, and documented threshold for threshold and ratio logic only,
 * never mentioning that it doubles as C when no control node exists.
 *
 * Documenting it is most of the task. These tests are what stop the
 * documentation being wrong later, and they pin the four facts a reader of the
 * schema now relies on:
 *
 *   1. params.threshold supplies C when there is no control_node.
 *   2. control_node WINS when both are given — threshold is the source used in
 *      its absence, not a runtime fallback.
 *   3. Neither is a load error, not a silent zero.
 *   4. A control-supplied C that decays past GSSK_LIMIT_C_EPSILON closes the
 *      pathway to 0.0 mid-run, without an error, in a model that loaded fine.
 *
 * Fact 4 is the one worth having a test for. It is not a defect — a
 * saturation constant of zero means the pathway saturates at zero throughput —
 * but it is invisible in the model file, and it is the difference between a
 * limit edge that has stopped and a limit edge that was never flowing.
 *
 * Flow is asserted DIRECTLY through GSSK_GetFlows (GIP-0001 G4), which landed
 * while this branch was open. The state-based checks are kept alongside it
 * rather than replaced: the rate and its effect on the store are two separate
 * claims, and a cache reporting a plausible rate that the integrator never
 * applied would satisfy either one alone.
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

/* C = 10 from params.threshold, no control_node anywhere. */
static const char *C_FROM_THRESHOLD =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\", \"value\": 100.0 },"
    "    { \"id\": \"b\", \"type\": \"storage\", \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"a\", \"target\": \"b\", \"logic\": \"limit\","
    "      \"params\": { \"k\": 0.5, \"threshold\": 10.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

/* Neither source of C. */
static const char *NO_C_AT_ALL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\", \"value\": 100.0 },"
    "    { \"id\": \"b\", \"type\": \"storage\", \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"a\", \"target\": \"b\", \"logic\": \"limit\","
    "      \"params\": { \"k\": 0.5 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

/* Both given, and chosen so the two answers are far apart: a constant control
 * of 2.0 saturates hard, a threshold of 1000.0 barely saturates at all. */
static const char *BOTH_SOURCES =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\",  \"value\": 100.0 },"
    "    { \"id\": \"b\", \"type\": \"storage\",  \"value\": 0.0 },"
    "    { \"id\": \"c\", \"type\": \"constant\", \"value\": 2.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"a\", \"target\": \"b\", \"logic\": \"limit\","
    "      \"params\": { \"k\": 0.5, \"control_node\": \"c\","
    "                    \"threshold\": 1000.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

/* The control is itself a store, draining fast into a sink. C therefore moves
 * during the run and eventually crosses GSSK_LIMIT_C_EPSILON. */
static const char *DECAYING_CONTROL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"a\", \"type\": \"storage\", \"value\": 100.0 },"
    "    { \"id\": \"b\", \"type\": \"storage\", \"value\": 0.0 },"
    "    { \"id\": \"c\", \"type\": \"storage\", \"value\": 5.0 },"
    "    { \"id\": \"d\", \"type\": \"storage\", \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"a\", \"target\": \"b\", \"logic\": \"limit\","
    "      \"params\": { \"k\": 0.5, \"control_node\": \"c\" } },"
    "    { \"origin\": \"c\", \"target\": \"d\", \"logic\": \"linear\","
    "      \"params\": { \"k\": 5.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 40, \"dt\": 0.1,"
    "                \"method\": \"rk4\" }"
    "}";

static GSSK_Instance *load(const char *json, const char *what) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("FATAL: %s failed to load: %d (%s)\n", what, (int)st,
               inst ? GSSK_GetErrorDescription(inst) : "");
        GSSK_Free(inst);
        exit(1);
    }
    return inst;
}

/* 1. params.threshold supplies C when there is no control_node.
 *
 * Checked against the closed form rather than "something moved": with C
 * genuinely 10 and Q_origin near 100, the instantaneous rate is
 * 0.5*100/(1+100/10) = 4.545..., an order of magnitude below the 50 that
 * linear logic would give. A build that ignored threshold would compute C=-1,
 * take the `C > eps` branch as false, and hold Q at 100 forever. */
static void test_c_from_threshold(void) {
    printf("C comes from params.threshold when no control_node is named\n");
    GSSK_Instance *inst = load(C_FROM_THRESHOLD, "C_FROM_THRESHOLD");
    const double *Q = GSSK_GetState(inst);

    double before = Q[0];
    GSSK_Step(inst, 0.1);
    double moved = before - Q[0];

    CHECK(moved > 1e-9, "Q_origin did not move — C was not taken from threshold");

    /* Now that per-edge flow is readable, name the rate rather than inferring
     * it. F = k*Q/(1 + Q/C) at the post-step state, with C = 10 from
     * params.threshold — a build that ignored threshold computes C = -1, takes
     * the `C > eps` branch as false, and reports exactly 0.0 here. */
    double q_now = GSSK_GetState(inst)[0];
    double rate_expected = 0.5 * q_now / (1.0 + q_now / 10.0);
    CHECK(fabs(GSSK_GetFlows(inst)[0] - rate_expected) < 1e-9,
          "flow reads %.15g, expected k*Q/(1+Q/C) = %.15g for C=10",
          GSSK_GetFlows(inst)[0], rate_expected);

    /* One RK4 step of length dt starting at F0 = k*Q/(1+Q/C). The step is not
     * exactly F0*dt because the rate falls as Q does, so compare to a few
     * percent rather than to TOL, and against the two rival hypotheses. */
    double expected = 0.5 * 100.0 / (1.0 + 100.0 / 10.0) * 0.1;
    CHECK(fabs(moved - expected) < 0.02 * expected,
          "one step moved %.9g, expected ~%.9g for C=10", moved, expected);

    /* Not the un-saturated rate: that would be 0.5*100*0.1 = 5.0. */
    CHECK(moved < 1.0, "flow %.9g looks unsaturated — C was ignored", moved);

    /* Saturation is the point: total throughput over the run must fall well
     * short of what the same k would move linearly. */
    for (int i = 0; i < 99; i++) GSSK_Step(inst, 0.1);
    CHECK(Q[0] > 50.0,
          "Q_origin fell to %.6g over 10 units — that is not saturating", Q[0]);
    CHECK(Q[0] < 99.0, "Q_origin barely moved (%.6g) — nothing is flowing", Q[0]);

    GSSK_Free(inst);
}

/* 2. control_node wins over threshold when both are present. */
static void test_control_node_wins(void) {
    printf("control_node takes precedence over params.threshold\n");
    GSSK_Instance *inst = load(BOTH_SOURCES, "BOTH_SOURCES");
    const double *Q = GSSK_GetState(inst);

    double before = Q[0];
    GSSK_Step(inst, 0.1);
    double moved = before - Q[0];

    /* C = 2 (the control), not 1000 (the threshold). */
    double if_control   = 0.5 * 100.0 / (1.0 + 100.0 /    2.0) * 0.1;
    double if_threshold = 0.5 * 100.0 / (1.0 + 100.0 / 1000.0) * 0.1;

    CHECK(fabs(moved - if_control) < 0.05 * if_control,
          "one step moved %.9g; C=control gives ~%.9g, C=threshold gives ~%.9g",
          moved, if_control, if_threshold);
    /* The two hypotheses are ~46x apart, so this cannot pass by coincidence. */
    CHECK(if_threshold > 10.0 * if_control,
          "the test model does not separate the two hypotheses");

    GSSK_Free(inst);
}

/* 3. Neither source is a load-time rejection, not a silent zero.
 *
 * The GIP filed this as "flow is silently 0.0 rather than an error". That is
 * only half true and the half matters: the ABSENT-C case is caught at load. */
static void test_no_c_is_rejected(void) {
    printf("A limit edge with neither control_node nor threshold is rejected\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(NO_C_AT_ALL, &inst);

    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "GSSK_Init returned %d, expected GSSK_ERR_SCHEMA_VIOLATION (%d)",
          (int)st, (int)GSSK_ERR_SCHEMA_VIOLATION);
    if (inst) {
        const char *msg = GSSK_GetErrorDescription(inst);
        CHECK(strstr(msg, "control_node") != NULL && strstr(msg, "threshold") != NULL,
              "error message names neither source of C: '%s'", msg);
    }
    GSSK_Free(inst);
}

/* 4. A control-supplied C is a state variable, and crossing
 *    GSSK_LIMIT_C_EPSILON closes the pathway to 0.0 without an error.
 *
 * This is the fact the schema now warns about, and the reason it is worth
 * warning about: the model loads, runs to completion, returns GSSK_SUCCESS
 * every step, and the limit edge simply stops. */
static void test_decaying_control_closes_the_path(void) {
    printf("A control decaying past the epsilon silently closes the pathway\n");
    GSSK_Instance *inst = load(DECAYING_CONTROL, "DECAYING_CONTROL");
    const double *Q = GSSK_GetState(inst);

    /* Early on the control is healthy and the edge flows. */
    double a_start = Q[0];
    for (int i = 0; i < 10; i++)
        CHECK(GSSK_Step(inst, 0.1) == GSSK_SUCCESS, "early step failed");
    double a_early = Q[0];
    CHECK(a_start - a_early > 1e-6,
          "the limit edge never flowed at all (%.9g moved) — the model is wrong",
          a_start - a_early);
    CHECK(Q[2] > 1e-9, "the control had already collapsed by step 10");

    /* Run until the control is well below the epsilon. */
    for (int i = 0; i < 390; i++)
        CHECK(GSSK_Step(inst, 0.1) == GSSK_SUCCESS,
              "step %d failed — this is supposed to be silent, not an error", i);

    CHECK(Q[2] < 1e-9,
          "the control is %.6g, still above the epsilon — the run is too short",
          Q[2]);

    /* With C below the epsilon the flow is exactly 0.0. Asserted on the rate
     * itself — this is the sentence the schema and the header now make, and
     * until G4 landed it could only be inferred from a frozen store. */
    CHECK(GSSK_GetFlows(inst)[0] == 0.0,
          "the limit edge reports flow %.20g after the control collapsed; the "
          "documented behaviour is exactly 0.0", GSSK_GetFlows(inst)[0]);
    /* And the store agrees: a rate of zero the integrator did not apply would
     * satisfy the assertion above on its own. */
    double a_before = Q[0];
    for (int i = 0; i < 20; i++) GSSK_Step(inst, 0.1);
    CHECK(fabs(Q[0] - a_before) == 0.0,
          "Q_origin moved by %.20g after the control collapsed; the flow is "
          "supposed to be exactly 0.0", fabs(Q[0] - a_before));

    /* And the origin still holds most of its contents — the pathway closed,
     * it did not drain. Without this, a fully-drained store would also sit
     * still and pass the assertion above. */
    CHECK(Q[0] > 50.0,
          "Q_origin is %.6g; it drained rather than stopping, so the "
          "frozen-state check above proves nothing", Q[0]);

    GSSK_Free(inst);
}

/* 5. Threshold logic compares Q_origin, strictly, and ignores control_node.
 *
 * Included because the schema now says so and nothing else asserts it. Two
 * edges from the same origin: one whose threshold sits just below Q_origin,
 * one just above. If the comparand were the control node both would agree. */
static void test_threshold_comparand(void) {
    printf("Threshold logic compares Q_origin strictly, ignoring control_node\n");
    GSSK_Instance *inst = load(
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"a\",   \"type\": \"storage\",  \"value\": 10.0 },"
        "    { \"id\": \"lo\",  \"type\": \"storage\",  \"value\": 0.0 },"
        "    { \"id\": \"hi\",  \"type\": \"storage\",  \"value\": 0.0 },"
        "    { \"id\": \"ctl\", \"type\": \"constant\", \"value\": 500.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"origin\": \"a\", \"target\": \"lo\", \"logic\": \"threshold\","
        "      \"params\": { \"k\": 0.1, \"threshold\": 5.0,"
        "                    \"control_node\": \"ctl\" } },"
        "    { \"origin\": \"a\", \"target\": \"hi\", \"logic\": \"threshold\","
        "      \"params\": { \"k\": 0.1, \"threshold\": 50.0,"
        "                    \"control_node\": \"ctl\" } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1,"
        "                \"method\": \"rk4\" }"
        "}",
        "THRESHOLD_MODEL");
    const double *Q = GSSK_GetState(inst);

    for (int i = 0; i < 5; i++) GSSK_Step(inst, 0.1);

    /* Q_origin is 10: above 5, below 50. The control is 500, above both — so
     * if the comparand were Q_control, BOTH edges would be flowing. */
    CHECK(Q[1] > 1e-9, "the below-Q_origin threshold edge did not flow");
    CHECK(Q[2] == 0.0,
          "the above-Q_origin threshold edge flowed (%.9g) — the comparand is "
          "not Q_origin", Q[2]);

    GSSK_Free(inst);
}

int main(void) {
    printf("=== Limit / threshold logic constant tests (GIP-0001 G6) ===\n\n");

    test_c_from_threshold();
    test_control_node_wins();
    test_no_c_is_rejected();
    test_decaying_control_closes_the_path();
    test_threshold_comparand();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL LIMIT LOGIC TESTS PASSED ===\n");
    return 0;
}
