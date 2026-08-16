#include "gssk.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Helper Functions ---

/* All randomness routes through the instance PRNG (see GSSK_SetSeed).  Using
 * libc rand() here would make results depend on process-global state that any
 * other instance — or the host application — could reset underneath us. */
static double get_random_double(GSSK_Instance *inst, double min, double max) {
  return GSSK_NextRandomUniform(inst, min, max);
}

/* Unbiased index in [0, n) via rejection sampling; the modulo of a 64-bit draw
 * would skew toward low indices. */
static int rand_index(GSSK_Instance *inst, int n) {
  uint64_t bound = (uint64_t)n;
  uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
  uint64_t r;
  do { r = GSSK_NextRandom(inst); } while (r >= limit);
  return (int)(r % bound);
}

static double interpolate(double t, double t1, double v1, double t2, double v2) {
  if (fabs(t2 - t1) < 1e-9)
    return v1;
  double alpha = (t - t1) / (t2 - t1);
  return v1 + alpha * (v2 - v1);
}

// --- Ensemble Forecasting ---

void GSSK_FreeEnsembleResult(GSSK_EnsembleResult *res) {
  if (res) {
    free(res->min_envelope);
    free(res->max_envelope);
    free(res->mean_envelope);
    free(res);
  }
}

GSSK_EnsembleResult *GSSK_EnsembleForecast(GSSK_Instance *inst, size_t runs,
                                           double perturbation) {
  if (!inst || runs == 0)
    return NULL;

  size_t node_count = GSSK_GetStateSize(inst);
  double t_start = GSSK_GetTStart(inst);
  double t_end = GSSK_GetTEnd(inst);
  double dt = GSSK_GetDt(inst);
  size_t step_count = (size_t)((t_end - t_start) / dt) + 1;

  GSSK_EnsembleResult *res = calloc(1, sizeof(GSSK_EnsembleResult));
  if (!res)
    return NULL;

  res->node_count = node_count;
  res->step_count = step_count;
  res->min_envelope = malloc(node_count * step_count * sizeof(double));
  res->max_envelope = malloc(node_count * step_count * sizeof(double));
  res->mean_envelope = malloc(node_count * step_count * sizeof(double));

  if (!res->min_envelope || !res->max_envelope || !res->mean_envelope) {
    GSSK_FreeEnsembleResult(res);
    return NULL;
  }

  // Initialize envelopes
  for (size_t i = 0; i < node_count * step_count; i++) {
    res->min_envelope[i] = INFINITY;
    res->max_envelope[i] = -INFINITY;
    res->mean_envelope[i] = 0.0;
  }

  size_t edge_count = GSSK_GetEdgeCount(inst);
  double *original_ks = malloc(edge_count * sizeof(double));
  for (size_t i = 0; i < edge_count; i++) {
    original_ks[i] = GSSK_GetEdgeK(inst, i);
  }

  for (size_t r = 0; r < runs; r++) {
    // Perturb parameters
    for (size_t i = 0; i < edge_count; i++) {
      double p = get_random_double(inst, 1.0 - perturbation, 1.0 + perturbation);
      GSSK_SetEdgeK(inst, i, original_ks[i] * p);
    }

    GSSK_Reset(inst);
    for (size_t s = 0; s < step_count; s++) {
      const double *state = GSSK_GetState(inst);
      for (size_t n = 0; n < node_count; n++) {
        double val = state[n];
        size_t idx = s * node_count + n;
        if (val < res->min_envelope[idx])
          res->min_envelope[idx] = val;
        if (val > res->max_envelope[idx])
          res->max_envelope[idx] = val;
        res->mean_envelope[idx] += val;
      }
      GSSK_Step(inst, dt);
    }
  }

  // Finalize mean
  for (size_t i = 0; i < node_count * step_count; i++) {
    res->mean_envelope[i] /= (double)runs;
  }

  // Restore original parameters
  for (size_t i = 0; i < edge_count; i++) {
    GSSK_SetEdgeK(inst, i, original_ks[i]);
  }
  free(original_ks);

  return res;
}

// --- Parameter Calibration ---

typedef struct {
  GSSK_Instance *inst;
  GSSK_NodeObservations *obs;
  size_t obs_count;
  int *node_indices;
  size_t param_count;
  double *best_params;
  double best_fitness;
} OptimizerContext;

static double calculate_fitness(OptimizerContext *ctx, double *params) {
  for (size_t i = 0; i < ctx->param_count; i++) {
    GSSK_SetEdgeK(ctx->inst, i, params[i]);
  }

  GSSK_Reset(ctx->inst);
  double t_start = GSSK_GetTStart(ctx->inst);
  double t_end = GSSK_GetTEnd(ctx->inst);
  double dt = GSSK_GetDt(ctx->inst);

  double total_mse = 0.0;
  size_t total_points = 0;

  double t = t_start;
  double *prev_state = malloc(GSSK_GetStateSize(ctx->inst) * sizeof(double));
  memcpy(prev_state, GSSK_GetState(ctx->inst),
         GSSK_GetStateSize(ctx->inst) * sizeof(double));
  double prev_t = t;

  while (t <= t_end + (dt * 0.01)) {
    // Check observations for this time window [prev_t, t]
    for (size_t o = 0; o < ctx->obs_count; o++) {
      int node_idx = ctx->node_indices[o];
      if (node_idx == -1)
        continue;

      for (size_t i = 0; i < ctx->obs[o].count; i++) {
        double obs_t = ctx->obs[o].data[i].time;
        if (obs_t > prev_t && obs_t <= t) {
          double sim_val = interpolate(obs_t, prev_t, prev_state[node_idx], t,
                                       GSSK_GetState(ctx->inst)[node_idx]);
          double diff = sim_val - ctx->obs[o].data[i].value;
          total_mse += diff * diff;
          total_points++;
        }
      }
    }

    if (t >= t_end)
      break;

    memcpy(prev_state, GSSK_GetState(ctx->inst),
           GSSK_GetStateSize(ctx->inst) * sizeof(double));
    prev_t = t;
    if (GSSK_Step(ctx->inst, dt) != GSSK_SUCCESS)
      break;
    t += dt;
  }

  free(prev_state);
  return total_points > 0 ? total_mse / total_points : INFINITY;
}

/* Renamed: original differential-evolution calibration kept as Monte-Carlo path. */
GSSK_Status GSSK_CalibrateMonteCarlo(GSSK_Instance *inst,
                                      GSSK_NodeObservations *obs,
                                      size_t obs_count, int iterations) {
  if (!inst || !obs || obs_count == 0)
    return GSSK_ERR_UNKNOWN;

  OptimizerContext ctx;
  ctx.inst = inst;
  ctx.obs = obs;
  ctx.obs_count = obs_count;
  ctx.node_indices = malloc(obs_count * sizeof(int));
  for (size_t i = 0; i < obs_count; i++) {
    ctx.node_indices[i] = GSSK_FindNodeIdx(inst, obs[i].node_id);
  }

  ctx.param_count = GSSK_GetEdgeCount(inst);
  if (ctx.param_count == 0) {
    free(ctx.node_indices);
    return GSSK_SUCCESS;
  }

  // Differential Evolution Parameters
  const int pop_size = 20;
  const double F = 0.8;
  const double CR = 0.9;

  double *population = malloc(pop_size * ctx.param_count * sizeof(double));
  double *fitness = malloc(pop_size * sizeof(double));
  double *best_params = malloc(ctx.param_count * sizeof(double));
  double best_fitness = INFINITY;

  // Initialize Population
  for (int i = 0; i < pop_size; i++) {
    for (size_t j = 0; j < ctx.param_count; j++) {
      // Assuming k is in range [0, 10] for now as a heuristic
      population[i * ctx.param_count + j] = get_random_double(inst, 0.0, 10.0);
    }
    fitness[i] = calculate_fitness(&ctx, &population[i * ctx.param_count]);
    if (fitness[i] < best_fitness) {
      best_fitness = fitness[i];
      memcpy(best_params, &population[i * ctx.param_count],
             ctx.param_count * sizeof(double));
    }
  }

  // DE Main Loop
  for (int iter = 0; iter < iterations; iter++) {
    for (int i = 0; i < pop_size; i++) {
      // Mutation
      int a, b, c;
      do { a = rand_index(inst, pop_size); } while (a == i);
      do { b = rand_index(inst, pop_size); } while (b == i || b == a);
      do { c = rand_index(inst, pop_size); } while (c == i || c == a || c == b);

      double *trial = malloc(ctx.param_count * sizeof(double));
      int R = rand_index(inst, (int)ctx.param_count);
      for (size_t j = 0; j < ctx.param_count; j++) {
        if (get_random_double(inst, 0, 1) < CR || j == (size_t)R) {
          trial[j] = population[a * ctx.param_count + j] +
                     F * (population[b * ctx.param_count + j] -
                          population[c * ctx.param_count + j]);
          if (trial[j] < 0) trial[j] = 0; // Boundary constraint
        } else {
          trial[j] = population[i * ctx.param_count + j];
        }
      }

      double trial_fitness = calculate_fitness(&ctx, trial);
      if (trial_fitness <= fitness[i]) {
        fitness[i] = trial_fitness;
        memcpy(&population[i * ctx.param_count], trial,
               ctx.param_count * sizeof(double));
        if (trial_fitness < best_fitness) {
          best_fitness = trial_fitness;
          memcpy(best_params, trial, ctx.param_count * sizeof(double));
        }
      }
      free(trial);
    }
  }

  // Set best parameters back to instance
  for (size_t i = 0; i < ctx.param_count; i++) {
    GSSK_SetEdgeK(inst, i, best_params[i]);
  }

  free(population);
  free(fitness);
  free(best_params);
  free(ctx.node_indices);

  return GSSK_SUCCESS;
}

/* Backward-compatibility shim: GSSK_Calibrate → Monte-Carlo path. */
GSSK_Status GSSK_Calibrate(GSSK_Instance *inst, GSSK_NodeObservations *obs,
                            size_t obs_count, int iterations) {
  return GSSK_CalibrateMonteCarlo(inst, obs, obs_count, iterations);
}

/* =========================================================================
 * Phase 3.4 — Gradient-based calibration (Levenberg-Marquardt + forward sens)
 *
 * Builds a Jacobian J[n_obs × m] by running a forward simulation with the
 * Phase 3 forward-sensitivity augmented ODE, then solves one L-M update:
 *   (JᵀJ + λ·diag(JᵀJ+ε))·Δk = −Jᵀr
 * Repeats for `iterations` outer loops.  Accepts the step if the MSE
 * objective decreases; otherwise increases damping and retries from the
 * best-known parameters.
 * ========================================================================= */

GSSK_Status GSSK_CalibrateGradient(GSSK_Instance *inst,
                                    GSSK_NodeObservations *obs,
                                    size_t obs_count,
                                    const size_t *param_edge_indices,
                                    size_t param_count,
                                    int iterations) {
  if (!inst || !obs || obs_count == 0 || !param_edge_indices ||
      param_count == 0 || iterations <= 0)
    return GSSK_ERR_UNKNOWN;

  size_t m     = param_count;
  double dt    = GSSK_GetDt(inst);
  double t0    = GSSK_GetTStart(inst);
  double t_end = GSSK_GetTEnd(inst);

  /* Pre-resolve node indices */
  int *node_idx = malloc(obs_count * sizeof(int));
  if (!node_idx) return GSSK_ERR_MALLOC_FAILED;
  size_t total_obs = 0;
  for (size_t o = 0; o < obs_count; o++) {
    node_idx[o] = GSSK_FindNodeIdx(inst, obs[o].node_id);
    total_obs += obs[o].count;
  }
  if (total_obs == 0) { free(node_idx); return GSSK_SUCCESS; }

  /* Save and track best k values */
  double *k_best = malloc(m * sizeof(double));
  double *k_cur  = malloc(m * sizeof(double));
  double *J_lm   = calloc(total_obs * m, sizeof(double));
  double *r_lm   = calloc(total_obs,     sizeof(double));
  double *JtJ    = malloc(m * m * sizeof(double));
  double *Jtr    = malloc(m * sizeof(double));
  double *delta  = malloc(m * sizeof(double));
  if (!k_best || !k_cur || !J_lm || !r_lm || !JtJ || !Jtr || !delta) {
    free(node_idx); free(k_best); free(k_cur);
    free(J_lm); free(r_lm); free(JtJ); free(Jtr); free(delta);
    return GSSK_ERR_MALLOC_FAILED;
  }

  for (size_t j = 0; j < m; j++)
    k_best[j] = k_cur[j] = GSSK_GetEdgeK(inst, param_edge_indices[j]);

  double lambda    = 1e-3;
  double obj_best  = 1e300;
  int    retry_max = 4;

  for (int iter = 0; iter < iterations; iter++) {
    /* Apply current parameters */
    for (size_t j = 0; j < m; j++)
      GSSK_SetEdgeK(inst, param_edge_indices[j], k_cur[j]);

    /* Enable forward sensitivity for all param edges */
    GSSK_EnableForwardSensitivity(inst, param_edge_indices, m);
    GSSK_Reset(inst);

    memset(J_lm, 0, total_obs * m * sizeof(double));
    memset(r_lm, 0, total_obs * sizeof(double));
    size_t obs_ptr = 0;

    double t = t0;
    while (t <= t_end + dt * 0.5) {
      /* Match observations near current t */
      for (size_t o = 0; o < obs_count; o++) {
        int ni = node_idx[o];
        if (ni < 0) continue;
        for (size_t i = 0; i < obs[o].count; i++) {
          if (fabs(obs[o].data[i].time - t) <= dt * 0.5 &&
              obs_ptr < total_obs) {
            r_lm[obs_ptr] = GSSK_GetState(inst)[ni] - obs[o].data[i].value;
            for (size_t j = 0; j < m; j++)
              J_lm[obs_ptr * m + j] = GSSK_GetSensitivity(inst, (size_t)ni, j);
            obs_ptr++;
          }
        }
      }
      if (t >= t_end) break;
      GSSK_Step(inst, dt);
      t += dt;
    }
    GSSK_DisableForwardSensitivity(inst);

    /* Compute MSE objective */
    double obj = 0.0;
    for (size_t i = 0; i < total_obs; i++) obj += r_lm[i] * r_lm[i];
    obj = total_obs > 0 ? obj / (double)total_obs : 0.0;

    /* L-M update with retry on no improvement */
    bool accepted = false;
    for (int retry = 0; retry < retry_max && !accepted; retry++) {
      /* Build JᵀJ and Jᵀr */
      memset(JtJ, 0, m * m * sizeof(double));
      memset(Jtr, 0, m * sizeof(double));
      for (size_t i = 0; i < total_obs; i++) {
        for (size_t p = 0; p < m; p++) {
          Jtr[p] += J_lm[i * m + p] * r_lm[i];
          for (size_t q = 0; q < m; q++)
            JtJ[p * m + q] += J_lm[i * m + p] * J_lm[i * m + q];
        }
      }
      /* L-M damping: JᵀJ += λ·(diag(JᵀJ) + ε) */
      for (size_t j = 0; j < m; j++)
        JtJ[j * m + j] += lambda * (JtJ[j * m + j] + 1e-8);

      /* Solve (JᵀJ)·Δk = −Jᵀr using a copy of JtJ */
      double *JtJ_copy = malloc(m * m * sizeof(double));
      if (!JtJ_copy) break;
      memcpy(JtJ_copy, JtJ, m * m * sizeof(double));
      for (size_t j = 0; j < m; j++) delta[j] = -Jtr[j];
      bool ok = false;
      /* gaussian_solve is static in gssk.c — use a simple inline solve here */
      /* We implement partial-pivot Gauss for m×m (typically small) */
      for (size_t col = 0; col < m; col++) {
        size_t piv = col;
        double pval = fabs(JtJ_copy[col * m + col]);
        for (size_t row = col + 1; row < m; row++) {
          double v = fabs(JtJ_copy[row * m + col]);
          if (v > pval) { pval = v; piv = row; }
        }
        if (pval < 1e-14) { ok = false; break; }
        if (piv != col) {
          for (size_t j = 0; j < m; j++) {
            double tmp = JtJ_copy[col * m + j];
            JtJ_copy[col * m + j] = JtJ_copy[piv * m + j];
            JtJ_copy[piv * m + j] = tmp;
          }
          double tmp = delta[col]; delta[col] = delta[piv]; delta[piv] = tmp;
        }
        for (size_t row = col + 1; row < m; row++) {
          double fac = JtJ_copy[row * m + col] / JtJ_copy[col * m + col];
          for (size_t j = col; j < m; j++)
            JtJ_copy[row * m + j] -= fac * JtJ_copy[col * m + j];
          delta[row] -= fac * delta[col];
        }
        ok = true;
      }
      if (ok) {
        for (int i = (int)m - 1; i >= 0; i--) {
          double s = delta[i];
          for (size_t j = (size_t)(i + 1); j < m; j++)
            s -= JtJ_copy[(size_t)i * m + j] * delta[j];
          delta[i] = s / JtJ_copy[(size_t)i * m + (size_t)i];
        }
      }
      free(JtJ_copy);
      if (!ok) { lambda *= 4.0; continue; }

      /* Trial k = k_best + Δk, clamped to [0, ∞) */
      for (size_t j = 0; j < m; j++) {
        double kc = k_best[j] + delta[j];
        k_cur[j] = kc > 0.0 ? kc : 0.0;
      }

      if (obj < obj_best) {
        obj_best = obj;
        memcpy(k_best, k_cur, m * sizeof(double));
        lambda *= 0.5;
        accepted = true;
      } else {
        lambda *= 4.0;
        memcpy(k_cur, k_best, m * sizeof(double));
      }
    }
    if (!accepted) memcpy(k_cur, k_best, m * sizeof(double));
  }

  /* Apply best parameters */
  for (size_t j = 0; j < m; j++)
    GSSK_SetEdgeK(inst, param_edge_indices[j], k_best[j]);
  GSSK_Reset(inst);

  free(node_idx); free(k_best); free(k_cur);
  free(J_lm); free(r_lm); free(JtJ); free(Jtr); free(delta);
  return GSSK_SUCCESS;
}
