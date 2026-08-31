/* Phase D.1 — money as a closed, conserved GNP loop (Odum Fig. 3).
 *
 * Odum draws two circuits that are not symmetric. Energy enters from a source,
 * does work, and leaves as degraded heat: it flows THROUGH. Money does not
 * leave — it goes ROUND, counter-current to the energy, from the structure
 * back to the sources that supplied it. Figure 3 is that loop.
 *
 * The C.4 countercurrent modelled only half of it. `buyer` was a source and
 * `spent` a sink, so money drained one way and the `"conserved": true`
 * declaration on the money carrier asserted NOTHING — see
 * test_open_loop_conservation_is_vacuous below, which is the reason this task
 * exists. Conservation is measured over storage nodes only
 * (update_carrier_conservation_errors in src/gssk.c), and a source/sink pair
 * contributes no storage at all: the sum is 0 before and 0 after, so the error
 * is 0/1e-15 = 0. A passing check that cannot fail is worse than no check.
 *
 * What is asserted here:
 *
 *   1. money's per-step conservation error stays within solver_tolerance for
 *      the whole run — a real invariant over a closed circuit
 *   2. energy's does NOT, over the very same run, because it dissipates
 *   3. the money actually moves, so (1) is not the trivial conservation of a
 *      stock that never leaves its node
 *   4. the model contains no money source and no money sink, which is the
 *      structural fact that makes (1) mean something
 *   5. the open-loop predecessor reports a perfect 0.0 while containing no
 *      money storage whatsoever — the negative control
 *
 * (2) and (5) are what stop this from being a test that passes for the wrong
 * reason. Without (2) "money is conserved" might just mean the solver is
 * conserving everything; without (5) it might mean the old model was fine.
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

/* Mirrors "solver_tolerance" in examples/odum_gnp_loop.json. Named in both
 * places because the kernel exposes no public getter for it — if you change
 * one, change the other. */
#define SOLVER_TOLERANCE 1e-6

#define MAXS 8192

static size_t ns;
static double t_[MAXS];
static double money_err_[MAXS];   /* GSSK_GetCarrierConservationError("money")  */
static double energy_err_[MAXS];  /* ... and the same for "energy"              */
static double M_total_[MAXS];     /* households + firms                         */
static double E_storage_[MAXS];   /* fuel + yield + structure                   */
static double firms_[MAXS];

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static GSSK_Instance *load(const char *path) {
    char *json = slurp(path);
    if (!json) { printf("  FATAL: cannot read %s\n", path); exit(1); }
    GSSK_Instance *inst = NULL;
    GSSK_Status st = GSSK_Init(json, &inst);
    if (st != GSSK_SUCCESS) {
        printf("  FATAL: %s failed to load: %d (%s)\n", path, (int)st,
               inst ? GSSK_GetErrorDescription(inst) : "");
        exit(1);
    }
    free(json);
    return inst;
}

static int idx_of(GSSK_Instance *inst, const char *id) {
    int i = GSSK_FindNodeIdx(inst, id);
    if (i < 0) { printf("  FATAL: no node '%s'\n", id); exit(1); }
    return i;
}

static int carrier_of(GSSK_Instance *inst, const char *id) {
    int i = GSSK_FindCarrierIdx(inst, id);
    if (i < 0) { printf("  FATAL: no carrier '%s'\n", id); exit(1); }
    return i;
}

static void run_model(void) {
    GSSK_Instance *inst = load("examples/odum_gnp_loop.json");

    int households = idx_of(inst, "households");
    int firms      = idx_of(inst, "firms");
    int fuel       = idx_of(inst, "fuel");
    int yield      = idx_of(inst, "yield");
    int structure  = idx_of(inst, "structure");
    int c_money    = carrier_of(inst, "money");
    int c_energy   = carrier_of(inst, "energy");

    double dt = GSSK_GetDt(inst), t_end = GSSK_GetTEnd(inst);
    ns = 0;
    while (GSSK_GetCurrentTime(inst) < t_end - 1e-12 && ns < MAXS) {
        if (GSSK_Step(inst, dt) != GSSK_SUCCESS) {
            printf("  FATAL: step failed at t=%g\n", GSSK_GetCurrentTime(inst));
            exit(1);
        }
        /* Read AFTER the step: the errors describe the step just taken. */
        const double *s = GSSK_GetState(inst);
        t_[ns]         = GSSK_GetCurrentTime(inst);
        money_err_[ns] = GSSK_GetCarrierConservationError(inst, (size_t)c_money);
        energy_err_[ns]= GSSK_GetCarrierConservationError(inst, (size_t)c_energy);
        M_total_[ns]   = s[households] + s[firms];
        E_storage_[ns] = s[fuel] + s[yield] + s[structure];
        firms_[ns]     = s[firms];
        ns++;
    }
    GSSK_Free(inst);
    printf("  ran %zu steps to t=%.1f\n", ns, t_[ns - 1]);
}

/* ── 1. money is conserved ─────────────────────────────────────────────── */

static void test_money_conservation_error_within_tolerance(void) {
    printf("Money conservation error stays within solver_tolerance...\n");

    double worst = 0.0;
    size_t worst_i = 0;
    for (size_t i = 0; i < ns; i++) {
        if (money_err_[i] > worst) { worst = money_err_[i]; worst_i = i; }
    }
    CHECK(worst < SOLVER_TOLERANCE,
          "money conservation error %.3e at t=%.2f exceeds solver_tolerance %.1e",
          worst, t_[worst_i], SOLVER_TOLERANCE);
    printf("    worst per-step money error: %.3e (tolerance %.1e)\n", worst, SOLVER_TOLERANCE);

    /* The per-step error can be small while the total still drifts over
     * thousands of steps, so pin the invariant end-to-end as well. */
    double drift = 0.0;
    for (size_t i = 0; i < ns; i++) {
        double d = fabs(M_total_[i] - M_total_[0]) / fabs(M_total_[0]);
        if (d > drift) drift = d;
    }
    CHECK(drift < SOLVER_TOLERANCE,
          "total money drifted %.3e from its initial value across the run", drift);
    printf("    M_total: %.12f -> %.12f (max relative drift %.3e)\n",
           M_total_[0], M_total_[ns - 1], drift);
}

/* ── 2. energy is not ──────────────────────────────────────────────────── */

static void test_energy_is_not_conserved_over_the_same_run(void) {
    printf("Energy is NOT conserved over the same run...\n");

    double worst_energy = 0.0;
    for (size_t i = 0; i < ns; i++)
        if (energy_err_[i] > worst_energy) worst_energy = energy_err_[i];

    CHECK(worst_energy > SOLVER_TOLERANCE,
          "energy conservation error never exceeded %.1e (max %.3e) — the model "
          "is not dissipating, so the asymmetry under test is absent",
          SOLVER_TOLERANCE, worst_energy);

    /* Direction matters: energy must LEAVE, not merely wobble. */
    CHECK(E_storage_[ns - 1] < E_storage_[0] * 0.5,
          "energy in storage went %.3f -> %.3f; expected it to at least halve as "
          "the reserve depletes to heat", E_storage_[0], E_storage_[ns - 1]);

    printf("    worst per-step energy error: %.3e (vs money %.3e)\n",
           worst_energy, money_err_[0]);
    printf("    energy in storage: %.3f -> %.3f\n", E_storage_[0], E_storage_[ns - 1]);
}

/* ── 3. the loop actually turns ────────────────────────────────────────── */

static void test_money_actually_circulates(void) {
    printf("Money actually circulates (conservation is not trivial)...\n");

    /* firms starts empty. If it never fills, no payment ever cleared and
     * "money is conserved" would only be saying that nothing happened. */
    double peak_firms = 0.0;
    for (size_t i = 0; i < ns; i++) if (firms_[i] > peak_firms) peak_firms = firms_[i];

    CHECK(peak_firms > 0.01 * M_total_[0],
          "firms peaked at %.6f, under 1%% of the %.6f money stock — the exchange "
          "never cleared and the conservation result is vacuous",
          peak_firms, M_total_[0]);
    printf("    firms' balance peaked at %.6f of %.6f total\n", peak_firms, M_total_[0]);
}

/* ── 4. no money enters or leaves by construction ──────────────────────── */

static void test_no_money_source_or_sink(void) {
    printf("Model contains no money source and no money sink...\n");

    GSSK_Instance *inst = load("examples/odum_gnp_loop.json");
    size_t n = GSSK_GetStateSize(inst);
    size_t money_storages = 0;

    for (size_t i = 0; i < n; i++) {
        if (strcmp(GSSK_GetNodeCarrier(inst, i), "money") != 0) continue;
        const char *type = GSSK_GetNodeTypeString(inst, i);
        const char *id   = GSSK_GetNodeID(inst, i);
        CHECK(strcmp(type, "source") != 0, "money source '%s' would inject money", id);
        CHECK(strcmp(type, "sink")   != 0, "money sink '%s' would drain money", id);
        if (strcmp(type, "storage") == 0) money_storages++;
    }
    CHECK(money_storages >= 2,
          "expected at least two money storages to form a loop, found %zu", money_storages);
    printf("    %zu money storages, no money source, no money sink\n", money_storages);

    GSSK_Free(inst);
}

/* ── 5. negative control: the open-loop predecessor ────────────────────── */

static void test_open_loop_conservation_is_vacuous(void) {
    printf("Negative control — C.4's open money path reports 0.0 with no storage...\n");

    GSSK_Instance *inst = load("examples/odum_countercurrent.json");
    int c_money = carrier_of(inst, "money");
    double dt = GSSK_GetDt(inst);

    for (int i = 0; i < 100; i++) {
        if (GSSK_Step(inst, dt) != GSSK_SUCCESS) {
            printf("  FATAL: control model step failed\n");
            exit(1);
        }
    }

    size_t n = GSSK_GetStateSize(inst);
    size_t money_storages = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(GSSK_GetNodeCarrier(inst, i), "money") == 0 &&
            strcmp(GSSK_GetNodeTypeString(inst, i), "storage") == 0)
            money_storages++;

    double err = GSSK_GetCarrierConservationError(inst, (size_t)c_money);

    /* Both at once is the point: a perfect score computed over nothing. */
    CHECK(money_storages == 0,
          "the C.4 model has gained %zu money storage(s); this control no longer "
          "demonstrates the vacuous case and should be rewritten", money_storages);
    CHECK(err == 0.0,
          "expected a trivially perfect 0.0 from the storage-free path, got %.3e", err);
    printf("    C.4 money: %zu storages, reported error %.1f — perfect and meaningless\n",
           money_storages, err);

    GSSK_Free(inst);
}

int main(void) {
    printf("=== GNP loop / carrier conservation tests (Phase D.1) ===\n\n");
    run_model();
    printf("\n");

    test_money_conservation_error_within_tolerance();
    test_energy_is_not_conserved_over_the_same_run();
    test_money_actually_circulates();
    test_no_money_source_or_sink();
    test_open_loop_conservation_is_vacuous();

    printf("\n");
    if (failures) { printf("=== FAILED (%d) ===\n", failures); return 1; }
    printf("=== ALL GNP-LOOP TESTS PASSED ===\n");
    return 0;
}
