/* Writes the native forcing evaluator's answers to JSON, for the WASM parity
 * check to compare against. Native is the reference; WASM must match it
 * bit-for-bit, because they are the same C compiled twice.
 *
 * sin() and exp() are the reason this exists: an emscripten build could
 * substitute a different libm, and the divergence would be small enough to
 * look like round-off while being systematic. */
#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *MODEL_TMPL =
    "{ \"metadata\": { \"schema_version\": 4 },"
    "  \"nodes\": [ { \"id\": \"sun\", \"type\": \"source\", \"value\": 0.0, \"forcing\": %s },"
    "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
    "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"sun\", \"target\": \"tank\","
    "                 \"logic\": \"linear\", \"params\": { \"k\": 1.0 } } ],"
    "  \"config\": { \"t_start\": 0, \"t_end\": 20, \"dt\": 0.1 } }";

/* %.17g round-trips a double exactly, which is what "bit-for-bit" needs. */
static void emit_samples(FILE *f, GSSK_Instance *inst, int is_edge) {
    static const double TS[] = { 0.0, 0.37, 1.0, 2.5, 3.14159, 7.0, 11.25, 19.9 };
    fprintf(f, "\"samples\":[");
    for (size_t i = 0; i < sizeof(TS)/sizeof(TS[0]); i++) {
        double v = is_edge ? GSSK_EvaluateEdgeForcing(inst, 0, TS[i])
                           : GSSK_EvaluateNodeForcing(inst, 0, TS[i]);
        fprintf(f, "%s{\"t\":%.17g,\"v\":%.17g}", i ? "," : "", TS[i], v);
    }
    fprintf(f, "]");
}

static void emit_case(FILE *f, const char *name, const char *forcing, int first) {
    char json[2048];
    snprintf(json, sizeof(json), MODEL_TMPL, forcing);
    GSSK_Instance *inst = NULL;
    if (GSSK_Init(json, &inst) != GSSK_SUCCESS) {
        fprintf(stderr, "dump_forcing_native: %s failed to load: %s\n",
                name, inst ? GSSK_GetErrorDescription(inst) : "");
        exit(1);
    }
    fprintf(f, "%s{\"name\":\"%s\",\"kind\":%d,\"model\":", first ? "" : ",",
            name, GSSK_GetNodeForcingKind(inst, 0));
    /* Emit the model as a JSON string so the JS side loads the identical text. */
    fputc('"', f);
    for (const char *p = json; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
    fputc(',', f);
    emit_samples(f, inst, 0);
    fputc('}', f);
    GSSK_Free(inst);
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "tests/results/forcing_native.json";
    FILE *f = fopen(out, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }

    fprintf(f, "{\"cases\":[");
    /* sine and exponential first — they are the reason this check exists. */
    emit_case(f, "sine",        "{\"waveform\":\"sine\",\"mean\":3.5,\"amplitude\":2.25,\"period\":7.0,\"phase\":1.125}", 1);
    emit_case(f, "exponential", "{\"waveform\":\"exponential\",\"t_on\":1.0,\"v0\":1.5,\"rate\":0.13}", 0);
    emit_case(f, "step",        "{\"waveform\":\"step\",\"t_on\":3.0,\"v0\":1.0,\"v1\":4.0}", 0);
    emit_case(f, "impulse",     "{\"waveform\":\"impulse\",\"t_on\":1.0,\"area\":5.0}", 0);
    emit_case(f, "ramp",        "{\"waveform\":\"ramp\",\"t_on\":0.5,\"v0\":1.0,\"slope\":0.75,\"max\":9.0}", 0);
    emit_case(f, "sawtooth",    "{\"waveform\":\"sawtooth\",\"mean\":2.0,\"amplitude\":1.5,\"period\":3.3,\"phase\":0.4}", 0);
    emit_case(f, "square",      "{\"waveform\":\"square\",\"mean\":2.0,\"amplitude\":1.5,\"period\":3.3,\"duty\":0.35}", 0);
    fprintf(f, "],");

    /* Edge attachment — the other half of the vocabulary. */
    {
        const char *EDGE_MODEL =
            "{ \"metadata\": { \"schema_version\": 4 },"
            "  \"nodes\": [ { \"id\": \"src\", \"type\": \"source\", \"value\": 1.0 },"
            "               { \"id\": \"tank\", \"type\": \"storage\", \"value\": 0.0 } ],"
            "  \"edges\": [ { \"id\": \"e1\", \"origin\": \"src\", \"target\": \"tank\","
            "                 \"logic\": \"constant\", \"params\": { \"k\": 99.0 },"
            "                 \"forcing\": { \"waveform\": \"sine\", \"mean\": 0.5,"
            "                                \"amplitude\": 0.25, \"period\": 4.0 } } ],"
            "  \"config\": { \"t_start\": 0, \"t_end\": 20, \"dt\": 0.1 } }";
        GSSK_Instance *inst = NULL;
        if (GSSK_Init(EDGE_MODEL, &inst) != GSSK_SUCCESS) {
            fprintf(stderr, "dump_forcing_native: edge case failed to load\n");
            return 1;
        }
        fprintf(f, "\"edge_case\":{\"kind\":%d,\"model\":", GSSK_GetEdgeForcingKind(inst, 0));
        fputc('"', f);
        for (const char *p = EDGE_MODEL; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            fputc(*p, f);
        }
        fputc('"', f);
        fputc(',', f);
        emit_samples(f, inst, 1);
        fprintf(f, "}");
        GSSK_Free(inst);
    }
    fprintf(f, "}\n");
    fclose(f);
    printf("wrote %s\n", out);
    return 0;
}
