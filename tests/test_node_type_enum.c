/* GSSK_NodeType and GSSK_GetNodeType (GIP-0001 G7).
 *
 * Most of G7 was already closed on main: GSSK_GetNodeTypeString returns the
 * type, the primitive set is a closed enum in gssk.schema.json, and GSSK_Init
 * rejects an unrecognised type naming the node. What remained is that a C
 * consumer had only string comparison for a decision the kernel makes with an
 * integer, which is slower and — worse — typo-able in a way the compiler
 * cannot see: `strcmp(t, "loop_limted")` is a valid program that quietly never
 * matches.
 *
 * The enum was already there, privately, in src/gssk.c and already called
 * GSSK_NodeType. This moves it to the public header rather than declaring a
 * second copy, so there is no pair to drift.
 *
 * Three things need pinning, and the third is the reason to bother:
 *
 *   1. The enum and the string function agree for every one of the nine
 *      primitives — asserted over the whole set, not a sample, so adding a
 *      tenth without extending both fails here.
 *   2. A composite node reports the PRIMITIVE its member expanded to, not the
 *      composite's name. `producer` is not a node type; it is three nodes.
 *   3. Out of bounds returns GSSK_NODE_INVALID. GSSK_GetNodeTypeString cannot
 *      report an error — it returns "storage", indistinguishable from a real
 *      storage node — and G7 asks for GSSK_GetNodeID's contract instead.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The nine primitives, in enum order, paired with the string they must
 * stringify to. Written out rather than derived, so the test is an
 * independent statement of the mapping and not a restatement of the kernel's. */
static const struct { GSSK_NodeType type; const char *str; } PRIMITIVES[] = {
    { GSSK_NODE_STORAGE,      "storage"      },
    { GSSK_NODE_SOURCE,       "source"       },
    { GSSK_NODE_SINK,         "sink"         },
    { GSSK_NODE_CONSTANT,     "constant"     },
    { GSSK_NODE_INTERACTION,  "interaction"  },
    { GSSK_NODE_GAIN,         "gain"         },
    { GSSK_NODE_LOOP_LIMITED, "loop_limited" },
    { GSSK_NODE_EXCHANGE,     "exchange"     },
    { GSSK_NODE_SWITCH,       "switch"       },
};
#define N_PRIMITIVES ((int)(sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0])))

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

/* One model carrying all nine primitives at once, so the ordinal-to-string
 * agreement is checked at every index of one real instance rather than nine
 * one-node models. A processing node needs its inputs to be well-formed, so
 * the shape is: source -> store, and the processing nodes hang off store. */
static const char *ALL_TYPES_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"n_storage\",   \"type\": \"storage\",      \"value\": 10.0 },"
    "    { \"id\": \"n_source\",    \"type\": \"source\",       \"value\": 5.0  },"
    "    { \"id\": \"n_sink\",      \"type\": \"sink\",         \"value\": 0.0  },"
    "    { \"id\": \"n_constant\",  \"type\": \"constant\",     \"value\": 2.0  },"
    "    { \"id\": \"n_interact\",  \"type\": \"interaction\",  \"value\": 0.0, \"params\": { \"k\": 0.01 } },"
    "    { \"id\": \"n_gain\",      \"type\": \"gain\",         \"value\": 0.0, \"params\": { \"k\": 2.0 } },"
    "    { \"id\": \"n_loop\",      \"type\": \"loop_limited\", \"value\": 0.0, \"params\": { \"k\": 0.5, \"C\": 10.0 } },"
    "    { \"id\": \"n_exchange\",  \"type\": \"exchange\",     \"value\": 0.0 },"
    "    { \"id\": \"n_switch\",    \"type\": \"switch\",       \"value\": 0.0, \"params\": { \"threshold\": 1.0 } }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"n_source\",  \"target\": \"n_storage\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } },"
    "    { \"origin\": \"n_storage\", \"target\": \"n_sink\",    \"logic\": \"linear\",   \"params\": { \"k\": 0.1 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 }"
    "}";

/* `producer` is a built-in composite expanding to body (storage), gate
 * (interaction) and heat (sink) — three DIFFERENT primitives, so a build that
 * reported one type for the whole composite cannot pass. */
static const char *COMPOSITE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"sun\",  \"type\": \"source\",   \"value\": 10.0 },"
    "    { \"id\": \"tree\", \"type\": \"producer\", \"value\": 5.0  },"
    "    { \"id\": \"soil\", \"type\": \"sink\",     \"value\": 0.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"sun\",  \"target\": \"tree\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } },"
    "    { \"origin\": \"tree\", \"target\": \"soil\", \"logic\": \"linear\",   \"params\": { \"k\": 0.1 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 }"
    "}";

/* 1. The enum agrees with the string function at every index, and every one of
 *    the nine primitives is actually reached. */
static void test_agrees_with_string(void) {
    printf("The enum and GSSK_GetNodeTypeString agree, over all nine primitives\n");
    GSSK_Instance *inst = load(ALL_TYPES_MODEL, "ALL_TYPES_MODEL");

    size_t n = GSSK_GetStateSize(inst);
    CHECK(n == 9, "model has %zu nodes, expected 9", n);

    int seen[N_PRIMITIVES];
    memset(seen, 0, sizeof(seen));

    for (size_t i = 0; i < n; i++) {
        GSSK_NodeType t = GSSK_GetNodeType(inst, i);
        const char *s   = GSSK_GetNodeTypeString(inst, i);

        CHECK(t != GSSK_NODE_INVALID,
              "node %zu ('%s') reports GSSK_NODE_INVALID for a valid index",
              i, GSSK_GetNodeID(inst, i));

        int found = 0;
        for (int p = 0; p < N_PRIMITIVES; p++) {
            if (PRIMITIVES[p].type != t) continue;
            found = 1;
            seen[p] = 1;
            CHECK(strcmp(s, PRIMITIVES[p].str) == 0,
                  "node %zu ('%s'): enum %d says \"%s\" but "
                  "GSSK_GetNodeTypeString says \"%s\"",
                  i, GSSK_GetNodeID(inst, i), (int)t, PRIMITIVES[p].str, s);
        }
        CHECK(found, "node %zu ('%s') reports enum value %d, which is not a "
                     "primitive in the table", i, GSSK_GetNodeID(inst, i), (int)t);
    }

    /* Every primitive must have been exercised, or the agreement above was
     * only checked on the handful this model happens to use. */
    for (int p = 0; p < N_PRIMITIVES; p++)
        CHECK(seen[p], "primitive \"%s\" (enum %d) never appeared — the model "
                       "no longer covers the whole set",
              PRIMITIVES[p].str, (int)PRIMITIVES[p].type);

    GSSK_Free(inst);
}

/* The ordinals are a published contract that crosses the WASM boundary as bare
 * integers, so renumbering them is a silent breaking change for every JS
 * consumer. Pinned literally. */
static void test_ordinals_are_pinned(void) {
    printf("Enum ordinals are pinned — renumbering breaks the WASM boundary\n");
    CHECK((int)GSSK_NODE_STORAGE      == 0, "GSSK_NODE_STORAGE moved");
    CHECK((int)GSSK_NODE_SOURCE       == 1, "GSSK_NODE_SOURCE moved");
    CHECK((int)GSSK_NODE_SINK         == 2, "GSSK_NODE_SINK moved");
    CHECK((int)GSSK_NODE_CONSTANT     == 3, "GSSK_NODE_CONSTANT moved");
    CHECK((int)GSSK_NODE_INTERACTION  == 4, "GSSK_NODE_INTERACTION moved");
    CHECK((int)GSSK_NODE_GAIN         == 5, "GSSK_NODE_GAIN moved");
    CHECK((int)GSSK_NODE_LOOP_LIMITED == 6, "GSSK_NODE_LOOP_LIMITED moved");
    CHECK((int)GSSK_NODE_EXCHANGE     == 7, "GSSK_NODE_EXCHANGE moved");
    CHECK((int)GSSK_NODE_SWITCH       == 8, "GSSK_NODE_SWITCH moved");
    CHECK((int)GSSK_NODE_INVALID      == 9, "GSSK_NODE_INVALID moved");
}

/* 2. A composite reports the primitive each member expanded to. */
static void test_composite_reports_primitives(void) {
    printf("A composite's members report their primitives, not the composite\n");
    GSSK_Instance *inst = load(COMPOSITE_MODEL, "COMPOSITE_MODEL");

    size_t n = GSSK_GetStateSize(inst);
    /* sun, soil, and producer's three members. */
    CHECK(n == 5, "expanded model has %zu nodes, expected 5", n);

    int saw_storage = 0, saw_interaction = 0, saw_sink_in_composite = 0;
    int members = 0;

    for (size_t i = 0; i < n; i++) {
        GSSK_NodeType t  = GSSK_GetNodeType(inst, i);
        const char *id   = GSSK_GetNodeID(inst, i);
        const char *comp = GSSK_GetNodeComposite(inst, i);

        /* Nothing may report an enum value outside the primitive set — there
         * is no GSSK_NODE_PRODUCER, and that is the point. */
        CHECK(t >= GSSK_NODE_STORAGE && t <= GSSK_NODE_SWITCH,
              "node '%s' reports enum %d, outside the primitive set", id, (int)t);

        if (!comp || strcmp(comp, "tree") != 0) continue;
        members++;

        const char *role = GSSK_GetNodeRole(inst, i);
        if (role && strcmp(role, "body") == 0) {
            saw_storage = 1;
            CHECK(t == GSSK_NODE_STORAGE,
                  "producer's 'body' reports %d, expected GSSK_NODE_STORAGE",
                  (int)t);
        } else if (role && strcmp(role, "gate") == 0) {
            saw_interaction = 1;
            CHECK(t == GSSK_NODE_INTERACTION,
                  "producer's 'gate' reports %d, expected GSSK_NODE_INTERACTION",
                  (int)t);
        } else if (role && strcmp(role, "heat") == 0) {
            saw_sink_in_composite = 1;
            CHECK(t == GSSK_NODE_SINK,
                  "producer's 'heat' reports %d, expected GSSK_NODE_SINK",
                  (int)t);
        }
    }

    CHECK(members == 3, "found %d members of 'tree', expected 3", members);
    /* The three members must be three DIFFERENT primitives, or a build that
     * reported one type for the whole composite would pass the loop above. */
    CHECK(saw_storage && saw_interaction && saw_sink_in_composite,
          "the composite did not expand to three distinct primitives "
          "(storage=%d interaction=%d sink=%d)",
          saw_storage, saw_interaction, saw_sink_in_composite);

    GSSK_Free(inst);
}

/* 3. Out of bounds and NULL follow GSSK_GetNodeID's contract, not the string
 *    function's. Asserted side by side, because the difference IS the fix. */
static void test_out_of_bounds(void) {
    printf("Out of bounds returns GSSK_NODE_INVALID, unlike the string form\n");
    GSSK_Instance *inst = load(ALL_TYPES_MODEL, "ALL_TYPES_MODEL");
    size_t n = GSSK_GetStateSize(inst);

    CHECK(GSSK_GetNodeType(inst, n) == GSSK_NODE_INVALID,
          "index == node_count returned %d", (int)GSSK_GetNodeType(inst, n));
    CHECK(GSSK_GetNodeType(inst, n + 1000) == GSSK_NODE_INVALID,
          "a far out-of-range index returned %d",
          (int)GSSK_GetNodeType(inst, n + 1000));
    CHECK(GSSK_GetNodeType(NULL, 0) == GSSK_NODE_INVALID,
          "NULL instance returned %d", (int)GSSK_GetNodeType(NULL, 0));

    /* Same conditions on GSSK_GetNodeID: NULL. The two now agree that an
     * invalid index is reportable. */
    CHECK(GSSK_GetNodeID(inst, n) == NULL, "GSSK_GetNodeID's contract changed");
    CHECK(GSSK_GetNodeID(NULL, 0) == NULL, "GSSK_GetNodeID's contract changed");

    /* And the wart this exists to route around: the string form still cannot
     * say so. If this ever stops being true the header's @warning is stale. */
    CHECK(strcmp(GSSK_GetNodeTypeString(inst, n), "storage") == 0,
          "GSSK_GetNodeTypeString no longer returns \"storage\" out of bounds "
          "— update the @warning in include/gssk.h");

    GSSK_Free(inst);
}

int main(void) {
    printf("=== GSSK_NodeType enum tests (GIP-0001 G7) ===\n\n");

    test_agrees_with_string();
    test_ordinals_are_pinned();
    test_composite_reports_primitives();
    test_out_of_bounds();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL NODE TYPE ENUM TESTS PASSED ===\n");
    return 0;
}
