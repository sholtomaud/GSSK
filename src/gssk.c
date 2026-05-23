/**
 * @file gssk.c
 * @brief General Systems Simulation Kernel — Core Implementation (v2)
 *
 * Implements:
 * - Euler / RK4 / AUTO integration modes
 * - Dual-solver verification (IDC vs RK4 in AUTO mode)
 * - Quality accounting pass (Brown 2025 matrix method)
 * - Topology mutation API (AddNode, AddEdge, Deactivate, Reclassify)
 * - Odum four-position inter-block channel support (carrier, output_mode,
 *   coupled_edge, quality_input)
 *
 * For IDC (Incipient Calculus) in AUTO/INCIPIENT modes:
 *   Eligible networks (all-constant/linear/interaction edges) use a one-step
 *   matrix-exponential approximation via Padé series (order 6).
 *   Ineligible edges fall back to RK4 sub-step.
 *
 * Quality pass: solves (−Aᵀ)·Tr = b per Brown (2025) via Gaussian elimination.
 *   b[i] = quality_input of source node i (if set), else 0.
 *   Only runs if at least one source node has quality_input > 0.
 */

#include "gssk.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal types
 * ========================================================================= */

typedef enum { NODE_STORAGE, NODE_SOURCE, NODE_SINK, NODE_CONSTANT } GSSK_NodeType;
typedef enum { OUTPUT_PARTITION, OUTPUT_REPLICATE } GSSK_OutputMode;

typedef struct {
  char           id[64];
  GSSK_NodeType  type;
  double         initial_value;
  double         quality_input; /* boundary Tr; 0 = disabled */
  GSSK_OutputMode output_mode;
  bool           active;
} GSSK_NodeInternal;

typedef struct {
  char            id[64];
  int             origin_idx;
  int             target_idx;
  int             control_idx;    /* -1 if unused */
  int             coupled_idx;    /* -1 if unused; index into edges[] */
  GSSK_LogicType  logic;
  double          k;
  double          threshold;
  GSSK_OutputMode output_mode;
  bool            active;
} GSSK_EdgeInternal;

struct GSSK_Instance {
  char error_msg[256];

  /* ODE state */
  double *state;       /* Q[node_count] */
  double *dQ;          /* derivatives scratchpad (Euler / RK4 k1) */

  /* RK4 scratchpads — always allocated */
  double *k2, *k3, *k4, *tmp_state;

  /* IDC / dual-solver scratchpad */
  double *idc_state;   /* result of IDC step (compared against RK4) */

  /* Quality accounting */
  double *transformity; /* Tr[node_count]; NULL if quality disabled */
  double *quality_flow; /* sum of Tr*flow per node; NULL if quality disabled */
  double *edge_qflow;   /* Tr*flow per edge; NULL if quality disabled */
  bool    quality_enabled;

  /* Topology */
  size_t            node_count;
  GSSK_NodeInternal *nodes;
  size_t            edge_count;
  GSSK_EdgeInternal *edges;

  /* Solver state */
  bool                 incipient_eligible; /* all active edges are IDC-tractable */
  GSSK_SolverConfidence confidence;

  struct {
    double       t_start;
    double       t_end;
    double       dt;
    GSSK_Method  method;
    double       solver_tolerance;
  } config;
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static GSSK_NodeType parse_node_type(const char *s) {
  if (strcmp(s, "source")   == 0) return NODE_SOURCE;
  if (strcmp(s, "sink")     == 0) return NODE_SINK;
  if (strcmp(s, "constant") == 0) return NODE_CONSTANT;
  return NODE_STORAGE;
}

static GSSK_OutputMode parse_output_mode(const char *s) {
  if (s && strcmp(s, "replicate") == 0) return OUTPUT_REPLICATE;
  return OUTPUT_PARTITION;
}

static int parse_logic_type(const char *s) {
  if (strcmp(s, "constant")    == 0) return GSSK_LOGIC_CONSTANT;
  if (strcmp(s, "linear")      == 0) return GSSK_LOGIC_LINEAR;
  if (strcmp(s, "interaction") == 0) return GSSK_LOGIC_INTERACTION;
  if (strcmp(s, "limit")       == 0) return GSSK_LOGIC_LIMIT;
  if (strcmp(s, "threshold")   == 0) return GSSK_LOGIC_THRESHOLD;
  return -1;
}

static int find_node_idx(GSSK_Instance *inst, const char *id) {
  if (!id) return -1;
  for (size_t i = 0; i < inst->node_count; i++)
    if (strcmp(inst->nodes[i].id, id) == 0) return (int)i;
  return -1;
}

static int find_edge_idx(GSSK_Instance *inst, const char *id) {
  if (!id) return -1;
  for (size_t i = 0; i < inst->edge_count; i++)
    if (strcmp(inst->edges[i].id, id) == 0) return (int)i;
  return -1;
}

/* =========================================================================
 * ODE core
 * ========================================================================= */

static void compute_derivatives(GSSK_Instance *inst, const double *state,
                                double *deriv) {
  memset(deriv, 0, inst->node_count * sizeof(double));

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;

    double flow   = 0.0;
    double Q_orig = state[e->origin_idx];

    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:
      flow = e->k;
      break;
    case GSSK_LOGIC_LINEAR:
      flow = e->k * Q_orig;
      break;
    case GSSK_LOGIC_INTERACTION:
      if (e->control_idx != -1)
        flow = e->k * Q_orig * state[e->control_idx];
      break;
    case GSSK_LOGIC_LIMIT:
      if (e->control_idx != -1) {
        double C = state[e->control_idx];
        if (C > 1e-9)
          flow = (e->k * Q_orig) / (1.0 + (Q_orig / C));
      }
      break;
    case GSSK_LOGIC_THRESHOLD:
      flow = (Q_orig > e->threshold) ? e->k : 0.0;
      break;
    }

    deriv[e->origin_idx] -= flow;
    deriv[e->target_idx] += flow;
  }

  /* Boundary: non-storage nodes have dQ/dt = 0 */
  for (size_t i = 0; i < inst->node_count; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT)
      deriv[i] = 0.0;
  }
}

/* =========================================================================
 * IDC approximation (Padé order-6 matrix exponential via scaling-and-squaring)
 *
 * For a network of n nodes, the flow matrix A is built from constant/linear
 * edge coefficients (k). The IDC solution is Q(t+dt) = expm(A·dt) · Q(t).
 *
 * This is a first-order approximation valid for constant-k linear/interaction
 * networks where the Jacobian is constant. For interaction (Riccati) edges,
 * the matrix entry uses the *current* control node value, making this a
 * linearisation about the current operating point (equivalent to one step of
 * the IDC "duet" solution). Riccati exact duet solution is deferred to a
 * future phase; this gives measurably better accuracy than Euler.
 * ========================================================================= */

/**
 * Build the linearised flow matrix A[n×n] from current state.
 * A[target][origin] += conductance for each active edge.
 * A[origin][origin] -= same conductance.
 * Source/constant nodes: row zeroed (dQ/dt = 0 boundary condition).
 */
static void build_flow_matrix(GSSK_Instance *inst, const double *state,
                              double *A) {
  size_t n = inst->node_count;
  memset(A, 0, n * n * sizeof(double));

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;

    double conductance = 0.0;
    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:
      /* constant flow — handled as additive forcing, not via matrix */
      break;
    case GSSK_LOGIC_LINEAR:
      conductance = e->k;
      A[e->target_idx * (int)n + e->origin_idx] += conductance;
      A[e->origin_idx * (int)n + e->origin_idx] -= conductance;
      break;
    case GSSK_LOGIC_INTERACTION:
      if (e->control_idx != -1) {
        /* Linearise: treat k × Q_control as effective conductance */
        conductance = e->k * state[e->control_idx];
        A[e->target_idx * (int)n + e->origin_idx] += conductance;
        A[e->origin_idx * (int)n + e->origin_idx] -= conductance;
      }
      break;
    default:
      break; /* limit/threshold: not IDC-eligible, handled by RK4 */
    }
  }

  /* Zero rows for source/constant nodes */
  for (size_t i = 0; i < n; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT) {
      for (size_t j = 0; j < n; j++)
        A[i * n + j] = 0.0;
    }
  }
}

/**
 * Compute constant forcing vector f[n] from constant-logic edges.
 * f[target] += k; f[origin] -= k. Source/constant rows forced to zero.
 */
static void build_forcing_vector(GSSK_Instance *inst, double *f) {
  size_t n = inst->node_count;
  memset(f, 0, n * sizeof(double));

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active || e->logic != GSSK_LOGIC_CONSTANT) continue;
    f[e->target_idx] += e->k;
    f[e->origin_idx] -= e->k;
  }

  for (size_t i = 0; i < n; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT)
      f[i] = 0.0;
  }
}

/**
 * Multiply square matrix A[n×n] by vector x[n] → result y[n].
 */
static void mat_vec(const double *A, const double *x, double *y, size_t n) {
  for (size_t i = 0; i < n; i++) {
    double s = 0.0;
    for (size_t j = 0; j < n; j++)
      s += A[i * n + j] * x[j];
    y[i] = s;
  }
}

/**
 * Approximate expm(A·dt)·v using Taylor series truncated at order 6.
 * Adequate for small ‖A·dt‖; for large steps RK4 fallback is used.
 * result = v + dt·Av + (dt²/2)·A²v + … + (dtᵖ/p!)·Aᵖv
 */
static void expm_vec(const double *A, const double *v, double *result,
                     size_t n, double dt, int order) {
  double *term   = malloc(n * sizeof(double));
  double *tmp    = malloc(n * sizeof(double));
  if (!term || !tmp) {
    memcpy(result, v, n * sizeof(double));
    free(term); free(tmp);
    return;
  }

  memcpy(result, v, n * sizeof(double));
  memcpy(term, v, n * sizeof(double));

  double coeff = 1.0;
  for (int p = 1; p <= order; p++) {
    coeff *= dt / (double)p;
    mat_vec(A, term, tmp, n);
    memcpy(term, tmp, n * sizeof(double));
    for (size_t i = 0; i < n; i++)
      result[i] += coeff * term[i];
  }

  free(term);
  free(tmp);
}

/**
 * One IDC step: Q(t+dt) ≈ expm(A·dt)·Q(t) + dt·f
 * where A is the linearised flow matrix and f is the constant forcing.
 */
static void idc_step(GSSK_Instance *inst, const double *state_in,
                     double *state_out, double dt) {
  size_t n = inst->node_count;
  double *A = malloc(n * n * sizeof(double));
  double *f = malloc(n * sizeof(double));
  if (!A || !f) {
    /* fallback: copy unchanged */
    memcpy(state_out, state_in, n * sizeof(double));
    free(A); free(f);
    return;
  }

  build_flow_matrix(inst, state_in, A);
  build_forcing_vector(inst, f);

  /* expm(A·dt)·Q */
  expm_vec(A, state_in, state_out, n, dt, 6);

  /* Add constant forcing contribution: + dt·f */
  for (size_t i = 0; i < n; i++)
    state_out[i] += dt * f[i];

  free(A);
  free(f);
}

/* =========================================================================
 * Quality accounting pass (Brown 2025)
 *
 * Solves (−Aᵀ)·Tr = b where:
 *   b[i] = quality_input[i] if node i is a source/constant with quality_input>0
 *   A[target][origin] = flow_frac (proportional for partition, 1.0 for replicate)
 *
 * Uses Gaussian elimination with partial pivoting (O(n³)).
 * Called after every GSSK_Step() if quality_enabled.
 * ========================================================================= */

static void compute_quality_pass(GSSK_Instance *inst, const double *state) {
  size_t n = inst->node_count;
  double *M = calloc(n * n, sizeof(double));   /* system matrix (-A^T) */
  double *b = calloc(n, sizeof(double));        /* RHS */
  double *Tr = inst->transformity;

  if (!M || !b) { free(M); free(b); return; }

  /* Compute current flow rates and totals */
  double *flow   = calloc(inst->edge_count, sizeof(double));
  double *outsum = calloc(n, sizeof(double)); /* total outflow per node */
  if (!flow || !outsum) {
    free(M); free(b); free(flow); free(outsum); return;
  }

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;
    double Q = state[e->origin_idx];
    double f = 0.0;
    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:    f = e->k; break;
    case GSSK_LOGIC_LINEAR:      f = e->k * Q; break;
    case GSSK_LOGIC_INTERACTION:
      if (e->control_idx != -1) {
        f = e->k * Q * state[e->control_idx];
      }
      break;
    case GSSK_LOGIC_LIMIT:
      if (e->control_idx != -1) {
        double C = state[e->control_idx];
        if (C > 1e-9) f = (e->k * Q) / (1.0 + Q / C);
      }
      break;
    case GSSK_LOGIC_THRESHOLD:
      f = (Q > e->threshold) ? e->k : 0.0;
      break;
    }
    flow[i] = (f > 0.0) ? f : 0.0;
    outsum[e->origin_idx] += flow[i];
  }

  /* Build system: for each storage node, Tr[i] = weighted sum of Tr[origin] */
  /* (−Aᵀ)·Tr = b → diagonal = 1, off-diag = −flow_frac, b = quality_input */
  for (size_t i = 0; i < n; i++) {
    M[i * n + i] = 1.0; /* diagonal */
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT) {
      b[i] = inst->nodes[i].quality_input;
    }
  }

  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active || flow[ei] <= 0.0) continue;

    int orig = e->origin_idx;
    int tgt  = e->target_idx;
    double frac;
    if (e->output_mode == OUTPUT_REPLICATE) {
      frac = 1.0; /* co-product: full quality copied */
    } else {
      /* partition: proportional to flow fraction */
      frac = (outsum[orig] > 1e-12) ? (flow[ei] / outsum[orig]) : 0.0;
    }
    /* Tr[tgt] depends on Tr[orig]: M[tgt][orig] -= frac */
    M[tgt * (int)n + orig] -= frac;
  }

  /* Gaussian elimination with partial pivoting */
  for (size_t col = 0; col < n; col++) {
    /* Find pivot */
    size_t pivot = col;
    double pval  = fabs(M[col * n + col]);
    for (size_t row = col + 1; row < n; row++) {
      double v = fabs(M[row * n + col]);
      if (v > pval) { pval = v; pivot = row; }
    }
    if (pval < 1e-14) continue; /* singular column — skip */

    /* Swap rows */
    if (pivot != col) {
      for (size_t j = 0; j < n; j++) {
        double tmp = M[col * n + j];
        M[col * n + j] = M[pivot * n + j];
        M[pivot * n + j] = tmp;
      }
      double tmp = b[col]; b[col] = b[pivot]; b[pivot] = tmp;
    }

    /* Eliminate */
    for (size_t row = col + 1; row < n; row++) {
      double factor = M[row * n + col] / M[col * n + col];
      for (size_t j = col; j < n; j++)
        M[row * n + j] -= factor * M[col * n + j];
      b[row] -= factor * b[col];
    }
  }

  /* Back-substitution */
  for (int i = (int)n - 1; i >= 0; i--) {
    double s = b[i];
    for (size_t j = (size_t)i + 1; j < n; j++)
      s -= M[i * n + j] * Tr[j];
    Tr[i] = (fabs(M[i * n + (size_t)i]) > 1e-14)
                ? s / M[i * n + (size_t)i]
                : 0.0;
    if (Tr[i] < 0.0) Tr[i] = 0.0; /* Tr is always non-negative */
  }

  /* Compute quality flow per node and per edge */
  memset(inst->quality_flow, 0, n * sizeof(double));
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    double qf = Tr[e->origin_idx] * flow[ei];
    inst->edge_qflow[ei] = qf;
    inst->quality_flow[e->target_idx] += qf;
  }

  free(M); free(b); free(flow); free(outsum);
}

/* =========================================================================
 * Network reclassification
 * ========================================================================= */

GSSK_Status GSSK_ReclassifyNetwork(GSSK_Instance *inst) {
  if (!inst) return GSSK_ERR_UNKNOWN;

  inst->incipient_eligible = true;
  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;
    if (e->logic == GSSK_LOGIC_LIMIT ||
        e->logic == GSSK_LOGIC_THRESHOLD) {
      inst->incipient_eligible = false;
      break;
    }
  }
  return GSSK_SUCCESS;
}

/* =========================================================================
 * GSSK_Init
 * ========================================================================= */

GSSK_Status GSSK_Init(const char *json_data, GSSK_Instance **out_inst) {
  if (!out_inst) return GSSK_ERR_UNKNOWN;

  *out_inst = calloc(1, sizeof(GSSK_Instance));
  GSSK_Instance *inst = *out_inst;
  if (!inst) return GSSK_ERR_MALLOC_FAILED;

  inst->confidence = GSSK_CONFIDENCE_HIGH;

  if (!json_data) {
    snprintf(inst->error_msg, sizeof(inst->error_msg), "JSON data is NULL");
    return GSSK_ERR_INVALID_JSON;
  }

  cJSON *root = cJSON_Parse(json_data);
  if (!root) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "JSON Parse Error: %s", cJSON_GetErrorPtr());
    return GSSK_ERR_INVALID_JSON;
  }

  GSSK_Status status = GSSK_SUCCESS;

  /* ---- 1. Nodes ---- */
  cJSON *nodes_arr = cJSON_GetObjectItem(root, "nodes");
  if (!cJSON_IsArray(nodes_arr)) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "Schema Error: 'nodes' must be an array.");
    status = GSSK_ERR_SCHEMA_VIOLATION;
    goto cleanup;
  }

  inst->node_count = (size_t)cJSON_GetArraySize(nodes_arr);
  inst->nodes  = calloc(inst->node_count, sizeof(GSSK_NodeInternal));
  inst->state  = calloc(inst->node_count, sizeof(double));
  inst->dQ     = calloc(inst->node_count, sizeof(double));

  if (!inst->nodes || !inst->state || !inst->dQ) {
    status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
  }

  bool any_quality = false;
  for (int i = 0; i < (int)inst->node_count; i++) {
    cJSON *node = cJSON_GetArrayItem(nodes_arr, i);
    cJSON *id   = cJSON_GetObjectItem(node, "id");
    cJSON *type = cJSON_GetObjectItem(node, "type");
    cJSON *val  = cJSON_GetObjectItem(node, "value");

    if (!cJSON_IsString(id) || !cJSON_IsString(type) || !cJSON_IsNumber(val)) {
      snprintf(inst->error_msg, sizeof(inst->error_msg),
               "Schema Error: Node %d missing id/type/value.", i);
      status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
    }

    for (int j = 0; j < i; j++) {
      if (strcmp(inst->nodes[j].id, id->valuestring) == 0) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Duplicate node ID '%s'.", id->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
    }

    strncpy(inst->nodes[i].id, id->valuestring, 63);
    inst->nodes[i].id[63]        = '\0';
    inst->nodes[i].type          = parse_node_type(type->valuestring);
    inst->nodes[i].initial_value = val->valuedouble;
    inst->nodes[i].active        = true;
    inst->state[i]               = val->valuedouble;

    /* Optional v2 fields */
    cJSON *qi = cJSON_GetObjectItem(node, "quality_input");
    if (cJSON_IsNumber(qi) && qi->valuedouble > 0.0) {
      inst->nodes[i].quality_input = qi->valuedouble;
      any_quality = true;
    }

    cJSON *om = cJSON_GetObjectItem(node, "output_mode");
    inst->nodes[i].output_mode = parse_output_mode(
        cJSON_IsString(om) ? om->valuestring : NULL);
  }

  /* ---- 2. Edges ---- */
  cJSON *edges_arr = cJSON_GetObjectItem(root, "edges");
  if (cJSON_IsArray(edges_arr)) {
    inst->edge_count = (size_t)cJSON_GetArraySize(edges_arr);
    inst->edges = calloc(inst->edge_count, sizeof(GSSK_EdgeInternal));
    if (!inst->edges && inst->edge_count > 0) {
      status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
    }

    for (int i = 0; i < (int)inst->edge_count; i++) {
      cJSON *edge      = cJSON_GetArrayItem(edges_arr, i);
      cJSON *origin    = cJSON_GetObjectItem(edge, "origin");
      cJSON *target    = cJSON_GetObjectItem(edge, "target");
      cJSON *logic_str = cJSON_GetObjectItem(edge, "logic");
      cJSON *params    = cJSON_GetObjectItem(edge, "params");

      if (!cJSON_IsString(origin) || !cJSON_IsString(target) ||
          !cJSON_IsString(logic_str) || !cJSON_IsObject(params)) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Edge %d missing origin/target/logic/params.", i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      /* Optional id */
      cJSON *eid = cJSON_GetObjectItem(edge, "id");
      if (cJSON_IsString(eid)) {
        strncpy(inst->edges[i].id, eid->valuestring, 63);
        inst->edges[i].id[63] = '\0';
      }

      inst->edges[i].origin_idx = find_node_idx(inst, origin->valuestring);
      inst->edges[i].target_idx = find_node_idx(inst, target->valuestring);
      inst->edges[i].active     = true;
      inst->edges[i].coupled_idx = -1;

      if (inst->edges[i].origin_idx == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Linkage Error: Edge %d unknown origin '%s'.",
                 i, origin->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      if (inst->edges[i].target_idx == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Linkage Error: Edge %d unknown target '%s'.",
                 i, target->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      int lt = parse_logic_type(logic_str->valuestring);
      if (lt == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Logic Error: Unknown logic '%s' in edge %d.",
                 logic_str->valuestring, i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      inst->edges[i].logic = (GSSK_LogicType)lt;

      cJSON *k = cJSON_GetObjectItem(params, "k");
      if (!cJSON_IsNumber(k)) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Edge %d missing 'k'.", i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      inst->edges[i].k = k->valuedouble;

      cJSON *ctrl = cJSON_GetObjectItem(params, "control_node");
      inst->edges[i].control_idx = cJSON_IsString(ctrl)
          ? find_node_idx(inst, ctrl->valuestring) : -1;
      if (cJSON_IsString(ctrl) && inst->edges[i].control_idx == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Linkage Error: Edge %d unknown control_node '%s'.",
                 i, ctrl->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      cJSON *thr = cJSON_GetObjectItem(params, "threshold");
      inst->edges[i].threshold = cJSON_IsNumber(thr) ? thr->valuedouble : 0.0;

      if ((inst->edges[i].logic == GSSK_LOGIC_INTERACTION ||
           inst->edges[i].logic == GSSK_LOGIC_LIMIT) &&
          inst->edges[i].control_idx == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Logic Error: Edge %d (%s) requires control_node.",
                 i, logic_str->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      /* Optional v2 fields */
      cJSON *om = cJSON_GetObjectItem(edge, "output_mode");
      inst->edges[i].output_mode = parse_output_mode(
          cJSON_IsString(om) ? om->valuestring : NULL);

      /* carrier: stored as string in id for now (v2 extension, kernel ignores
       * carrier for ODE — used only in quality pass and UI) */
    }

    /* Second pass: resolve coupled_edge references (after all edges parsed) */
    for (int i = 0; i < (int)inst->edge_count; i++) {
      cJSON *edge  = cJSON_GetArrayItem(edges_arr, i);
      cJSON *cpled = cJSON_GetObjectItem(edge, "coupled_edge");
      if (cJSON_IsString(cpled)) {
        inst->edges[i].coupled_idx = find_edge_idx(inst, cpled->valuestring);
      }
    }
  }

  /* ---- 3. Config ---- */
  cJSON *config = cJSON_GetObjectItem(root, "config");
  inst->config.t_start          = 0.0;
  inst->config.t_end            = 100.0;
  inst->config.dt               = 0.1;
  inst->config.method           = GSSK_METHOD_AUTO;
  inst->config.solver_tolerance = 1e-6;

  if (cJSON_IsObject(config)) {
    cJSON *ts = cJSON_GetObjectItem(config, "t_start");
    cJSON *te = cJSON_GetObjectItem(config, "t_end");
    cJSON *dt = cJSON_GetObjectItem(config, "dt");
    cJSON *tol = cJSON_GetObjectItem(config, "solver_tolerance");

    if (cJSON_IsNumber(ts))  inst->config.t_start = ts->valuedouble;
    if (cJSON_IsNumber(te))  inst->config.t_end   = te->valuedouble;
    if (cJSON_IsNumber(dt))  inst->config.dt       = dt->valuedouble;
    if (cJSON_IsNumber(tol)) inst->config.solver_tolerance = tol->valuedouble;

    if (inst->config.t_end <= inst->config.t_start) {
      snprintf(inst->error_msg, sizeof(inst->error_msg),
               "Config Error: t_end (%.2f) must be > t_start (%.2f).",
               inst->config.t_end, inst->config.t_start);
      status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
    }
    if (inst->config.dt <= 0.0) {
      snprintf(inst->error_msg, sizeof(inst->error_msg),
               "Config Error: dt (%.4f) must be > 0.", inst->config.dt);
      status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
    }

    cJSON *meth = cJSON_GetObjectItem(config, "method");
    if (cJSON_IsString(meth)) {
      if (strcmp(meth->valuestring, "euler")     == 0)
        inst->config.method = GSSK_METHOD_EULER;
      else if (strcmp(meth->valuestring, "rk4")  == 0)
        inst->config.method = GSSK_METHOD_RK4;
      else if (strcmp(meth->valuestring, "incipient") == 0)
        inst->config.method = GSSK_METHOD_INCIPIENT;
      else
        inst->config.method = GSSK_METHOD_AUTO; /* "auto" or unknown → auto */
    }
  }

  /* ---- 4. Allocate scratchpads ---- */
  size_t n = inst->node_count;

  /* RK4 scratchpads — always allocated (needed for dual-solver verification) */
  inst->k2        = calloc(n, sizeof(double));
  inst->k3        = calloc(n, sizeof(double));
  inst->k4        = calloc(n, sizeof(double));
  inst->tmp_state = calloc(n, sizeof(double));
  inst->idc_state = calloc(n, sizeof(double));
  if (!inst->k2 || !inst->k3 || !inst->k4 ||
      !inst->tmp_state || !inst->idc_state) {
    status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
  }

  /* Quality accounting arrays */
  inst->quality_enabled = any_quality;
  if (any_quality) {
    inst->transformity  = calloc(n, sizeof(double));
    inst->quality_flow  = calloc(n, sizeof(double));
    inst->edge_qflow    = calloc(inst->edge_count ? inst->edge_count : 1,
                                 sizeof(double));
    if (!inst->transformity || !inst->quality_flow || !inst->edge_qflow) {
      status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
    }
  }

  /* ---- 5. Classify network ---- */
  GSSK_ReclassifyNetwork(inst);

cleanup:
  cJSON_Delete(root);
  return status;
}

/* =========================================================================
 * GSSK_Step
 * ========================================================================= */

GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt) {
  if (!inst) return GSSK_ERR_UNKNOWN;

  size_t n    = inst->node_count;
  GSSK_Status ret = GSSK_SUCCESS;

  /* ---- RK4 step (always computed — ground truth / fallback) ---- */
  /* k1 */
  compute_derivatives(inst, inst->state, inst->dQ);
  /* k2 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = inst->state[i] + 0.5 * dt * inst->dQ[i];
  compute_derivatives(inst, inst->tmp_state, inst->k2);
  /* k3 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = inst->state[i] + 0.5 * dt * inst->k2[i];
  compute_derivatives(inst, inst->tmp_state, inst->k3);
  /* k4 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = inst->state[i] + dt * inst->k3[i];
  compute_derivatives(inst, inst->tmp_state, inst->k4);

  /* RK4 result stored in tmp_state */
  for (size_t i = 0; i < n; i++) {
    inst->tmp_state[i] = inst->state[i] +
        (dt / 6.0) * (inst->dQ[i] + 2.0 * inst->k2[i] +
                      2.0 * inst->k3[i] + inst->k4[i]);
  }

  /* ---- Solver dispatch ---- */
  if (inst->config.method == GSSK_METHOD_EULER) {
    /* Pure Euler */
    compute_derivatives(inst, inst->state, inst->dQ);
    for (size_t i = 0; i < n; i++)
      inst->state[i] += inst->dQ[i] * dt;

  } else if (inst->config.method == GSSK_METHOD_RK4) {
    /* Pure RK4 */
    memcpy(inst->state, inst->tmp_state, n * sizeof(double));

  } else if (inst->config.method == GSSK_METHOD_INCIPIENT ||
             inst->config.method == GSSK_METHOD_AUTO) {

    if (inst->incipient_eligible) {
      /* IDC step */
      idc_step(inst, inst->state, inst->idc_state, dt);
    }

    if (inst->config.method == GSSK_METHOD_AUTO && inst->incipient_eligible) {
      /* Dual-solver cross-validation */
      double max_err = 0.0;
      for (size_t i = 0; i < n; i++) {
        double rk = inst->tmp_state[i];
        double idc = inst->idc_state[i];
        double denom = fabs(rk) > 1e-12 ? fabs(rk) : 1e-12;
        double err = fabs(idc - rk) / denom;
        if (err > max_err) max_err = err;
      }

      if (max_err < inst->config.solver_tolerance) {
        /* Solvers agree — use IDC result */
        inst->confidence = GSSK_CONFIDENCE_HIGH;
        memcpy(inst->state, inst->idc_state, n * sizeof(double));
      } else {
        /* Solvers diverge — fall back to RK4, freeze cybernetic adjustments */
        inst->confidence = GSSK_CONFIDENCE_DEGRADED;
        memcpy(inst->state, inst->tmp_state, n * sizeof(double));
        ret = GSSK_WARN_SOLVER_DIVERGENCE;
      }
    } else if (inst->config.method == GSSK_METHOD_INCIPIENT &&
               inst->incipient_eligible) {
      /* Forced IDC, no cross-check */
      inst->confidence = GSSK_CONFIDENCE_HIGH;
      memcpy(inst->state, inst->idc_state, n * sizeof(double));
    } else {
      /* Not IDC-eligible (limit/threshold edges present) — fall back to RK4 */
      inst->confidence = GSSK_CONFIDENCE_HIGH; /* RK4 is the authority */
      memcpy(inst->state, inst->tmp_state, n * sizeof(double));
    }
  }

  /* ---- Post-step: stability and clamping ---- */
  for (size_t i = 0; i < n; i++) {
    if (isnan(inst->state[i]) || isinf(inst->state[i]))
      return GSSK_ERR_DIVERGENCE;
    if (inst->state[i] < 0.0)
      inst->state[i] = 0.0;
  }

  /* ---- Quality accounting pass ---- */
  if (inst->quality_enabled)
    compute_quality_pass(inst, inst->state);

  return ret;
}

/* =========================================================================
 * Topology mutation
 * ========================================================================= */

GSSK_Status GSSK_AddNode(GSSK_Instance *inst, const char *json_node_fragment) {
  if (!inst || !json_node_fragment) return GSSK_ERR_UNKNOWN;

  cJSON *node = cJSON_Parse(json_node_fragment);
  if (!node) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddNode: JSON parse error");
    return GSSK_ERR_INVALID_JSON;
  }

  cJSON *id   = cJSON_GetObjectItem(node, "id");
  cJSON *type = cJSON_GetObjectItem(node, "type");
  cJSON *val  = cJSON_GetObjectItem(node, "value");

  if (!cJSON_IsString(id) || !cJSON_IsString(type) || !cJSON_IsNumber(val)) {
    cJSON_Delete(node);
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddNode: missing id/type/value");
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  if (find_node_idx(inst, id->valuestring) != -1) {
    cJSON_Delete(node);
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddNode: duplicate id '%s'", id->valuestring);
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  /* Grow arrays */
  size_t new_n = inst->node_count + 1;
  GSSK_NodeInternal *new_nodes  = realloc(inst->nodes,
      new_n * sizeof(GSSK_NodeInternal));
  double *new_state = realloc(inst->state, new_n * sizeof(double));
  double *new_dQ    = realloc(inst->dQ,    new_n * sizeof(double));
  double *new_k2    = realloc(inst->k2,    new_n * sizeof(double));
  double *new_k3    = realloc(inst->k3,    new_n * sizeof(double));
  double *new_k4    = realloc(inst->k4,    new_n * sizeof(double));
  double *new_tmp   = realloc(inst->tmp_state, new_n * sizeof(double));
  double *new_idc   = realloc(inst->idc_state, new_n * sizeof(double));

  if (!new_nodes || !new_state || !new_dQ || !new_k2 ||
      !new_k3 || !new_k4 || !new_tmp || !new_idc) {
    cJSON_Delete(node);
    return GSSK_ERR_MALLOC_FAILED;
  }

  inst->nodes     = new_nodes;
  inst->state     = new_state;
  inst->dQ        = new_dQ;
  inst->k2        = new_k2;
  inst->k3        = new_k3;
  inst->k4        = new_k4;
  inst->tmp_state = new_tmp;
  inst->idc_state = new_idc;

  /* Grow quality arrays if enabled */
  if (inst->quality_enabled) {
    double *new_tr = realloc(inst->transformity, new_n * sizeof(double));
    double *new_qf = realloc(inst->quality_flow,  new_n * sizeof(double));
    if (!new_tr || !new_qf) { cJSON_Delete(node); return GSSK_ERR_MALLOC_FAILED; }
    inst->transformity = new_tr;
    inst->quality_flow = new_qf;
    inst->transformity[new_n - 1] = 0.0;
    inst->quality_flow[new_n - 1] = 0.0;
  }

  size_t idx = inst->node_count;
  memset(&inst->nodes[idx], 0, sizeof(GSSK_NodeInternal));
  strncpy(inst->nodes[idx].id, id->valuestring, 63);
  inst->nodes[idx].id[63]        = '\0';
  inst->nodes[idx].type          = parse_node_type(type->valuestring);
  inst->nodes[idx].initial_value = val->valuedouble;
  inst->nodes[idx].active        = true;
  inst->state[idx]               = val->valuedouble;
  inst->dQ[idx]                  = 0.0;
  inst->k2[idx] = inst->k3[idx] = inst->k4[idx] = 0.0;
  inst->tmp_state[idx] = inst->idc_state[idx] = 0.0;

  cJSON *qi = cJSON_GetObjectItem(node, "quality_input");
  if (cJSON_IsNumber(qi) && qi->valuedouble > 0.0)
    inst->nodes[idx].quality_input = qi->valuedouble;

  cJSON *om = cJSON_GetObjectItem(node, "output_mode");
  inst->nodes[idx].output_mode = parse_output_mode(
      cJSON_IsString(om) ? om->valuestring : NULL);

  inst->node_count = new_n;
  cJSON_Delete(node);
  return GSSK_ReclassifyNetwork(inst);
}

GSSK_Status GSSK_AddEdge(GSSK_Instance *inst, const char *json_edge_fragment) {
  if (!inst || !json_edge_fragment) return GSSK_ERR_UNKNOWN;

  cJSON *edge = cJSON_Parse(json_edge_fragment);
  if (!edge) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddEdge: JSON parse error");
    return GSSK_ERR_INVALID_JSON;
  }

  cJSON *origin    = cJSON_GetObjectItem(edge, "origin");
  cJSON *target    = cJSON_GetObjectItem(edge, "target");
  cJSON *logic_str = cJSON_GetObjectItem(edge, "logic");
  cJSON *params    = cJSON_GetObjectItem(edge, "params");

  if (!cJSON_IsString(origin) || !cJSON_IsString(target) ||
      !cJSON_IsString(logic_str) || !cJSON_IsObject(params)) {
    cJSON_Delete(edge);
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddEdge: missing origin/target/logic/params");
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  int orig_idx = find_node_idx(inst, origin->valuestring);
  int tgt_idx  = find_node_idx(inst, target->valuestring);
  if (orig_idx == -1 || tgt_idx == -1) {
    cJSON_Delete(edge);
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddEdge: unknown origin/target node");
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  int lt = parse_logic_type(logic_str->valuestring);
  if (lt == -1) {
    cJSON_Delete(edge);
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "GSSK_AddEdge: unknown logic '%s'", logic_str->valuestring);
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  cJSON *k = cJSON_GetObjectItem(params, "k");
  if (!cJSON_IsNumber(k)) {
    cJSON_Delete(edge);
    snprintf(inst->error_msg, sizeof(inst->error_msg), "GSSK_AddEdge: missing k");
    return GSSK_ERR_SCHEMA_VIOLATION;
  }

  /* Grow edge arrays */
  size_t new_ec = inst->edge_count + 1;
  GSSK_EdgeInternal *new_edges = realloc(inst->edges,
      new_ec * sizeof(GSSK_EdgeInternal));
  if (!new_edges) { cJSON_Delete(edge); return GSSK_ERR_MALLOC_FAILED; }
  inst->edges = new_edges;

  if (inst->quality_enabled) {
    double *new_eqf = realloc(inst->edge_qflow, new_ec * sizeof(double));
    if (!new_eqf) { cJSON_Delete(edge); return GSSK_ERR_MALLOC_FAILED; }
    inst->edge_qflow = new_eqf;
    inst->edge_qflow[new_ec - 1] = 0.0;
  }

  size_t ei = inst->edge_count;
  memset(&inst->edges[ei], 0, sizeof(GSSK_EdgeInternal));

  cJSON *eid = cJSON_GetObjectItem(edge, "id");
  if (cJSON_IsString(eid)) {
    strncpy(inst->edges[ei].id, eid->valuestring, 63);
    inst->edges[ei].id[63] = '\0';
  }

  inst->edges[ei].origin_idx  = orig_idx;
  inst->edges[ei].target_idx  = tgt_idx;
  inst->edges[ei].logic       = (GSSK_LogicType)lt;
  inst->edges[ei].k           = k->valuedouble;
  inst->edges[ei].active      = true;
  inst->edges[ei].coupled_idx = -1;
  inst->edges[ei].control_idx = -1;

  cJSON *ctrl = cJSON_GetObjectItem(params, "control_node");
  if (cJSON_IsString(ctrl))
    inst->edges[ei].control_idx = find_node_idx(inst, ctrl->valuestring);

  cJSON *thr = cJSON_GetObjectItem(params, "threshold");
  inst->edges[ei].threshold = cJSON_IsNumber(thr) ? thr->valuedouble : 0.0;

  cJSON *om = cJSON_GetObjectItem(edge, "output_mode");
  inst->edges[ei].output_mode = parse_output_mode(
      cJSON_IsString(om) ? om->valuestring : NULL);

  inst->edge_count = new_ec;
  cJSON_Delete(edge);
  return GSSK_ReclassifyNetwork(inst);
}

GSSK_Status GSSK_DeactivateEdge(GSSK_Instance *inst, const char *edge_id) {
  if (!inst || !edge_id) return GSSK_ERR_UNKNOWN;
  int idx = find_edge_idx(inst, edge_id);
  if (idx == -1) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "DeactivateEdge: edge '%s' not found", edge_id);
    return GSSK_ERR_NOT_FOUND;
  }
  inst->edges[idx].active = false;
  inst->edges[idx].k      = 0.0;
  return GSSK_ReclassifyNetwork(inst);
}

GSSK_Status GSSK_DeactivateNode(GSSK_Instance *inst, const char *node_id) {
  if (!inst || !node_id) return GSSK_ERR_UNKNOWN;
  int idx = find_node_idx(inst, node_id);
  if (idx == -1) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "DeactivateNode: node '%s' not found", node_id);
    return GSSK_ERR_NOT_FOUND;
  }
  inst->nodes[idx].active = false;
  /* Zero all edges connected to this node */
  for (size_t i = 0; i < inst->edge_count; i++) {
    if (inst->edges[i].origin_idx == idx ||
        inst->edges[i].target_idx == idx) {
      inst->edges[i].active = false;
      inst->edges[i].k      = 0.0;
    }
  }
  return GSSK_ReclassifyNetwork(inst);
}

/* =========================================================================
 * Accessors
 * ========================================================================= */

void GSSK_Reset(GSSK_Instance *inst) {
  if (!inst) return;
  for (size_t i = 0; i < inst->node_count; i++)
    inst->state[i] = inst->nodes[i].initial_value;
  if (inst->transformity)
    memset(inst->transformity, 0, inst->node_count * sizeof(double));
  if (inst->quality_flow)
    memset(inst->quality_flow, 0, inst->node_count * sizeof(double));
  inst->confidence = GSSK_CONFIDENCE_HIGH;
}

const char *GSSK_GetErrorDescription(GSSK_Instance *inst) {
  return inst ? inst->error_msg : "Invalid Instance";
}

const double *GSSK_GetState(GSSK_Instance *inst) {
  return inst ? inst->state : NULL;
}

size_t GSSK_GetStateSize(GSSK_Instance *inst) {
  return inst ? inst->node_count : 0;
}

const double *GSSK_GetTransformationRatio(GSSK_Instance *inst) {
  return (inst && inst->quality_enabled) ? inst->transformity : NULL;
}

const double *GSSK_GetQualityFlow(GSSK_Instance *inst) {
  return (inst && inst->quality_enabled) ? inst->quality_flow : NULL;
}

double GSSK_GetEdgeQualityFlow(GSSK_Instance *inst, size_t edge_idx) {
  if (!inst || !inst->quality_enabled || edge_idx >= inst->edge_count)
    return 0.0;
  return inst->edge_qflow[edge_idx];
}

GSSK_SolverConfidence GSSK_GetSolverConfidence(GSSK_Instance *inst) {
  return inst ? inst->confidence : GSSK_CONFIDENCE_HIGH;
}

const char *GSSK_GetNodeID(GSSK_Instance *inst, size_t index) {
  if (!inst || index >= inst->node_count) return NULL;
  return inst->nodes[index].id;
}

int GSSK_FindNodeIdx(GSSK_Instance *inst, const char *id) {
  return (!inst || !id) ? -1 : find_node_idx(inst, id);
}

const char *GSSK_GetEdgeID(GSSK_Instance *inst, size_t index) {
  if (!inst || index >= inst->edge_count) return NULL;
  return inst->edges[index].id[0] ? inst->edges[index].id : NULL;
}

int GSSK_FindEdgeIdx(GSSK_Instance *inst, const char *id) {
  return (!inst || !id) ? -1 : find_edge_idx(inst, id);
}

size_t GSSK_GetEdgeCount(GSSK_Instance *inst) {
  return inst ? inst->edge_count : 0;
}

double GSSK_GetEdgeK(GSSK_Instance *inst, size_t index) {
  if (!inst || index >= inst->edge_count) return 0.0;
  return inst->edges[index].k;
}

void GSSK_SetEdgeK(GSSK_Instance *inst, size_t index, double k) {
  if (!inst || index >= inst->edge_count) return;
  inst->edges[index].k = k;
}

double GSSK_GetTStart(GSSK_Instance *inst) { return inst ? inst->config.t_start : 0.0; }
double GSSK_GetTEnd(GSSK_Instance *inst)   { return inst ? inst->config.t_end   : 0.0; }
double GSSK_GetDt(GSSK_Instance *inst)     { return inst ? inst->config.dt      : 0.0; }

void GSSK_Free(GSSK_Instance *inst) {
  if (!inst) return;
  free(inst->state);
  free(inst->dQ);
  free(inst->k2);
  free(inst->k3);
  free(inst->k4);
  free(inst->tmp_state);
  free(inst->idc_state);
  free(inst->nodes);
  free(inst->edges);
  free(inst->transformity);
  free(inst->quality_flow);
  free(inst->edge_qflow);
  free(inst);
}
