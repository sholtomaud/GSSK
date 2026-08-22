/* Flat carrier accessors — the getters that let a consumer read a carrier's
 * unit and conservation flag without decoding GSSK_Carrier.
 *
 * The data has always been there: carriers are parsed, stored, and round-trip
 * through GSSK_SerializeModel with `unit` and `conserved` intact, and in C
 * GSSK_GetCarrier hands back the whole struct. The gap is the WASM boundary,
 * where that same call is a bare heap pointer and a JS caller has to assume
 * field offsets, `bool` width and the absence of trailing padding. None of
 * that is an ABI contract, and a field reorder breaks it by returning
 * plausible garbage rather than by failing.
 *
 * So the tests below do two things: exercise each flat getter including its
 * out-of-range behaviour, and assert the flat path agrees with GSSK_GetCarrier
 * for every index, which is the check that stops the two drifting apart.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Three carriers, three distinct units, mixed conserved flags — including one
 * declared false explicitly, so "not conserved" is a real answer and not just
 * the absence of the field. */
static const char *MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"carriers\": ["
    "    { \"id\": \"energy\", \"unit\": \"kWh\",    \"conserved\": true  },"
    "    { \"id\": \"money\",  \"unit\": \"AUD\",    \"conserved\": true  },"
    "    { \"id\": \"signal\", \"unit\": \"AUD/kWh\", \"conserved\": false }"
    "  ],"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0, \"carrier\": \"energy\" },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0,  \"carrier\": \"energy\" }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"src\", \"target\": \"tank\", \"carrier\": \"energy\","
    "      \"logic\": \"constant\", \"params\": { \"k\": 1.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 }"
    "}";

/* A model with no carriers block at all — the out-of-range paths have to be
 * correct when carrier_count is 0, which is the common case for older models. */
static const char *NO_CARRIERS_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"src\", \"target\": \"tank\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 }"
    "}";

static const char *EXPECT_ID[]   = { "energy", "money",  "signal"  };
static const char *EXPECT_UNIT[] = { "kWh",    "AUD",    "AUD/kWh" };
static const int   EXPECT_CONS[] = { 1,        1,        0         };
#define N_CARRIERS 3

/* ---------------------------------------------------------------- */

static void test_values(GSSK_Instance *inst) {
    printf("Testing each flat getter against the declared carriers...\n");

    CHECK(GSSK_GetCarrierCount(inst) == N_CARRIERS,
          "expected %d carriers, got %zu", N_CARRIERS, GSSK_GetCarrierCount(inst));

    for (size_t i = 0; i < N_CARRIERS; i++) {
        const char *id   = GSSK_GetCarrierID(inst, i);
        const char *unit = GSSK_GetCarrierUnit(inst, i);
        int cons         = GSSK_GetCarrierConserved(inst, i);

        CHECK(id != NULL,   "GSSK_GetCarrierID(%zu) must never be NULL", i);
        CHECK(unit != NULL, "GSSK_GetCarrierUnit(%zu) must never be NULL", i);
        if (!id || !unit) continue;

        CHECK(strcmp(id, EXPECT_ID[i]) == 0,
              "carrier %zu id: expected '%s', got '%s'", i, EXPECT_ID[i], id);
        CHECK(strcmp(unit, EXPECT_UNIT[i]) == 0,
              "carrier %zu unit: expected '%s', got '%s'", i, EXPECT_UNIT[i], unit);
        CHECK(cons == EXPECT_CONS[i],
              "carrier %zu conserved: expected %d, got %d", i, EXPECT_CONS[i], cons);
    }
    printf("  3 carriers OK\n");
}

/* The whole point: a JS consumer that uses the flat path must see exactly what
 * a C consumer decoding the struct sees, at every index, or the two APIs have
 * silently diverged. */
static void test_agrees_with_struct(GSSK_Instance *inst) {
    printf("Testing the flat getters agree with GSSK_GetCarrier...\n");
    for (size_t i = 0; i < GSSK_GetCarrierCount(inst); i++) {
        const GSSK_Carrier *c = GSSK_GetCarrier(inst, i);
        CHECK(c != NULL, "GSSK_GetCarrier(%zu) should not be NULL in range", i);
        if (!c) continue;
        CHECK(strcmp(GSSK_GetCarrierID(inst, i), c->id) == 0,
              "id drift at %zu: flat '%s' vs struct '%s'",
              i, GSSK_GetCarrierID(inst, i), c->id);
        CHECK(strcmp(GSSK_GetCarrierUnit(inst, i), c->unit) == 0,
              "unit drift at %zu: flat '%s' vs struct '%s'",
              i, GSSK_GetCarrierUnit(inst, i), c->unit);
        CHECK(GSSK_GetCarrierConserved(inst, i) == (c->conserved ? 1 : 0),
              "conserved drift at %zu", i);
    }
    printf("  Flat and struct paths agree\n");
}

/* The conventions differ between the two, deliberately, and the header says so.
 * Pin it, because "returns NULL" and "returns empty string" are exactly the
 * kind of thing a caller gets wrong once and then crashes on. */
static void test_out_of_range(GSSK_Instance *inst) {
    printf("Testing out-of-range behaviour...\n");
    size_t n = GSSK_GetCarrierCount(inst);

    const size_t bad[] = { n, n + 1, (size_t)-1 };
    for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
        size_t idx = bad[b];
        const char *id   = GSSK_GetCarrierID(inst, idx);
        const char *unit = GSSK_GetCarrierUnit(inst, idx);

        CHECK(id != NULL && id[0] == '\0',
              "GSSK_GetCarrierID(%zu) must be \"\", not NULL", idx);
        CHECK(unit != NULL && unit[0] == '\0',
              "GSSK_GetCarrierUnit(%zu) must be \"\", not NULL", idx);
        CHECK(GSSK_GetCarrierConserved(inst, idx) == 0,
              "GSSK_GetCarrierConserved(%zu) must be 0", idx);
        /* The other convention, unchanged: */
        CHECK(GSSK_GetCarrier(inst, idx) == NULL,
              "GSSK_GetCarrier(%zu) must still be NULL", idx);
    }
    printf("  Out-of-range OK (\"\" for strings, NULL for the struct)\n");
}

static void test_find(GSSK_Instance *inst) {
    printf("Testing GSSK_FindCarrierIdx round-trips...\n");
    for (size_t i = 0; i < GSSK_GetCarrierCount(inst); i++) {
        const char *id = GSSK_GetCarrierID(inst, i);
        int found = GSSK_FindCarrierIdx(inst, id);
        CHECK(found == (int)i, "'%s' should be at %zu, got %d", id, i, found);
        if (found >= 0)
            CHECK(strcmp(GSSK_GetCarrierID(inst, (size_t)found), id) == 0,
                  "id -> idx -> id must round-trip for '%s'", id);
    }
    CHECK(GSSK_FindCarrierIdx(inst, "nonesuch") == -1,
          "absent id must be -1");
    CHECK(GSSK_FindCarrierIdx(inst, "") == -1,
          "empty id must be -1");
    CHECK(GSSK_FindCarrierIdx(inst, NULL) == -1,
          "NULL id must be -1, not a crash");
    printf("  Lookup OK\n");
}

static void test_null_instance(void) {
    printf("Testing NULL instance...\n");
    const char *id   = GSSK_GetCarrierID(NULL, 0);
    const char *unit = GSSK_GetCarrierUnit(NULL, 0);
    CHECK(id != NULL && id[0] == '\0',   "GSSK_GetCarrierID(NULL) must be \"\"");
    CHECK(unit != NULL && unit[0] == '\0', "GSSK_GetCarrierUnit(NULL) must be \"\"");
    CHECK(GSSK_GetCarrierConserved(NULL, 0) == 0, "GSSK_GetCarrierConserved(NULL) must be 0");
    CHECK(GSSK_FindCarrierIdx(NULL, "energy") == -1, "GSSK_FindCarrierIdx(NULL) must be -1");
    printf("  NULL instance OK\n");
}

static void test_model_without_carriers(void) {
    printf("Testing a model that declares no carriers...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(NO_CARRIERS_MODEL, &inst);
    CHECK(st == GSSK_SUCCESS, "no-carriers model must load, got %d", (int)st);
    if (st == GSSK_SUCCESS) {
        CHECK(GSSK_GetCarrierCount(inst) == 0, "carrier count must be 0");
        CHECK(GSSK_GetCarrierID(inst, 0)[0] == '\0', "index 0 must be \"\"");
        CHECK(GSSK_GetCarrierUnit(inst, 0)[0] == '\0', "index 0 unit must be \"\"");
        CHECK(GSSK_GetCarrierConserved(inst, 0) == 0, "index 0 conserved must be 0");
        CHECK(GSSK_FindCarrierIdx(inst, "energy") == -1, "lookup must be -1");
    }
    GSSK_Free(inst);
    printf("  Empty carrier list OK\n");
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== Carrier accessor tests ===\n\n");

    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(MODEL, &inst);
    if (st != GSSK_SUCCESS) {
        printf("FATAL: model failed to load: %d (%s)\n",
               (int)st, inst ? GSSK_GetErrorDescription(inst) : "");
        GSSK_Free(inst);
        return 1;
    }

    test_values(inst);
    test_agrees_with_struct(inst);
    test_out_of_range(inst);
    test_find(inst);
    GSSK_Free(inst);

    test_null_instance();
    test_model_without_carriers();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL CARRIER API TESTS PASSED ===\n");
    return 0;
}
