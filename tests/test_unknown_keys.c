/* Unknown model keys must be an error, not silence.
 *
 * Before this, the following model loaded, returned GSSK_SUCCESS and ran to
 * completion with `forcing`, `nonsense_top_level_key` and `bogus_param` all
 * ignored without a word:
 *
 *   {"metadata":{"schema_version":4},
 *    "forcing":{"sun":{"waveform":"sine","amplitude":5,"period":24}},
 *    "nonsense_top_level_key":123,
 *    "nodes":[...], "edges":[{... "params":{"k":0.5,"bogus_param":9}}], ...}
 *
 * The danger is not the wasted key. It is that a model authored against a
 * kernel that HAS forcing, loaded by a kernel that does not, produces a
 * plausible completed run with no diagnostic: its JSON says "forced", its
 * trajectory says "constant", and nothing reconciles the two. For a kernel
 * whose case rests on reproducibility and on the Phase G archival story that
 * is the worst available failure mode — not a crash, a quietly different
 * model. Same hazard class as the node-`type` fallback that ADR 0004 left
 * open and reject-unknown-node-types closed; a wrong key is the same mistake
 * one level up.
 *
 * gssk.schema.json was already strict here — additionalProperties:false at the
 * root and on Node, Edge, EdgeParams and Config, with ^_ keys permitted by
 * patternProperties. So these tests pin the kernel to a rule the project
 * already publishes rather than inventing one.
 *
 * Shaped after tests/test_node_type_validation.c, and covering both parsers
 * for the same reason it does: GSSK_Init and GSSK_AddNode/GSSK_AddEdge are
 * separate code paths.
 */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
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

/* A well-formed model with one hole punched in it, so each level can be tested
 * with everything else held constant. `%s` slots take an extra key (with its
 * leading comma) or an empty string. */
static void build(char *buf, size_t cap,
                  const char *root_extra, const char *node_extra,
                  const char *edge_extra, const char *param_extra,
                  const char *config_extra) {
    snprintf(buf, cap,
        "{"
        "  \"metadata\": { \"schema_version\": 4 }%s,"
        "  \"nodes\": ["
        "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0 },"
        "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0%s }"
        "  ],"
        "  \"edges\": ["
        "    { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\","
        "      \"logic\": \"constant\", \"params\": { \"k\": 0.5%s }%s }"
        "  ],"
        "  \"config\": { \"t_start\": 0, \"t_end\": 1, \"dt\": 0.1%s }"
        "}",
        root_extra, node_extra, param_extra, edge_extra, config_extra);
}

static const char *EMPTY = "";

static void expect_rejected(const char *json, const char *level,
                            const char *key, const char *container) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s: unknown key '%s' should be GSSK_ERR_SCHEMA_VIOLATION, got %d",
          level, key, (int)st);
    if (inst) {
        const char *msg = GSSK_GetErrorDescription(inst);
        /* The key alone is not actionable — an authoring UI needs to know
         * WHICH element to highlight. */
        CHECK(strstr(msg, key) != NULL,
              "%s: message must name the key '%s': \"%s\"", level, key, msg);
        CHECK(strstr(msg, container) != NULL,
              "%s: message must name the container '%s': \"%s\"",
              level, container, msg);
        if (st == GSSK_ERR_SCHEMA_VIOLATION)
            printf("    %s\n", msg);
    }
    GSSK_Free(inst);
}

static void expect_loads(const char *json, const char *what) {
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    CHECK(st == GSSK_SUCCESS, "%s should load, got %d (%s)",
          what, (int)st, inst ? GSSK_GetErrorDescription(inst) : "");
    GSSK_Free(inst);
}

/* ---------------------------------------------------------------- */

static void test_each_level_rejects(void) {
    printf("Testing an unknown key is rejected at each of the five levels...\n");
    char json[2048];

    build(json, sizeof(json), ", \"nonsense_top_level_key\": 123",
          EMPTY, EMPTY, EMPTY, EMPTY);
    expect_rejected(json, "root", "nonsense_top_level_key", "top-level");

    /* The exact key from the task: a forcing block a future kernel will read
     * and this one will not. */
    build(json, sizeof(json),
          ", \"forcing\": { \"sun\": { \"waveform\": \"sine\", \"amplitude\": 5 } }",
          EMPTY, EMPTY, EMPTY, EMPTY);
    expect_rejected(json, "root (forcing)", "forcing", "top-level");

    build(json, sizeof(json), EMPTY, ", \"valu\": 3.0", EMPTY, EMPTY, EMPTY);
    expect_rejected(json, "node", "valu", "tank");

    build(json, sizeof(json), EMPTY, EMPTY, ", \"origen\": \"src\"", EMPTY, EMPTY);
    expect_rejected(json, "edge", "origen", "e1");

    build(json, sizeof(json), EMPTY, EMPTY, EMPTY, ", \"bogus_param\": 9", EMPTY);
    expect_rejected(json, "edge params", "bogus_param", "e1");

    build(json, sizeof(json), EMPTY, EMPTY, EMPTY, EMPTY, ", \"dtt\": 0.01");
    expect_rejected(json, "config", "dtt", "config");

    printf("  All five levels reject\n");
}

/* Load-bearing, not a courtesy: examples/household_model_annotated.json and
 * examples/price_dynamics_model.json carry `_note` and `_mechanism` throughout,
 * and the schema documents the convention. Annotations are how a model explains
 * itself, which is most of the point of the Phase G archival story. */
static void test_each_level_accepts_underscore(void) {
    printf("Testing an _-prefixed key is accepted at each of the five levels...\n");
    char json[2048];

    build(json, sizeof(json), ", \"_mechanism\": \"prose\"", EMPTY, EMPTY, EMPTY, EMPTY);
    expect_loads(json, "root _mechanism");

    build(json, sizeof(json), EMPTY, ", \"_note\": \"prose\"", EMPTY, EMPTY, EMPTY);
    expect_loads(json, "node _note");

    build(json, sizeof(json), EMPTY, EMPTY, ", \"_note\": \"prose\"", EMPTY, EMPTY);
    expect_loads(json, "edge _note");

    build(json, sizeof(json), EMPTY, EMPTY, EMPTY, ", \"_why_this_k\": \"prose\"", EMPTY);
    expect_loads(json, "edge params _why_this_k");

    build(json, sizeof(json), EMPTY, EMPTY, EMPTY, EMPTY, ", \"_solver_note\": \"prose\"");
    expect_loads(json, "config _solver_note");

    /* All five at once, and a bare "_". */
    build(json, sizeof(json), ", \"_a\": 1", ", \"_b\": 2", ", \"_c\": 3",
          ", \"_d\": 4", ", \"_\": 5");
    expect_loads(json, "all five levels annotated at once");

    printf("  All five levels accept _-prefixed keys\n");
}

/* The regression that matters. If the accepted sets drift from what the parser
 * reads, this fails by rejecting models that are in fact valid — which is worse
 * than the bug being fixed, because it breaks working models rather than
 * broken ones. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = '\0';
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static void load_dir(const char *dir, int *loaded, int *skipped) {
    DIR *d = opendir(dir);
    if (!d) { printf("  (cannot open %s — skipping)\n", dir); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        char *json = slurp(path);
        if (!json) { printf("  (cannot read %s)\n", path); continue; }

        GSSK_Instance *inst = NULL;
        GSSK_Status st = GSSK_Init(json, &inst);

        /* invalid_model.json exists to fail, and it fails on linkage, not on
         * keys. Anything else that fails on GSSK_ERR_SCHEMA_VIOLATION is this
         * change breaking a model that used to work. */
        if (st == GSSK_SUCCESS) {
            (*loaded)++;
        } else if (st == GSSK_ERR_SCHEMA_VIOLATION &&
                   strstr(GSSK_GetErrorDescription(inst), "unknown key") != NULL) {
            CHECK(0, "%s now fails on an unknown key: %s",
                  path, GSSK_GetErrorDescription(inst));
        } else {
            (*skipped)++;
            printf("  skip %-46s (%s)\n", path,
                   inst ? GSSK_GetErrorDescription(inst) : "no message");
        }
        GSSK_Free(inst);
        free(json);
    }
    closedir(d);
}

static void test_existing_corpora_still_load(void) {
    printf("Testing every existing model still loads...\n");
    int loaded = 0, skipped = 0;
    load_dir("examples", &loaded, &skipped);
    load_dir("tests/schema_fixtures", &loaded, &skipped);
    printf("  %d model(s) loaded, %d skipped for unrelated reasons\n",
           loaded, skipped);
    CHECK(loaded > 0, "no models were loaded — is the working directory wrong?");
}

/* The check that catches the serialiser emitting a key the parser will not
 * take back. It has already caught one: build_topology_json writes
 * "active": false for a deactivated edge, which the published schema did not
 * declare and the accepted set did not contain. */
static void reload_serialized(GSSK_Instance *inst, const char *what) {
    char *model = NULL, *snap = NULL;
    CHECK(GSSK_SerializeModel(inst, &model) == GSSK_SUCCESS,
          "%s: SerializeModel failed", what);
    CHECK(GSSK_SerializeSnapshot(inst, &snap) == GSSK_SUCCESS,
          "%s: SerializeSnapshot failed", what);

    if (model) {
        GSSK_Instance *r = NULL;
        GSSK_Status st = GSSK_Init(model, &r);
        CHECK(st == GSSK_SUCCESS, "%s: serialised MODEL must reload, got %d (%s)",
              what, (int)st, r ? GSSK_GetErrorDescription(r) : "");
        GSSK_Free(r);
        GSSK_FreeString(model);
    }
    if (snap) {
        GSSK_Instance *r = NULL;
        GSSK_Status st = GSSK_Init(snap, &r);
        CHECK(st == GSSK_SUCCESS, "%s: serialised SNAPSHOT must reload, got %d (%s)",
              what, (int)st, r ? GSSK_GetErrorDescription(r) : "");
        GSSK_Free(r);
        GSSK_FreeString(snap);
    }
}

static const char *BASE_MODEL =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"carriers\": [ { \"id\": \"energy\", \"unit\": \"kWh\", \"conserved\": true } ],"
    "  \"nodes\": ["
    "    { \"id\": \"src\",  \"type\": \"source\",  \"value\": 10.0, \"carrier\": \"energy\","
    "      \"quality_input\": 1.0 },"
    "    { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0,  \"carrier\": \"energy\" }"
    "  ],"
    "  \"edges\": ["
    "    { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\", \"carrier\": \"energy\","
    "      \"logic\": \"constant\", \"params\": { \"k\": 0.5 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

static void test_serialized_output_reloads(void) {
    printf("Testing serialised output reloads under the stricter parser...\n");

    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);
    for (int i = 0; i < 10; i++) GSSK_Step(inst, GSSK_GetDt(inst));
    reload_serialized(inst, "stepped model");
    GSSK_Free(inst);

    /* A deactivated edge, which is the case that makes the serialiser emit
     * `active`. No model in examples/ has one, so make test-schema never sees
     * this path — this is the only place it is checked. */
    inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);
    assert(GSSK_DeactivateEdge(inst, "e1") == GSSK_SUCCESS);
    char *model = NULL;
    assert(GSSK_SerializeModel(inst, &model) == GSSK_SUCCESS);
    CHECK(strstr(model, "\"active\"") != NULL,
          "expected the serialiser to emit \"active\" for a deactivated edge; "
          "if it no longer does, drop \"active\" from EDGE_KEYS and the schema");
    GSSK_FreeString(model);
    reload_serialized(inst, "model with a deactivated edge");
    GSSK_Free(inst);

    /* And after a runtime add, so the mutation path's output is covered too. */
    inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);
    assert(GSSK_AddNode(inst, "{\"id\":\"t2\",\"type\":\"storage\",\"value\":1.0}")
           == GSSK_SUCCESS);
    assert(GSSK_AddEdge(inst,
        "{\"id\":\"e2\",\"origin\":\"tank\",\"target\":\"t2\",\"logic\":\"linear\","
        " \"params\":{\"k\":0.2}}") == GSSK_SUCCESS);
    reload_serialized(inst, "model after AddNode/AddEdge");
    GSSK_Free(inst);

    printf("  Serialised output reloads\n");
}

/* ---------------------------------------------------------------- */

/* A rejected add must be a true no-op: this is the call a drag-and-drop editor
 * makes on every element change, so a rejection that half-grew the arrays would
 * corrupt the model the author is still editing. */
static void assert_add_is_noop(int is_node, const char *fragment,
                               const char *what, const char *expect_in_msg) {
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);

    size_t n_before = GSSK_GetStateSize(inst);
    size_t e_before = GSSK_GetEdgeCount(inst);
    double *state_before = malloc(n_before * sizeof(double));
    memcpy(state_before, GSSK_GetState(inst), n_before * sizeof(double));

    GSSK_Status st = is_node ? GSSK_AddNode(inst, fragment)
                             : GSSK_AddEdge(inst, fragment);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "%s: should be GSSK_ERR_SCHEMA_VIOLATION, got %d", what, (int)st);

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
    CHECK(GSSK_Step(inst, GSSK_GetDt(inst)) == GSSK_SUCCESS,
          "%s: GSSK_Step must still succeed after a rejected add", what);

    free(state_before);
    GSSK_Free(inst);
}

static void test_runtime_adds(void) {
    printf("Testing GSSK_AddNode / GSSK_AddEdge reject unknown keys as no-ops...\n");

    assert_add_is_noop(1, "{\"id\":\"x\",\"type\":\"storage\",\"value\":1.0,\"valu\":2.0}",
                       "AddNode unknown key", "valu");
    assert_add_is_noop(0,
        "{\"id\":\"e9\",\"origin\":\"src\",\"target\":\"tank\",\"logic\":\"linear\","
        " \"params\":{\"k\":0.1},\"origen\":\"src\"}",
        "AddEdge unknown key", "origen");
    assert_add_is_noop(0,
        "{\"id\":\"e9\",\"origin\":\"src\",\"target\":\"tank\",\"logic\":\"linear\","
        " \"params\":{\"k\":0.1,\"bogus_param\":9}}",
        "AddEdge unknown param", "bogus_param");

    /* And the annotation convention holds on the runtime paths too. */
    GSSK_Instance *inst = NULL;
    assert(GSSK_Init(BASE_MODEL, &inst) == GSSK_SUCCESS);
    CHECK(GSSK_AddNode(inst,
            "{\"id\":\"x\",\"type\":\"storage\",\"value\":1.0,\"_note\":\"prose\"}")
          == GSSK_SUCCESS, "AddNode must accept an _-prefixed key: %s",
          GSSK_GetErrorDescription(inst));
    CHECK(GSSK_AddEdge(inst,
            "{\"id\":\"e9\",\"origin\":\"src\",\"target\":\"x\",\"logic\":\"linear\","
            " \"params\":{\"k\":0.1,\"_why\":\"prose\"},\"_note\":\"prose\"}")
          == GSSK_SUCCESS, "AddEdge must accept _-prefixed keys: %s",
          GSSK_GetErrorDescription(inst));
    GSSK_Free(inst);

    printf("  Runtime adds OK\n");
}

/* The whole point of the task, end to end: the model from the task description
 * used to run to completion and report success. */
static const char *THE_MODEL_FROM_THE_TASK =
    "{"
    "  \"metadata\": { \"schema_version\": 4 },"
    "  \"forcing\": { \"sun\": { \"waveform\": \"sine\", \"amplitude\": 5, \"period\": 24 } },"
    "  \"nonsense_top_level_key\": 123,"
    "  \"nodes\": ["
    "    { \"id\": \"sun\",   \"type\": \"source\",  \"value\": 1.0 },"
    "    { \"id\": \"plant\", \"type\": \"storage\", \"value\": 0.0 }"
    "  ],"
    "  \"edges\": ["
    "    { \"origin\": \"sun\", \"target\": \"plant\", \"logic\": \"linear\","
    "      \"params\": { \"k\": 0.5, \"bogus_param\": 9 } }"
    "  ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 10, \"dt\": 0.1 }"
    "}";

static void test_the_motivating_model(void) {
    printf("Testing the motivating model now fails at init...\n");
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(THE_MODEL_FROM_THE_TASK, &inst);
    CHECK(st == GSSK_ERR_SCHEMA_VIOLATION,
          "the forcing model must fail at init, got %d", (int)st);
    if (inst) printf("    %s\n", GSSK_GetErrorDescription(inst));
    GSSK_Free(inst);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== Unknown key rejection tests ===\n\n");

    test_each_level_rejects();
    test_each_level_accepts_underscore();
    test_existing_corpora_still_load();
    test_serialized_output_reloads();
    test_runtime_adds();
    test_the_motivating_model();

    printf("\n");
    if (failures) {
        printf("=== FAILED (%d) ===\n", failures);
        return 1;
    }
    printf("=== ALL UNKNOWN KEY TESTS PASSED ===\n");
    return 0;
}
