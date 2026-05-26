/*
 * fuzz_gssk.c — LibFuzzer entry point for GSSK_Init.
 *
 * Build: clang -fsanitize=fuzzer,address -Iinclude src/gssk.c src/advanced.c
 *              src/cJSON.c tests/fuzz_gssk.c -lm -o bin/fuzz_gssk
 *
 * The fuzzer treats arbitrary byte sequences as JSON strings and feeds them to
 * GSSK_Init. We verify three safety properties:
 *   1. GSSK_Init never crashes (any status code is acceptable).
 *   2. GSSK_Init always populates *out_inst (even on error).
 *   3. GSSK_Free never crashes on the returned instance.
 *
 * The corpus directory `tests/fuzz_corpus/` seeds the fuzzer with known-good
 * and known-bad JSON inputs.
 */

#include "gssk.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Construct a NUL-terminated C string from the fuzz input. */
    char *json = malloc(size + 1);
    if (!json) return 0;
    memcpy(json, data, size);
    json[size] = '\0';

    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    (void)st;

    /* Property 2: out_inst must always be populated. */
    if (inst == NULL) {
        free(json);
        __builtin_trap(); /* signal fuzzer: violation */
    }

    /* If init succeeded, take one step and free. */
    if (st == GSSK_SUCCESS) {
        double dt = GSSK_GetDt(inst);
        if (dt <= 0.0 || dt > 1e6) dt = 0.1;
        GSSK_Step(inst, dt);
    }

    /* Property 3: GSSK_Free must not crash. */
    GSSK_Free(inst);
    free(json);
    return 0;
}
