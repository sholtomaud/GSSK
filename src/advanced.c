#include "gssk.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Helper Functions ---

static double get_random_double(double min, double max) {
  return min + ((double)rand() / RAND_MAX) * (max - min);
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

  // srand is handled at higher level or by system in WASM

  for (size_t r = 0; r < runs; r++) {
    // Perturb parameters
    for (size_t i = 0; i < edge_count; i++) {
      double p = get_random_double(1.0 - perturbation, 1.0 + perturbation);
      GSSK_SetEdgeK(inst, i, original_ks[i] * p);
    }

    GSSK_Reset(inst);
    double t = t_start;
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
      t += dt;
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

GSSK_Status GSSK_Calibrate(GSSK_Instance *inst, GSSK_NodeObservations *obs,
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

  // srand handled globally

  // Initialize Population
  for (int i = 0; i < pop_size; i++) {
    for (size_t j = 0; j < ctx.param_count; j++) {
      // Assuming k is in range [0, 10] for now as a heuristic
      population[i * ctx.param_count + j] = get_random_double(0.0, 10.0);
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
      do { a = rand() % pop_size; } while (a == i);
      do { b = rand() % pop_size; } while (b == i || b == a);
      do { c = rand() % pop_size; } while (c == i || c == a || c == b);

      double *trial = malloc(ctx.param_count * sizeof(double));
      int R = rand() % ctx.param_count;
      for (size_t j = 0; j < ctx.param_count; j++) {
        if (get_random_double(0, 1) < CR || j == (size_t)R) {
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
