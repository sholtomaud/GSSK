/* Dump what the kernel actually emits, so the schema can be checked against
 * it.
 *
 * scripts/validate_models.py validates the hand-written models in examples/.
 * That catches a schema that rejects what a human writes, but not a schema
 * that rejects what GSSK_SerializeModel and GSSK_SerializeSnapshot produce —
 * and serialised output is what the archival story in Phase G depends on.
 * This tool loads a model, steps it, and writes both serialisations to
 * <out_dir>/<name>.model.json and <out_dir>/<name>.snapshot.json for the
 * validator to pick up.
 *
 * Usage: dump_serialized <out_dir> <model.json>...
 *
 * A model that fails GSSK_Init is reported and skipped, not fatal:
 * examples/invalid_model.json is deliberately unloadable. Exit is non-zero
 * only if a model loads but cannot be serialised, or nothing could be
 * written at all. */

#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static int write_file(const char *dir, const char *stem, const char *suffix,
                      const char *json) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s%s", dir, stem, suffix);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "  cannot write %s\n", path); return 0; }
    fputs(json, f);
    fclose(f);
    return 1;
}

/* "examples/simple_model.json" -> "simple_model" */
static void stem_of(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n > 5 && strcmp(base + n - 5, ".json") == 0) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: dump_serialized <out_dir> <model.json>...\n");
        return 2;
    }
    const char *out_dir = argv[1];
    int written = 0, failed = 0;

    for (int i = 2; i < argc; i++) {
        const char *path = argv[i];
        char stem[256];
        stem_of(path, stem, sizeof(stem));

        char *data = read_file(path);
        if (!data) { fprintf(stderr, "  cannot read %s\n", path); failed++; continue; }

        GSSK_Instance *inst = NULL;
        GSSK_Status st = GSSK_Init(data, &inst);
        free(data);
        if (st != GSSK_SUCCESS || !inst) {
            printf("  skip %-40s (does not load: %s)\n", path,
                   inst ? GSSK_GetErrorDescription(inst) : "init failed");
            if (inst) GSSK_Free(inst);
            continue;
        }

        /* Step so the snapshot carries a non-initial state: t, step count and
         * any adjusted edge k are then real values, not defaults. A step that
         * diverges is not this tool's problem — the regression suite owns
         * that — so the loop stops and serialises whatever state it reached. */
        double dt = GSSK_GetDt(inst);
        for (int s = 0; s < 10; s++)
            if (GSSK_Step(inst, dt) != GSSK_SUCCESS) break;

        char *model = NULL, *snap = NULL;
        if (GSSK_SerializeModel(inst, &model) != GSSK_SUCCESS || !model) {
            fprintf(stderr, "  FAIL %s: GSSK_SerializeModel\n", path);
            failed++;
        } else if (write_file(out_dir, stem, ".model.json", model)) {
            written++;
        } else {
            failed++;
        }
        if (GSSK_SerializeSnapshot(inst, &snap) != GSSK_SUCCESS || !snap) {
            fprintf(stderr, "  FAIL %s: GSSK_SerializeSnapshot\n", path);
            failed++;
        } else if (write_file(out_dir, stem, ".snapshot.json", snap)) {
            written++;
        } else {
            failed++;
        }

        GSSK_FreeString(model);
        GSSK_FreeString(snap);
        GSSK_Free(inst);
    }

    printf("  serialised %d file(s) into %s\n", written, out_dir);
    if (failed) return 1;
    if (written == 0) {
        fprintf(stderr, "  nothing serialised — the check would be vacuous\n");
        return 1;
    }
    return 0;
}
