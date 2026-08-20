/* Node type validation — the parser must reject a type string it does not
 * recognise instead of falling back to `storage`.
 *
 * The fallback was the hazard ADR 0004 left open: schema validation is
 * advisory, and `Node.type` cannot be a closed enum because archetype names
 * are user-defined, so no validator can tell a typo from a legitimate
 * archetype reference. Only the parser knows which archetypes were declared,
 * which makes this the one place the check can live.
 *
 * These tests pin both halves of that: everything legitimate still loads
 * (nine primitives, four built-in composites, a declared archetype), and
 * everything else is an error naming the node and the offending string.
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

/* Build a one-node model around `type`, plus a source feeding it so the model
 * is well-formed regardless of which type is under test. */
static void model_with_type(char *buf, size_t cap, const char *type) {
    snprintf(buf, cap,
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"src\", \"type\": \"source\", \"value\": 10.0 },"
        "    { \"id\": \"n\",   \"type\": \"%s\",     \"value\": 1.0 }"
        "  ],"
        "  \"edges\": ["
        "    { \"origin\": \"src\", \"target\": \"n\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1 }"
        "}", type);
}

static void expect_loads(const char *type, const char *what) {
    char json[1024];
    model_with_type(json, sizeof(json), type);
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    CHECK(st == GSSK_SUCCESS, "%s '%s' should load, got status %d (%s)",
          what, type, (int)st, inst ? GSSK_GetErrorDescription(inst) : "");
    GSSK_Free(inst);
}

static void expect_rejected(const char *type, const char *why) {
    char json[1024];
    model_with_type(json, sizeof(json), type);
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s: type '%s' should be GSSK_ERR_SCHEMA_VIOLATION, got %d", why, type, (int)st);
    if (st != GSSK_SUCCESS && inst) {
        const char *msg = GSSK_GetErrorDescription(inst);
        /* A UI needs the node id to highlight the offending element, and the
         * string to say what was wrong with it. The array index alone is not
         * enough to act on. */
        CHECK(strstr(msg, "'n'") != NULL,
              "error for '%s' must name the node id: \"%s\"", type, msg);
        CHECK(strstr(msg, type) != NULL,
              "error for '%s' must quote the offending type: \"%s\"", type, msg);
    }
    GSSK_Free(inst);
}

/* ---------------------------------------------------------------- */

static void test_primitives_load(void) {
    printf("Testing the nine primitives still load...\n");
    static const char *primitives[] = {
        "storage", "source", "sink", "constant", "interaction",
        "gain", "loop_limited", "exchange", "switch"
    };
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++)
        expect_loads(primitives[i], "primitive");
    printf("  9 primitives OK\n");
}

static void test_builtin_composites_load(void) {
    printf("Testing built-in composites still load...\n");
    static const char *composites[] = {
        "producer", "consumer", "misc_box", "system_frame"
    };
    for (size_t i = 0; i < sizeof(composites) / sizeof(composites[0]); i++)
        expect_loads(composites[i], "built-in composite");
    printf("  4 built-in composites OK\n");
}

/* Acceptance criterion 3 — the check must not break archetype dispatch. A
 * declared archetype is indistinguishable from a typo to a schema validator;
 * it must remain distinguishable to the parser. Mirrors the model shape in
 * test_advanced.c's Phase 8 tests. */
static const char *USER_ARCHETYPE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"archetypes\": {"
    "    \"relay\": {"
    "      \"nodes\": [ { \"id\": \"buf\", \"type\": \"storage\", \"value\": 0.0 } ],"
    "      \"edges\": [],"
    "      \"ports\": { \"in\": \"buf\", \"out\": \"buf\" }"
    "    }"
    "  },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\", \"value\": 10.0 },"
    "    { \"id\": \"r\",    \"type\": \"relay\",  \"value\": 5.0  },"
    "    { \"id\": \"sink\", \"type\": \"sink\",   \"value\": 0.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"src\", \"target\": \"r\",    \"logic\": \"constant\", \"params\": { \"k\": 1.0 } },"
    "    { \"origin\": \"r\",   \"target\": \"sink\", \"logic\": \"linear\",   \"params\": { \"k\": 0.5 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

static void test_declared_archetype_still_resolves(void) {
    printf("Testing a declared archetype still resolves...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(USER_ARCHETYPE_MODEL, &inst);
    CHECK(st == GSSK_SUCCESS, "declared archetype 'relay' must load, got %d (%s)",
          (int)st, inst ? GSSK_GetErrorDescription(inst) : "");
    if (st == GSSK_SUCCESS) {
        /* It expanded, rather than merely being tolerated as a bare node. */
        int found = 0;
        for (size_t i = 0; i < GSSK_GetStateSize(inst); i++)
            if (strcmp(GSSK_GetNodeID(inst, i), "r__buf") == 0) found = 1;
        CHECK(found, "'relay' must expand to its member node 'r__buf'");
    }
    GSSK_Free(inst);
    printf("  Declared archetype OK\n");
}

static void test_unknown_types_rejected(void) {
    printf("Testing unknown types are rejected...\n");
    expect_rejected("storge",     "typo'd primitive");
    expect_rejected("Source",     "case-wrong primitive");
    expect_rejected("producer_",  "typo'd composite");
    expect_rejected("relay",      "archetype name not declared by this model");
    expect_rejected("",           "empty type string");
    printf("  Unknown types rejected\n");
}

/* The point of the whole task: this model used to run to completion, with
 * `grasss` silently a storage node, and report success. */
static const char *TYPO_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"sun\",    \"type\": \"source\", \"value\": 100.0 },"
    "    { \"id\": \"grasss\", \"type\": \"storge\", \"value\": 10.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"sun\", \"target\": \"grasss\", \"logic\": \"linear\", \"params\": { \"k\": 0.1 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

static void test_typo_fails_at_init_not_silently(void) {
    printf("Testing a typo fails at init instead of running to completion...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(TYPO_MODEL, &inst);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "typo model must fail at init, got %d", (int)st);
    if (inst) {
        const char *msg = GSSK_GetErrorDescription(inst);
        CHECK(strstr(msg, "grasss") != NULL, "message must name 'grasss': \"%s\"", msg);
        CHECK(strstr(msg, "storge") != NULL, "message must quote 'storge': \"%s\"", msg);
        printf("  message: %s\n", msg);
    }
    GSSK_Free(inst);
}

/* ---------------------------------------------------------------- */

static const char *RUNTIME_BASE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0  }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"src\", \"target\": \"tank\", \"logic\": \"linear\", \"params\": { \"k\": 0.5 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

/* A rejected add must be a true no-op. This is the path a drag-and-drop editor
 * calls on every element change, so a rejection that half-grew the arrays
 * would corrupt the model the author is still editing. */
static void assert_add_is_noop(const char *fragment, const char *what,
                               const char *expect_in_msg) {
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(RUNTIME_BASE_MODEL, &inst) == GSSK_SUCCESS);

    size_t n_before = GSSK_GetStateSize(inst);
    size_t e_before = GSSK_GetEdgeCount(inst);
    double *state_before = malloc(n_before * sizeof(double));
    memcpy(state_before, GSSK_GetState(inst), n_before * sizeof(double));

    GSSK_Status st = GSSK_AddNode(inst, fragment);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s: AddNode should be GSSK_ERR_SCHEMA_VIOLATION, got %d", what, (int)st);

    const char *msg = GSSK_GetErrorDescription(inst);
    CHECK(strstr(msg, expect_in_msg) != NULL,
          "%s: message should mention \"%s\", got \"%s\"", what, expect_in_msg, msg);

    CHECK(GSSK_GetStateSize(inst) == n_before,
          "%s: node_count changed (%zu -> %zu)", what, n_before, GSSK_GetStateSize(inst));
    CHECK(GSSK_GetEdgeCount(inst) == e_before,
          "%s: edge_count changed (%zu -> %zu)", what, e_before, GSSK_GetEdgeCount(inst));
    for (size_t i = 0; i < n_before; i++)
        CHECK(GSSK_GetState(inst)[i] == state_before[i],
              "%s: state[%zu] changed", what, i);

    /* And the instance is still usable — rejection must not poison it. */
    CHECK(GSSK_Step(inst, GSSK_GetDt(inst)) == GSSK_SUCCESS,
          "%s: GSSK_Step must still succeed after a rejected add", what);

    free(state_before);
    GSSK_Free(inst);
}

static void test_addnode_rejects(void) {
    printf("Testing GSSK_AddNode rejects non-primitives as a no-op...\n");

    assert_add_is_noop("{\"id\":\"x\",\"type\":\"storge\",\"value\":1.0}",
                       "typo'd primitive", "storge");
    assert_add_is_noop("{\"id\":\"x\",\"type\":\"Source\",\"value\":1.0}",
                       "case-wrong primitive", "Source");
    assert_add_is_noop("{\"id\":\"x\",\"type\":\"nonesuch\",\"value\":1.0}",
                       "undeclared archetype name", "nonesuch");

    /* AddNode expands nothing, so a composite here would have become a lone
     * storage node. It is refused, and the message says why — expanding
     * composites at runtime is deliberately a separate change. */
    assert_add_is_noop("{\"id\":\"x\",\"type\":\"producer\",\"value\":1.0}",
                       "built-in composite at runtime", "runtime");

    printf("  AddNode rejections OK\n");
}

static void test_addnode_still_accepts_primitives(void) {
    printf("Testing GSSK_AddNode still accepts primitives...\n");
    static const char *primitives[] = {
        "storage", "source", "sink", "constant", "interaction",
        "gain", "loop_limited", "exchange", "switch"
    };
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++) {
        GSSK_Instance *inst = NULL;
        assert(GSSK_Init(RUNTIME_BASE_MODEL, &inst) == GSSK_SUCCESS);
        size_t before = GSSK_GetStateSize(inst);
        char frag[256];
        snprintf(frag, sizeof(frag),
                 "{\"id\":\"added\",\"type\":\"%s\",\"value\":2.0}", primitives[i]);
        GSSK_Status st = GSSK_AddNode(inst, frag);
        CHECK(st == GSSK_SUCCESS, "AddNode('%s') should succeed, got %d (%s)",
              primitives[i], (int)st, GSSK_GetErrorDescription(inst));
        CHECK(GSSK_GetStateSize(inst) == before + 1,
              "AddNode('%s') should append one node", primitives[i]);
        GSSK_Free(inst);
    }
    printf("  9 primitives accepted at runtime\n");
}

/* The archival story (Phase G) depends on serialised output still loading.
 * GSSK_SerializeModel writes post-expansion primitives via node_type_str, so a
 * model built from composites must survive the stricter parser on reload — if
 * it emitted "producer" for an expanded node, this check would fail. */
static void test_serialized_model_reloads(void) {
    printf("Testing serialised output still reloads under the stricter parser...\n");

    static const char *COMPOSITE_MODEL =
        "{"
        "  \"metadata\": { \"schema_version\": 4 },"
        "  \"nodes\": ["
        "    { \"id\": \"sun\",   \"type\": \"source\",       \"value\": 100.0 },"
        "    { \"id\": \"plant\", \"type\": \"producer\",     \"value\": 50.0 },"
        "    { \"id\": \"deer\",  \"type\": \"consumer\",     \"value\": 10.0 },"
        "    { \"id\": \"box\",   \"type\": \"misc_box\",     \"value\": 1.0  },"
        "    { \"id\": \"frame\", \"type\": \"system_frame\", \"value\": 0.0  }"
        "  ],"
        "  \"edges\": ["
        "    { \"origin\": \"sun\", \"target\": \"plant\", \"logic\": \"constant\", \"params\": { \"k\": 1.0 } }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 5, \"dt\": 0.1 }"
        "}";

    GSSK_Instance *a = NULL;
    assert(GSSK_Init(COMPOSITE_MODEL, &a) == GSSK_SUCCESS);
    /* AUTO mode may report a solver-agreement warning on this stiff little
     * model; that is not what is under test here. */
    GSSK_Status step = GSSK_Step(a, GSSK_GetDt(a));
    assert(step == GSSK_SUCCESS || step == GSSK_WARN_SOLVER_DIVERGENCE);

    char *model_json = NULL, *snap_json = NULL;
    CHECK(GSSK_SerializeModel(a, &model_json) == GSSK_SUCCESS, "SerializeModel failed");
    CHECK(GSSK_SerializeSnapshot(a, &snap_json) == GSSK_SUCCESS, "SerializeSnapshot failed");

    GSSK_Instance *b = NULL;
    GSSK_Status st = GSSK_Init(model_json, &b);
    CHECK(st == GSSK_SUCCESS, "serialised model must reload, got %d (%s)",
          (int)st, b ? GSSK_GetErrorDescription(b) : "");
    CHECK(GSSK_GetStateSize(b) == GSSK_GetStateSize(a),
          "reloaded model should have the same node count (%zu vs %zu)",
          GSSK_GetStateSize(b), GSSK_GetStateSize(a));

    GSSK_Instance *c = NULL;
    st = GSSK_Init(snap_json, &c);
    CHECK(st == GSSK_SUCCESS, "serialised snapshot must reload, got %d (%s)",
          (int)st, c ? GSSK_GetErrorDescription(c) : "");

    GSSK_FreeString(model_json);
    GSSK_FreeString(snap_json);
    GSSK_Free(a); GSSK_Free(b); GSSK_Free(c);
    printf("  Serialised round-trip OK\n");
}

int main(void) {
    printf("=== Node type validation tests ===\n\n");

    test_primitives_load();
    test_builtin_composites_load();
    test_declared_archetype_still_resolves();
    test_unknown_types_rejected();
    test_typo_fails_at_init_not_silently();
    test_addnode_rejects();
    test_addnode_still_accepts_primitives();
    test_serialized_model_reloads();

    printf("\n");
    if (failures) {
        printf("=== %d CHECK(s) FAILED ===\n", failures);
        return 1;
    }
    printf("=== All node type validation tests PASSED ===\n");
    return 0;
}
