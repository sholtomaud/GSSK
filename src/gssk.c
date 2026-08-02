/**
 * @file gssk.c
 * @brief General Systems Kernel — Core Implementation (v3)
 *
 * Implements:
 * - Euler / RK4 / AUTO / INCIPIENT integration modes
 * - IDC as baseline solver in AUTO/INCIPIENT (no silent fallback — Phase 1)
 * - Padé (3,3) matrix-exponential: N(X)/D(X) where X=A·dt, A-stable, O(h⁷)
 * - Riccati exact duet for isolated 2-node interaction systems
 * - Limit edges included in IDC flow matrix via effective conductance g=k·C/(C+Q)
 * - Threshold event detection via Illinois algorithm (up to 64 iterations)
 * - Per-edge and step-level IDC vs RK4 error estimates
 * - Dual-solver verification: IDC result used when within tolerance, else RK4
 * - Quality accounting pass (Brown 2025 matrix method)
 * - Topology mutation API (AddNode, AddEdge, Deactivate, Reclassify)
 * - Odum four-position inter-block channel support
 * - Metadata versioning and schema v3 support
 * - Round-trip serialization (SerializeModel / SerializeSnapshot)
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
#include <stdint.h>

/* Maximum number of threshold crossings processed per GSSK_Step call.
 * Sequential crossings (sub-stepping) and simultaneous crossings both
 * count against this limit. */
#define GSSK_MAX_EVENTS_PER_STEP 8

/* =========================================================================
 * Internal types
 * ========================================================================= */

typedef enum {
  NODE_STORAGE, NODE_SOURCE, NODE_SINK, NODE_CONSTANT,
  NODE_INTERACTION,  /* Phase 7: multi-input production/work gate */
  NODE_GAIN,         /* Phase 7: constant gain amplifier */
  NODE_LOOP_LIMITED, /* Phase 7: Michaelis-Menten loop-limited converter */
  NODE_EXCHANGE,     /* Phase 7: transaction exchange diamond */
  NODE_SWITCH        /* Phase 7: digital switching box */
} GSSK_NodeType;
typedef enum { OUTPUT_PARTITION, OUTPUT_REPLICATE } GSSK_OutputMode;

typedef struct {
  char           id[64];
  GSSK_NodeType  type;
  double         initial_value;
  double         quality_input; /* boundary Tr; 0 = disabled */
  GSSK_OutputMode output_mode;
  bool           active;
  char           carrier[32];  /* Odum Position 1 carrier label — metadata only */
  /* Phase 7 — processing node params */
  double  node_k;         /* gain/rate coefficient */
  double  node_C;         /* loop_limited: saturation constant */
  double  node_threshold; /* switch: threshold */
  double  node_price;     /* exchange: price per unit of goods */
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
  char            carrier[32];   /* Odum Position 1 carrier label — metadata only */
} GSSK_EdgeInternal;

/* Internal event record (Phase 1.3) */
typedef struct {
  double t;
  char   edge_id[64];
  int    direction; /* +1 = crossed upward, -1 = crossed downward */
} GSSK_EventInternal;

/* =========================================================================
 * Phase 8 — Composite Node Types & Archetype System (internal)
 * ========================================================================= */

#define GSSK_MAX_ARCH_NODES   16
#define GSSK_MAX_ARCH_EDGES   32
#define GSSK_MAX_ARCH_PORTS    8
#define GSSK_MAX_ARCHETYPES   32
#define GSSK_MAX_COMPOSITES  128

/* Template for one node inside an archetype definition */
typedef struct {
  char   id[64];        /* internal id within archetype namespace */
  char   type_str[32];  /* node type string */
  double value;
  double node_k, node_C, node_threshold, node_price;
  char   carrier[32];
} GSSK_ANodeTmpl;

/* Template for one edge inside an archetype definition */
typedef struct {
  char   id[64];
  char   origin[64];
  char   target[64];
  char   logic[32];     /* logic type string; empty = "linear" */
  double k;
  double threshold;
  char   carrier[32];
} GSSK_AEdgeTmpl;

/* One archetype definition (built-in or user-defined) */
typedef struct {
  char           name[64];
  GSSK_ANodeTmpl nodes[GSSK_MAX_ARCH_NODES];
  size_t         node_count;
  GSSK_AEdgeTmpl edges[GSSK_MAX_ARCH_EDGES];
  size_t         edge_count;
  char           port_names[GSSK_MAX_ARCH_PORTS][32];
  char           port_nodes[GSSK_MAX_ARCH_PORTS][64];
  size_t         port_count;
  char           default_in[64];
  char           default_out[64];
  bool           is_structural;
} GSSK_ADefn;

/* Per-instance composite expansion record */
typedef struct {
  char composite_id[64];
  int  in_node_idx;
  int  out_node_idx;
} GSSK_CompositeMap;

/* -------------------------------------------------------------------------
 * Phase 9 — Runtime Pattern Discovery
 * ------------------------------------------------------------------------- */
#define GSSK_MOTIF_TABLE_CAP      256  /* max distinct motif patterns tracked */
#define GSSK_MOTIF_MIN_COUNT        3  /* occurrences/step to count toward stability */
#define GSSK_MOTIF_MIN_STEPS       10  /* consecutive stable steps → candidate */
#define GSSK_MOTIF_SCAN_NODE_LIMIT 64  /* skip scan when node_count exceeds this */

typedef struct {
  char    canon[128];     /* canonical pattern string */
  char    node_types[3][32]; /* node type strings in canonical order */
  uint8_t size;           /* 2 or 3 */
  uint8_t edge_bits;      /* adjacency bitmask in canonical order */
  double  complexity;     /* popcount(edge_bits) / size */
  size_t  occurrence;     /* times seen this step */
  size_t  stable_steps;   /* consecutive steps meeting MIN_COUNT */
  bool    is_candidate;   /* promoted: stable_steps >= MIN_STEPS */
} GSSK_MotifEntry;

struct GSSK_Instance {
  char error_msg[256];

  /* ODE state */
  double *state;       /* Q[node_count] */
  double *dQ;          /* derivatives scratchpad (Euler / RK4 k1) */

  /* RK4 scratchpads — always allocated */
  double *k2, *k3, *k4, *tmp_state;

  /* IDC / dual-solver scratchpad */
  double *idc_state;   /* result of IDC step (compared against RK4) */

  /* Phase 1 — per-edge and step error estimates */
  double *edge_error;  /* |flow_idc - flow_rk4| / max(|flow_rk4|, eps) per edge */
  double  step_error;  /* max over all edges */

  /* Phase 1.3 — threshold event log */
  GSSK_EventInternal *events;
  size_t event_count;
  size_t event_capacity;

  /* Phase 2 — DOPRI5 adaptive numerics */
  double *k5, *k6, *k7;        /* stages 5, 6, 7 of DOPRI5 (k1-k4 reuse dQ/k2-k4) */
  double  h_next;               /* suggested step size for next GSSK_StepAdaptive */
  double  h_last;               /* actual h accepted in last step */
  double  err_norm_prev;        /* previous normalized DOPRI5 error (PI controller) */
  double  conservation_error;   /* relative total-Q change last step */
  GSSK_DiagHooks diag_hooks;    /* diagnostic callbacks (on_step, on_event, etc.) */

  /* Phase 3 — Forward Sensitivity: S[node_count × sens_param_count] = ∂Q_i/∂k_j */
  double  *sens_matrix;         /* row-major: S[i*m+j] */
  size_t  *sens_param_idx;      /* edge indices of tracked parameters */
  size_t   sens_param_count;    /* m */

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
  bool                 incipient_eligible; /* Phase 1: always true */
  GSSK_SolverConfidence confidence;

  struct {
    double       t_start;
    double       t_end;
    double       dt;
    GSSK_Method  method;
    double       solver_tolerance;
    /* Phase 2 — adaptive numerics */
    double       rel_tol;  /* DOPRI5 relative tolerance (default 1e-6) */
    double       abs_tol;  /* DOPRI5 absolute tolerance (default 1e-9) */
    double       h_min;    /* min adaptive step size (0 = dt * 1e-6) */
    double       h_max;    /* max adaptive step size (0 = dt) */
  } config;

  /* Metadata (v3) */
  int         schema_version;
  char        created_at[32];
  char        kernel_version[16];
  char        model_hash[65];
  char        model_name[256];
  char        model_description[1024];
  char        model_author[128];

  /* Simulation clock — advances with each successful GSSK_Step() */
  double      current_t;
  size_t      step_count;

  /* Phase 4 — Mutation Log */
  GSSK_MutationRecord *mutation_log;
  size_t               mutation_count;
  size_t               mutation_capacity;
  char                 pending_cause[64]; /* cleared after first mutation append */

  /* Phase 5 — Multi-Carrier */
  GSSK_Carrier *carriers;            /* carrier definitions from "carriers" array */
  size_t        carrier_count;
  double       *carrier_cons_error;  /* per-carrier conservation error, last step */

  /* Phase 8 — Archetype System */
  GSSK_ADefn        arch_defns[GSSK_MAX_ARCHETYPES];
  size_t            arch_count;
  GSSK_CompositeMap composites[GSSK_MAX_COMPOSITES];
  size_t            composite_count;

  /* Phase 9 — Runtime Pattern Discovery */
  GSSK_MotifEntry motifs[GSSK_MOTIF_TABLE_CAP];
  size_t          motif_count;
  double          generativity_index;
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static GSSK_MutationOp parse_mutation_op(const char *s);  /* forward decl (Phase 4) */
static const char *node_type_str(GSSK_NodeType t);          /* forward decl (Phase 7) */
static void append_mutation(GSSK_Instance *inst, GSSK_MutationOp op,
                             const char *target_id, const char *payload); /* Phase 4, used in Phase 9 */
static void scan_motifs_internal(GSSK_Instance *inst);                    /* Phase 9 */

static GSSK_NodeType parse_node_type(const char *s) {
  if (strcmp(s, "source")       == 0) return NODE_SOURCE;
  if (strcmp(s, "sink")         == 0) return NODE_SINK;
  if (strcmp(s, "constant")     == 0) return NODE_CONSTANT;
  if (strcmp(s, "interaction")  == 0) return NODE_INTERACTION;
  if (strcmp(s, "gain")         == 0) return NODE_GAIN;
  if (strcmp(s, "loop_limited") == 0) return NODE_LOOP_LIMITED;
  if (strcmp(s, "exchange")     == 0) return NODE_EXCHANGE;
  if (strcmp(s, "switch")       == 0) return NODE_SWITCH;
  return NODE_STORAGE;
}

static bool is_processing_node(GSSK_NodeType t) {
  return t == NODE_INTERACTION || t == NODE_GAIN ||
         t == NODE_LOOP_LIMITED || t == NODE_EXCHANGE ||
         t == NODE_SWITCH;
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

/* Phase 7 — per-type processing-node helpers.  Each helper computes a flow F
 * "through" node `ni` based on its incoming/outgoing edges and applies F as
 * deductions on the upstream Q stocks and additions on the downstream Q
 * stocks.  Processing nodes themselves never accumulate Q (boundary handled
 * by compute_derivatives below). */

static void compute_interaction_node(GSSK_Instance *inst, size_t ni,
                                     const double *state, double *deriv) {
  /* Multi-input product gate.  First incoming edge → "energy" (consumed).
   * Remaining incoming edges → "control" inputs (not consumed, multiplicative).
   * F = node_k × ∏ state[origin]; output evenly partitioned over outputs. */
  double F = inst->nodes[ni].node_k;
  int    energy_orig = -1;
  size_t in_count = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->target_idx != ni) continue;
    F *= state[e->origin_idx];
    if (in_count == 0) energy_orig = e->origin_idx;
    in_count++;
  }
  if (in_count == 0) return;

  /* count outputs */
  size_t out_count = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    out_count++;
  }

  if (energy_orig >= 0) deriv[energy_orig] -= F;
  if (out_count == 0) return;
  double share = F / (double)out_count;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    deriv[e->target_idx] += share;
  }
}

static void compute_gain_node(GSSK_Instance *inst, size_t ni,
                              const double *state, double *deriv) {
  /* Constant-gain amplifier.  First incoming edge → control (not consumed).
   * Second (if any) → energy source.  If energy source is a STORAGE node,
   * draw F from it; otherwise (SOURCE/CONSTANT) treat as free supply. */
  int control_orig = -1, energy_orig = -1;
  size_t seen = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->target_idx != ni) continue;
    if (seen == 0)      control_orig = e->origin_idx;
    else if (seen == 1) energy_orig  = e->origin_idx;
    seen++;
  }
  if (control_orig < 0) return;

  double F = inst->nodes[ni].node_k * state[control_orig];

  if (energy_orig >= 0 && inst->nodes[energy_orig].type == NODE_STORAGE)
    deriv[energy_orig] -= F;

  size_t out_count = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    out_count++;
  }
  if (out_count == 0) return;
  double share = F / (double)out_count;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    deriv[e->target_idx] += share;
  }
}

static void compute_loop_limited_node(GSSK_Instance *inst, size_t ni,
                                      const double *state, double *deriv) {
  /* Michaelis-Menten loop-limited converter.  Single incoming edge. */
  int in_orig = -1;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->target_idx != ni) continue;
    in_orig = e->origin_idx; break;
  }
  if (in_orig < 0) return;

  double Q_in = state[in_orig];
  double C    = inst->nodes[ni].node_C > 1e-9 ? inst->nodes[ni].node_C : 1.0;
  double F    = inst->nodes[ni].node_k * Q_in / (1.0 + Q_in / C);

  deriv[in_orig] -= F;

  size_t out_count = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    out_count++;
  }
  if (out_count == 0) return;
  double share = F / (double)out_count;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    deriv[e->target_idx] += share;
  }
}

static void compute_switch_node(GSSK_Instance *inst, size_t ni,
                                const double *state, double *deriv) {
  /* Digital switch.  First incoming edge → flow source (consumed when ON).
   * Second (optional) → sensor (not consumed).  If only one input, it acts
   * as both flow source and sensor. */
  int flow_orig = -1, sensor_orig = -1;
  size_t seen = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->target_idx != ni) continue;
    if (seen == 0)      flow_orig   = e->origin_idx;
    else if (seen == 1) sensor_orig = e->origin_idx;
    seen++;
  }
  if (flow_orig < 0) return;
  if (sensor_orig < 0) sensor_orig = flow_orig;

  bool   on = state[sensor_orig] > inst->nodes[ni].node_threshold;
  double F  = on ? (inst->nodes[ni].node_k * state[flow_orig]) : 0.0;
  if (!on) return;

  deriv[flow_orig] -= F;

  size_t out_count = 0;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    out_count++;
  }
  if (out_count == 0) return;
  double share = F / (double)out_count;
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    if ((size_t)e->origin_idx != ni) continue;
    deriv[e->target_idx] += share;
  }
}

/* ---- Transaction diamond: shared physics primitive (ADR 0001) -------------
 *
 * Odum's small diamond is both a junction and a coupling ratio.  It has two
 * authoring forms — the NODE_EXCHANGE hub and the inline `coupled_edge` — and
 * per ADR 0001 both are backed by the helpers below so their behaviour cannot
 * diverge.  Three seams are shared, not one:
 *
 *   resolve_exchange_legs()      which edges form the diamond
 *   exchange_primary_flow()      the forward flow, including its money gate
 *   apply_transaction_coupling() the counter-flow F_money = price x F_primary
 *
 * The leg-discovery seam matters because the hand-written NODE_EXCHANGE
 * Jacobian block re-derived it independently; sharing it is what keeps the RK4
 * and incipient paths from drifting apart.
 */

typedef struct {
  int goods_in;   /* upstream goods stock, -1 if absent */
  int money_in;   /* upstream money stock, -1 if absent */
  int goods_out;  /* downstream goods stock, -1 if absent */
  int money_out;  /* downstream money stock, -1 if absent */
} ExchangeLegs;

/* Classify the edges incident on exchange node `ni` by carrier.  "money" is a
 * magic carrier string; anything else counts as goods.  Single-valued and
 * last-wins — a second same-carrier leg silently replaces the first. */
static ExchangeLegs resolve_exchange_legs(const GSSK_Instance *inst, size_t ni) {
  ExchangeLegs L = { -1, -1, -1, -1 };
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    const GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    bool is_money = (strcmp(e->carrier, "money") == 0);
    if ((size_t)e->target_idx == ni) {
      if (is_money) L.money_in  = e->origin_idx;
      else          L.goods_in  = e->origin_idx;
    } else if ((size_t)e->origin_idx == ni) {
      if (is_money) L.money_out = e->target_idx;
      else          L.goods_out = e->target_idx;
    }
  }
  return L;
}

/* The price this diamond trades at.  Single accessor so Phase C.0 can make
 * price a node reference in one place and have every path — derivative,
 * incipient forcing and Jacobian — pick the change up together. */
static double exchange_price(const GSSK_Instance *inst, size_t ni) {
  return inst->nodes[ni].node_price;
}

/* Forward (real) flow through the diamond.  The buyer's money stock gates the
 * flow as an interaction multiplier — it is not merely the debited account —
 * so this is shared with the inline form too, not just the coupling ratio. */
static double exchange_primary_flow(const GSSK_Instance *inst, size_t ni,
                                    const ExchangeLegs *L, const double *state) {
  if (L->goods_in < 0) return 0.0;
  double F = inst->nodes[ni].node_k * state[L->goods_in];
  if (L->money_in >= 0) F *= state[L->money_in];
  return F;
}

/* Apply the money counter-flow paired to a forward flow of F_primary.  Money
 * runs backwards along the diamond: debited from `money_from`, credited to
 * `money_to`.  Either endpoint may be absent (-1). */
static void apply_transaction_coupling(double F_primary, double price,
                                       int money_from, int money_to,
                                       double *deriv) {
  double F_money = price * F_primary;
  if (money_from >= 0) deriv[money_from] -= F_money;
  if (money_to   >= 0) deriv[money_to]   += F_money;
}

static void compute_exchange_node(GSSK_Instance *inst, size_t ni,
                                  const double *state, double *deriv) {
  ExchangeLegs L = resolve_exchange_legs(inst, ni);
  if (L.goods_in < 0) return;

  double F_goods = exchange_primary_flow(inst, ni, &L, state);

  deriv[L.goods_in] -= F_goods;
  if (L.goods_out >= 0) deriv[L.goods_out] += F_goods;

  apply_transaction_coupling(F_goods, exchange_price(inst, ni),
                             L.money_in, L.money_out, deriv);
}

static void compute_derivatives(GSSK_Instance *inst, const double *state,
                                double *deriv) {
  memset(deriv, 0, inst->node_count * sizeof(double));

  /* v3 edges — skip any edge whose origin OR target is a v4 processing node.
   * Those edges only carry topology information; the per-type helper computes
   * the actual flow contribution. */
  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;
    if (is_processing_node(inst->nodes[e->origin_idx].type) ||
        is_processing_node(inst->nodes[e->target_idx].type))
      continue;

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
    case GSSK_LOGIC_LIMIT: {
      double C = -1.0;
      if (e->control_idx != -1) C = state[e->control_idx];
      else if (e->threshold > 0.0) C = e->threshold;
      if (C > 1e-9)
        flow = (e->k * Q_orig) / (1.0 + (Q_orig / C));
      break;
    }
    case GSSK_LOGIC_THRESHOLD:
      flow = (Q_orig > e->threshold) ? e->k : 0.0;
      break;
    }

    deriv[e->origin_idx] -= flow;
    deriv[e->target_idx] += flow;
  }

  /* v4 processing nodes — call per-type helper */
  for (size_t i = 0; i < inst->node_count; i++) {
    switch (inst->nodes[i].type) {
    case NODE_INTERACTION:  compute_interaction_node(inst, i, state, deriv); break;
    case NODE_GAIN:         compute_gain_node(inst, i, state, deriv); break;
    case NODE_LOOP_LIMITED: compute_loop_limited_node(inst, i, state, deriv); break;
    case NODE_SWITCH:       compute_switch_node(inst, i, state, deriv); break;
    case NODE_EXCHANGE:     compute_exchange_node(inst, i, state, deriv); break;
    default: break;
    }
  }

  /* Boundary: non-storage nodes have dQ/dt = 0 (sources, constants, and all
   * processing nodes — they do not accumulate Q). */
  for (size_t i = 0; i < inst->node_count; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT ||
        is_processing_node(inst->nodes[i].type))
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
    /* Phase 7: skip v3 edges whose endpoints touch a processing node — those
     * flows are computed by the per-type helper and treated as RK4 forcing. */
    if (is_processing_node(inst->nodes[e->origin_idx].type) ||
        is_processing_node(inst->nodes[e->target_idx].type))
      continue;

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
        conductance = e->k * state[e->control_idx];
        A[e->target_idx * (int)n + e->origin_idx] += conductance;
        A[e->origin_idx * (int)n + e->origin_idx] -= conductance;
      }
      break;
    case GSSK_LOGIC_LIMIT: {
      /* Linearise Michaelis-Menten at current Q: g = k·C/(C+Q) */
      double C = -1.0;
      if (e->control_idx != -1) C = state[e->control_idx];
      else if (e->threshold > 0.0) C = e->threshold;
      double Q = state[e->origin_idx];
      if (C > 1e-9) {
        conductance = e->k * C / (C + Q); /* → k as Q→0, → k/2 at Q=C */
        A[e->target_idx * (int)n + e->origin_idx] += conductance;
        A[e->origin_idx * (int)n + e->origin_idx] -= conductance;
      }
      break;
    }
    default:
      break; /* threshold: handled as constant forcing in build_forcing_vector */
    }
  }

  /* Zero rows for source/constant/processing nodes */
  for (size_t i = 0; i < n; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT ||
        is_processing_node(inst->nodes[i].type)) {
      for (size_t j = 0; j < n; j++)
        A[i * n + j] = 0.0;
    }
  }
}

/**
 * Compute constant/threshold forcing vector f[n].
 * Constant edges: f += k (state-independent).
 * Threshold edges: f += k if Q_origin > threshold (treated as constant this step).
 * Source/constant node rows zeroed (dQ/dt = 0 boundary condition).
 */
static void build_forcing_vector(GSSK_Instance *inst, const double *state,
                                 double *f) {
  size_t n = inst->node_count;
  memset(f, 0, n * sizeof(double));

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) continue;
    /* Phase 7: skip v3 edges touching processing nodes (handled elsewhere). */
    if (is_processing_node(inst->nodes[e->origin_idx].type) ||
        is_processing_node(inst->nodes[e->target_idx].type))
      continue;
    double flow = 0.0;
    if (e->logic == GSSK_LOGIC_CONSTANT) {
      flow = e->k;
    } else if (e->logic == GSSK_LOGIC_THRESHOLD) {
      flow = (state[e->origin_idx] > e->threshold) ? e->k : 0.0;
    } else {
      continue;
    }
    f[e->target_idx] += flow;
    f[e->origin_idx] -= flow;
  }

  for (size_t i = 0; i < n; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT ||
        is_processing_node(inst->nodes[i].type))
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

/* =========================================================================
 * Phase 1 — Padé (3,3) matrix exponential and IDC helpers
 *
 * expm_pade33(A, v, result, n, dt):
 *   Computes result = D(X)^{-1} N(X) v where X = A·dt,
 *   N(X) = 120I + 60X + 12X² + X³, D(X) = 120I − 60X + 12X² − X³.
 *   A-stable, O(h⁷) global error vs O(h⁷) for Taylor-6 but much better
 *   for large ‖A·dt‖ (rational vs truncated series).
 *
 * Riccati exact duet (Phase 1.1):
 *   For an isolated 2-node interaction: F = k·Q_A·Q_B, S = Q_A+Q_B conserved.
 *   Q_A(t+dt) = S·Q_A₀ / (Q_A₀ + Q_B₀·exp(k·S·dt))
 * ========================================================================= */

/**
 * Matrix-matrix multiply: C = A × B (all n×n, row-major).
 */
static void mat_mat(const double *A, const double *B, double *C, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      double s = 0.0;
      for (size_t k = 0; k < n; k++)
        s += A[i * n + k] * B[k * n + j];
      C[i * n + j] = s;
    }
  }
}

/**
 * Gaussian elimination with partial pivoting. Solves M·x = b in-place.
 * On return, b[] holds the solution x[]. Returns false if singular.
 */
static bool gaussian_solve(double *M, double *b, size_t n) {
  for (size_t col = 0; col < n; col++) {
    size_t pivot = col;
    double pval = fabs(M[col * n + col]);
    for (size_t row = col + 1; row < n; row++) {
      double v = fabs(M[row * n + col]);
      if (v > pval) { pval = v; pivot = row; }
    }
    if (pval < 1e-14) return false;
    if (pivot != col) {
      for (size_t j = 0; j < n; j++) {
        double tmp = M[col * n + j];
        M[col * n + j] = M[pivot * n + j];
        M[pivot * n + j] = tmp;
      }
      double tmp = b[col]; b[col] = b[pivot]; b[pivot] = tmp;
    }
    for (size_t row = col + 1; row < n; row++) {
      double factor = M[row * n + col] / M[col * n + col];
      for (size_t j = col; j < n; j++)
        M[row * n + j] -= factor * M[col * n + j];
      b[row] -= factor * b[col];
    }
  }
  for (int i = (int)n - 1; i >= 0; i--) {
    double s = b[i];
    for (size_t j = (size_t)(i + 1); j < n; j++)
      s -= M[(size_t)i * n + j] * b[j];
    b[(size_t)i] = s / M[(size_t)i * n + (size_t)i];
  }
  return true;
}

/**
 * Padé (3,3) approximation: result ≈ expm(A·dt)·v.
 * Falls back to v unchanged if D(X) is numerically singular.
 */
static void expm_pade33(const double *A, const double *v, double *result,
                        size_t n, double dt) {
  double *X   = malloc(n * n * sizeof(double));
  double *X2  = malloc(n * n * sizeof(double));
  double *X3  = malloc(n * n * sizeof(double));
  double *Nmat = malloc(n * n * sizeof(double));
  double *Dmat = malloc(n * n * sizeof(double));
  double *rhs  = malloc(n * sizeof(double));

  if (!X || !X2 || !X3 || !Nmat || !Dmat || !rhs) {
    memcpy(result, v, n * sizeof(double));
    free(X); free(X2); free(X3); free(Nmat); free(Dmat); free(rhs);
    return;
  }

  /* X = A·dt */
  for (size_t i = 0; i < n * n; i++) X[i] = A[i] * dt;

  /* X² = X·X, X³ = X²·X */
  mat_mat(X, X, X2, n);
  mat_mat(X2, X, X3, n);

  /* N = 120I + 60X + 12X² + X³; D = 120I − 60X + 12X² − X³ */
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      double diag = (i == j) ? 1.0 : 0.0;
      Nmat[i * n + j] = 120.0 * diag + 60.0 * X[i * n + j]
                      + 12.0 * X2[i * n + j] + X3[i * n + j];
      Dmat[i * n + j] = 120.0 * diag - 60.0 * X[i * n + j]
                      + 12.0 * X2[i * n + j] - X3[i * n + j];
    }
  }

  /* rhs = N·v; solve D·result = rhs */
  mat_vec(Nmat, v, rhs, n);
  if (!gaussian_solve(Dmat, rhs, n))
    memcpy(result, v, n * sizeof(double)); /* singular fallback */
  else
    memcpy(result, rhs, n * sizeof(double));

  free(X); free(X2); free(X3); free(Nmat); free(Dmat); free(rhs);
}

/**
 * RK4 step using instance scratchpads (fast path, not re-entrant).
 * Writes result to Q_out. Q_out may alias inst->tmp_state.
 */
static void rk4_step_ex(GSSK_Instance *inst, const double *Q_in,
                        double *Q_out, double dt) {
  size_t n = inst->node_count;
  /* k1 → inst->dQ */
  compute_derivatives(inst, Q_in, inst->dQ);
  /* k2 staging → inst->tmp_state, k2 → inst->k2 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = Q_in[i] + 0.5 * dt * inst->dQ[i];
  compute_derivatives(inst, inst->tmp_state, inst->k2);
  /* k3 staging → inst->tmp_state, k3 → inst->k3 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = Q_in[i] + 0.5 * dt * inst->k2[i];
  compute_derivatives(inst, inst->tmp_state, inst->k3);
  /* k4 staging → inst->tmp_state, k4 → inst->k4 */
  for (size_t i = 0; i < n; i++)
    inst->tmp_state[i] = Q_in[i] + dt * inst->k3[i];
  compute_derivatives(inst, inst->tmp_state, inst->k4);
  /* final (reads dQ/k2/k3/k4 only — safe even when Q_out == inst->tmp_state) */
  for (size_t i = 0; i < n; i++)
    Q_out[i] = Q_in[i] + (dt / 6.0) * (inst->dQ[i] + 2.0 * inst->k2[i]
                                       + 2.0 * inst->k3[i] + inst->k4[i]);
}

/**
 * RK4 step with heap-allocated scratchpads (safe for re-entrant calls
 * from find_threshold_crossing while inst scratchpads hold RK4 result).
 */
static void rk4_step_alloc(GSSK_Instance *inst, const double *Q_in,
                            double *Q_out, double dt) {
  size_t n = inst->node_count;
  double *buf = malloc(5 * n * sizeof(double));
  if (!buf) { memcpy(Q_out, Q_in, n * sizeof(double)); return; }
  double *k1  = buf;
  double *k2  = buf +     n;
  double *k3  = buf + 2 * n;
  double *k4  = buf + 3 * n;
  double *tmp = buf + 4 * n;
  compute_derivatives(inst, Q_in, k1);
  for (size_t i = 0; i < n; i++) tmp[i] = Q_in[i] + 0.5 * dt * k1[i];
  compute_derivatives(inst, tmp, k2);
  for (size_t i = 0; i < n; i++) tmp[i] = Q_in[i] + 0.5 * dt * k2[i];
  compute_derivatives(inst, tmp, k3);
  for (size_t i = 0; i < n; i++) tmp[i] = Q_in[i] + dt * k3[i];
  compute_derivatives(inst, tmp, k4);
  for (size_t i = 0; i < n; i++)
    Q_out[i] = Q_in[i] + (dt / 6.0) * (k1[i] + 2.0*k2[i] + 2.0*k3[i] + k4[i]);
  free(buf);
}

/**
 * Returns true if the entire active network is a single isolated interaction
 * duet suitable for Riccati closed-form integration.
 * Conditions: exactly one active edge, logic=interaction, target==control,
 * both nodes are storage type.
 */
static bool network_is_isolated_duet(GSSK_Instance *inst, size_t *out_edge,
                                      int *out_A, int *out_B) {
  size_t active_count = 0;
  size_t di = 0;
  for (size_t i = 0; i < inst->edge_count; i++) {
    if (inst->edges[i].active) { active_count++; di = i; }
  }
  if (active_count != 1) return false;
  GSSK_EdgeInternal *e = &inst->edges[di];
  if (e->logic != GSSK_LOGIC_INTERACTION) return false;
  if (e->target_idx != e->control_idx)   return false;
  if (inst->nodes[e->origin_idx].type != NODE_STORAGE) return false;
  if (inst->nodes[e->target_idx].type  != NODE_STORAGE) return false;
  if (out_edge) *out_edge = di;
  if (out_A)    *out_A    = e->origin_idx;
  if (out_B)    *out_B    = e->target_idx;
  return true;
}

/**
 * IDC step: Q(t+dt) ≈ expm(A·dt)·Q_in + dt·f.
 * Uses Riccati exact duet if the network is an isolated pair,
 * otherwise Padé (3,3) for the full flow matrix.
 * Writes to Q_out (must not alias inst scratchpads).
 */
static void idc_step_ex(GSSK_Instance *inst, const double *Q_in,
                        double *Q_out, double dt) {
  size_t n = inst->node_count;

  /* Riccati exact duet shortcut */
  size_t di; int A_idx, B_idx;
  if (network_is_isolated_duet(inst, &di, &A_idx, &B_idx)) {
    GSSK_EdgeInternal *e = &inst->edges[di];
    double QA0 = Q_in[A_idx], QB0 = Q_in[B_idx];
    double S = QA0 + QB0;
    double denom = QA0 + QB0 * exp(e->k * S * dt);
    memcpy(Q_out, Q_in, n * sizeof(double));
    Q_out[A_idx] = (fabs(denom) > 1e-15) ? S * QA0 / denom : QA0;
    Q_out[B_idx] = S - Q_out[A_idx];
    return;
  }

  /* Padé (3,3) path */
  double *A = malloc(n * n * sizeof(double));
  double *f = malloc(n * sizeof(double));
  if (!A || !f) {
    memcpy(Q_out, Q_in, n * sizeof(double));
    free(A); free(f);
    return;
  }
  build_flow_matrix(inst, Q_in, A);
  build_forcing_vector(inst, Q_in, f);

  /* Phase 7 — treat processing-node contributions as additive forcing this
   * step (linearised about Q_in).  Lets IDC reproduce RK4 for v4 models. */
  {
    double *pf = calloc(n, sizeof(double));
    if (pf) {
      for (size_t i = 0; i < inst->node_count; i++) {
        switch (inst->nodes[i].type) {
        case NODE_INTERACTION:  compute_interaction_node(inst, i, Q_in, pf); break;
        case NODE_GAIN:         compute_gain_node(inst, i, Q_in, pf); break;
        case NODE_LOOP_LIMITED: compute_loop_limited_node(inst, i, Q_in, pf); break;
        case NODE_SWITCH:       compute_switch_node(inst, i, Q_in, pf); break;
        case NODE_EXCHANGE:     compute_exchange_node(inst, i, Q_in, pf); break;
        default: break;
        }
      }
      /* Don't accumulate into source/constant/processing rows */
      for (size_t i = 0; i < n; i++) {
        if (inst->nodes[i].type == NODE_SOURCE ||
            inst->nodes[i].type == NODE_CONSTANT ||
            is_processing_node(inst->nodes[i].type))
          pf[i] = 0.0;
        f[i] += pf[i];
      }
      free(pf);
    }
  }

  expm_pade33(A, Q_in, Q_out, n, dt);
  for (size_t i = 0; i < n; i++)
    Q_out[i] += dt * f[i];
  free(A); free(f);
}

/**
 * Compute flow on a single edge given a state vector.
 */
static double compute_edge_flow(const GSSK_EdgeInternal *e,
                                const double *state) {
  double Q = state[e->origin_idx];
  switch (e->logic) {
  case GSSK_LOGIC_CONSTANT:    return e->k;
  case GSSK_LOGIC_LINEAR:      return e->k * Q;
  case GSSK_LOGIC_INTERACTION:
    if (e->control_idx != -1) return e->k * Q * state[e->control_idx];
    return 0.0;
  case GSSK_LOGIC_LIMIT: {
    double C = -1.0;
    if (e->control_idx != -1) C = state[e->control_idx];
    else if (e->threshold > 0.0) C = e->threshold;
    if (C > 1e-9) return (e->k * Q) / (1.0 + Q / C);
    return 0.0;
  }
  case GSSK_LOGIC_THRESHOLD:
    return (Q > e->threshold) ? e->k : 0.0;
  }
  return 0.0;
}

/**
 * Compute per-edge relative error between IDC and RK4 flows.
 * Updates inst->edge_error[] and inst->step_error.
 */
static void compute_per_edge_errors(GSSK_Instance *inst,
                                    const double *Q_rk4,
                                    const double *Q_idc) {
  inst->step_error = 0.0;
  for (size_t i = 0; i < inst->edge_count; i++) {
    if (!inst->edge_error) break;
    GSSK_EdgeInternal *e = &inst->edges[i];
    if (!e->active) { inst->edge_error[i] = 0.0; continue; }
    /* Phase 7: edges touching processing nodes do not have a meaningful
     * per-edge flow (they're topology hints); processing flow is computed
     * inside the per-type helper and folded into IDC forcing.  Excluding
     * them here keeps step_error well-defined. */
    if (is_processing_node(inst->nodes[e->origin_idx].type) ||
        is_processing_node(inst->nodes[e->target_idx].type)) {
      inst->edge_error[i] = 0.0;
      continue;
    }
    double fr = compute_edge_flow(e, Q_rk4);
    double fi = compute_edge_flow(e, Q_idc);
    double denom = fabs(fr) > 1e-12 ? fabs(fr) : 1e-12;
    inst->edge_error[i] = fabs(fi - fr) / denom;
    if (inst->edge_error[i] > inst->step_error)
      inst->step_error = inst->edge_error[i];
  }
}

/**
 * Append an event to the event log, growing the log as needed.
 */
static void emit_event(GSSK_Instance *inst, double t,
                       const char *edge_id, int dir) {
  if (inst->event_count >= inst->event_capacity) {
    size_t new_cap = inst->event_capacity ? inst->event_capacity * 2 : 8;
    GSSK_EventInternal *ev = realloc(inst->events,
        new_cap * sizeof(GSSK_EventInternal));
    if (!ev) return;
    inst->events = ev;
    inst->event_capacity = new_cap;
  }
  GSSK_EventInternal *ev = &inst->events[inst->event_count++];
  ev->t = t;
  strncpy(ev->edge_id, edge_id ? edge_id : "", 63);
  ev->edge_id[63] = '\0';
  ev->direction = dir;
}

/**
 * Illinois algorithm to find the time t* ∈ (0, dt) when Q_origin crosses
 * the threshold for edge edge_idx. Returns t* and populates Q_cross.
 * Uses rk4_step_alloc (heap-based, safe to call while inst scratchpads
 * hold the main RK4 result).
 *
 * Q_at_dt: pre-computed state at t=dt (pass NULL to compute internally).
 *   Providing it avoids one redundant rk4_step_alloc call when the caller
 *   already has the endpoint (e.g. do_threshold_substep).
 */
static double find_threshold_crossing(GSSK_Instance *inst,
    const double *Q_before, double dt, size_t edge_idx, double *Q_cross,
    const double *Q_at_dt) {
  GSSK_EdgeInternal *e = &inst->edges[edge_idx];
  int orig = e->origin_idx;
  double thr = e->threshold;
  size_t n = inst->node_count;

  double fa = Q_before[orig] - thr;
  double a = 0.0, b = dt;

  double *Q_b = malloc(n * sizeof(double));
  double *Q_m = malloc(n * sizeof(double));
  if (!Q_b || !Q_m) {
    free(Q_b); free(Q_m);
    memcpy(Q_cross, Q_before, n * sizeof(double));
    return 0.0;
  }

  if (Q_at_dt)
    memcpy(Q_b, Q_at_dt, n * sizeof(double));
  else
    rk4_step_alloc(inst, Q_before, Q_b, dt);
  double fb = Q_b[orig] - thr;

  double mid = 0.5 * (a + b);
  int last_side = 0;

  for (int iter = 0; iter < 64; iter++) {
    /* False-position step */
    double denom_fp = fb - fa;
    mid = (fabs(denom_fp) > 1e-15)
          ? a + (b - a) * (-fa) / denom_fp
          : 0.5 * (a + b);

    rk4_step_alloc(inst, Q_before, Q_m, mid);
    double fm = Q_m[orig] - thr;

    if (fabs(fm) < 1e-10 || (b - a) < 1e-12 * dt) break;

    if (fm * fa < 0.0) {
      b = mid; fb = fm;
      if (last_side == 1) fa *= 0.5; /* Illinois modification */
      last_side = 1;
    } else {
      a = mid; fa = fm;
      if (last_side == 2) fb *= 0.5;
      last_side = 2;
    }
  }

  memcpy(Q_cross, Q_m, n * sizeof(double));
  free(Q_b); free(Q_m);
  return mid;
}

/**
 * Sub-stepping threshold event handler (Phase 1.3 — full implementation).
 *
 * Handles all three cases:
 *   1. Simultaneous crossings  — emits all events whose times fall within a
 *      tolerance band of the earliest crossing, then advances to that point.
 *   2. Degenerate/tangent      — skips edges whose origin is already within
 *      eps of the threshold (prevents spurious re-detection after a crossing).
 *   3. Sequential sub-stepping — restarts the integrator from each crossing
 *      point and continues for the remaining sub-interval, up to
 *      GSSK_MAX_EVENTS_PER_STEP iterations.
 *
 * Parameters:
 *   Q_before    — state at the start of the full step (= inst->state)
 *   Q_full_step — pre-computed full RK4 result (= inst->tmp_state); used
 *                 as the trial endpoint in the first iteration to avoid
 *                 a redundant rk4_step_alloc call.  May equal Q_out.
 *   dt          — full step size
 *   t_base      — absolute time at the start of the step
 *   Q_out       — where to write the final sub-stepped state.
 *                 Safe to alias Q_full_step (copy happens before any write).
 */
static void do_threshold_substep(GSSK_Instance *inst,
    const double *Q_before, const double *Q_full_step,
    double dt, double t_base, double *Q_out)
{
  size_t n = inst->node_count;
  /* relative tolerance for the degenerate-start guard */
  const double eps_thr = 1e-12;
  /* crossing times within this fraction of t_rem are treated as simultaneous */
  const double simul_tol = 1e-10;

  double *Q_cur     = malloc(n * sizeof(double));
  double *Q_trial   = malloc(n * sizeof(double));
  double *Q_cross_t = malloc(n * sizeof(double)); /* tmp per-edge Q at crossing */
  double *Q_cross_b = malloc(n * sizeof(double)); /* Q at earliest crossing */
  if (!Q_cur || !Q_trial || !Q_cross_t || !Q_cross_b) {
    free(Q_cur); free(Q_trial); free(Q_cross_t); free(Q_cross_b);
    return; /* Q_out already holds full-step result; caller keeps it */
  }

  memcpy(Q_cur, Q_before, n * sizeof(double));
  double t_elapsed = 0.0;

  for (int iter = 0; iter < GSSK_MAX_EVENTS_PER_STEP; iter++) {
    double t_rem = dt - t_elapsed;
    if (t_rem < 1e-14 * dt) break;

    /* trial step — reuse caller's pre-computed result on the first iteration */
    if (iter == 0 && Q_full_step != NULL)
      memcpy(Q_trial, Q_full_step, n * sizeof(double));
    else
      rk4_step_alloc(inst, Q_cur, Q_trial, t_rem);

    /* --- scan all threshold edges for crossings in [0, t_rem] ------------ */
    struct { size_t ei; double tc; int dir; } ci[GSSK_MAX_EVENTS_PER_STEP];
    size_t n_ci = 0;
    double best_tc = t_rem + 1.0; /* sentinel larger than any valid crossing */

    for (size_t ei = 0; ei < inst->edge_count; ei++) {
      GSSK_EdgeInternal *e = &inst->edges[ei];
      if (!e->active || e->logic != GSSK_LOGIC_THRESHOLD) continue;

      int orig = e->origin_idx;
      double thr = e->threshold;

      /* Degenerate guard: skip if origin already sits on the threshold.
       * This prevents spurious re-detection immediately after a crossing. */
      double eps = eps_thr * (1.0 + fabs(thr));
      if (fabs(Q_cur[orig] - thr) < eps) continue;

      bool was_above = Q_cur[orig]   > thr;
      bool is_above  = Q_trial[orig] > thr;
      if (was_above == is_above) continue; /* no sign change — no crossing */

      /* Illinois refinement; pass Q_trial as pre-computed endpoint */
      double tc = find_threshold_crossing(inst, Q_cur, t_rem, ei,
                                          Q_cross_t, Q_trial);
      if (n_ci < GSSK_MAX_EVENTS_PER_STEP) {
        ci[n_ci].ei  = ei;
        ci[n_ci].tc  = tc;
        ci[n_ci].dir = is_above ? +1 : -1;
        n_ci++;
        if (tc < best_tc) {
          best_tc = tc;
          /* save state at earliest crossing so we can advance to it */
          memcpy(Q_cross_b, Q_cross_t, n * sizeof(double));
        }
      }
    }

    if (n_ci == 0) {
      /* No crossing in this sub-interval — take the full trial step and stop */
      memcpy(Q_cur, Q_trial, n * sizeof(double));
      break;
    }

    /* Emit all events at or near the earliest crossing time.
     * "Near" = within simul_tol * t_rem (handles floating-point simultaneity). */
    double tol = simul_tol * t_rem + 1e-14;
    for (size_t c = 0; c < n_ci; c++) {
      if (ci[c].tc <= best_tc + tol) {
        GSSK_EdgeInternal *e = &inst->edges[ci[c].ei];
        emit_event(inst,
                   t_base + t_elapsed + ci[c].tc,
                   e->id[0] ? e->id : NULL,
                   ci[c].dir);
      }
    }

    /* Restart integrator from the crossing point */
    memcpy(Q_cur, Q_cross_b, n * sizeof(double));
    t_elapsed += best_tc;
  }

  memcpy(Q_out, Q_cur, n * sizeof(double));
  free(Q_cur); free(Q_trial); free(Q_cross_t); free(Q_cross_b);
}

/* =========================================================================
 * Phase 2 — Dormand-Prince 5(4) adaptive solver
 *
 * DOPRI5 Butcher tableau (Dormand & Prince 1980).  6 function evaluations
 * per step (FSAL not exploited here; k7 used only for the error estimate).
 * 5th-order propagated solution; embedded 4th-order for error estimation.
 * ========================================================================= */

/**
 * Perform one DOPRI5 step from Q_in, writing the 5th-order solution into
 * inst->tmp_state (= Q_out) and the error vector into Q_err.
 * Uses inst scratchpads: dQ (k1), k2..k4 (existing), k5/k6/k7 (Phase 2).
 * Stages are computed through inst->tmp_state; Q_out must equal inst->tmp_state.
 */
static void dopri5_step(GSSK_Instance *inst, const double *Q_in,
                        double *Q_out, double *Q_err, double h) {
  size_t n = inst->node_count;
  double *k1 = inst->dQ;      /* stage 1 derivative */
  double *k2 = inst->k2;
  double *k3 = inst->k3;
  double *k4 = inst->k4;
  double *k5 = inst->k5;
  double *k6 = inst->k6;
  double *k7 = inst->k7;
  double *s  = inst->tmp_state; /* staging buffer (= Q_out) */

  /* Stage 1 */
  compute_derivatives(inst, Q_in, k1);

  /* Stage 2 at c2 = 1/5 */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (1.0/5.0 * k1[i]);
  compute_derivatives(inst, s, k2);

  /* Stage 3 at c3 = 3/10 */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (3.0/40.0 * k1[i] + 9.0/40.0 * k2[i]);
  compute_derivatives(inst, s, k3);

  /* Stage 4 at c4 = 4/5 */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (44.0/45.0 * k1[i] - 56.0/15.0 * k2[i]
                          + 32.0/9.0  * k3[i]);
  compute_derivatives(inst, s, k4);

  /* Stage 5 at c5 = 8/9 */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (19372.0/6561.0  * k1[i] - 25360.0/2187.0 * k2[i]
                          + 64448.0/6561.0 * k3[i] -   212.0/ 729.0 * k4[i]);
  compute_derivatives(inst, s, k5);

  /* Stage 6 at c6 = 1 */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (9017.0/3168.0 * k1[i] -  355.0/33.0    * k2[i]
                          + 46732.0/5247.0 * k3[i] +  49.0/176.0   * k4[i]
                          - 5103.0/18656.0 * k5[i]);
  compute_derivatives(inst, s, k6);

  /* 5th-order propagated solution → s (= Q_out = inst->tmp_state) */
  for (size_t i = 0; i < n; i++)
    s[i] = Q_in[i] + h * (35.0/384.0    * k1[i]
                          + 500.0/1113.0 * k3[i]
                          + 125.0/192.0  * k4[i]
                          - 2187.0/6784.0* k5[i]
                          + 11.0/84.0    * k6[i]);

  /* Stage 7 at c7 = 1 (derivative at accepted solution; needed for error) */
  compute_derivatives(inst, s, k7);

  if (Q_out != s) memcpy(Q_out, s, n * sizeof(double));

  /* Error estimate e = b5 − b4 applied to stages k1..k7 */
  for (size_t i = 0; i < n; i++)
    Q_err[i] = h * (  71.0/57600.0   * k1[i]
                    -  71.0/16695.0   * k3[i]
                    +  71.0/1920.0    * k4[i]
                    - 17253.0/339200.0* k5[i]
                    +  22.0/525.0     * k6[i]
                    -   1.0/40.0      * k7[i]);
}

/** WRMSE error norm scaled by mixed absolute/relative tolerance. */
static double dopri5_err_norm(const double *Q_in, const double *Q_out,
                               const double *Q_err, size_t n,
                               double atol, double rtol) {
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    double sc = atol + rtol * fmax(fabs(Q_in[i]), fabs(Q_out[i]));
    double e  = sc > 0.0 ? Q_err[i] / sc : 0.0;
    sum += e * e;
  }
  return sqrt(sum / (n > 0 ? n : 1));
}

/**
 * I-controller step-size proposal for DOPRI5 (order 5).
 * h_new = h * safety * (1/err_norm)^(1/5), clamped to [h_min, h_max].
 * If err_norm ≈ 0 the step is essentially exact; scale up by FACMAX.
 */
static double dopri5_new_h(double err_norm, double h,
                            double h_min, double h_max) {
  const double FAC    = 0.9;
  const double FACMAX = 5.0;
  const double FACMIN = 0.2;
  double factor = (err_norm > 1e-14) ? FAC * pow(1.0 / err_norm, 0.2)
                                     : FACMAX;
  factor = fmin(FACMAX, fmax(FACMIN, factor));
  double h_new = h * factor;
  if (h_min > 0.0) h_new = fmax(h_new, h_min);
  if (h_max > 0.0) h_new = fmin(h_new, h_max);
  return h_new;
}

/**
 * Gershgorin circle bound on the spectral radius of the flow matrix A.
 * λ_bound = max_i ( |A[i][i]| + Σ_{j≠i} |A[i][j]| ).
 * Used as a heuristic for h_max: h_max ≤ 3.5 / λ_bound.
 * Returns 0 if allocation fails or all entries are zero.
 */
static double gershgorin_spectral_bound(GSSK_Instance *inst,
                                         const double *state) {
  size_t n = inst->node_count;
  double *A = calloc(n * n, sizeof(double));
  if (!A) return 0.0;
  build_flow_matrix(inst, state, A);
  double lam = 0.0;
  for (size_t i = 0; i < n; i++) {
    double row = fabs(A[i * n + i]);
    for (size_t j = 0; j < n; j++)
      if (j != i) row += fabs(A[i * n + j]);
    if (row > lam) lam = row;
  }
  free(A);
  return lam;
}

/**
 * Relative conservation error for a fully closed system.
 * Returns |ΣQ_after − ΣQ_before| / max(|ΣQ_before|, 1e-15).
 * Only storage nodes contribute to the sum.
 * Returns 0 if any non-storage active node exists (open system).
 */
static double closed_system_conservation_error(GSSK_Instance *inst,
                                                const double *Q_before,
                                                const double *Q_after) {
  for (size_t i = 0; i < inst->node_count; i++) {
    if (inst->nodes[i].active && inst->nodes[i].type != NODE_STORAGE)
      return 0.0; /* open system — skip */
  }
  double sum_b = 0.0, sum_a = 0.0;
  for (size_t i = 0; i < inst->node_count; i++) {
    if (inst->nodes[i].active) {
      sum_b += Q_before[i];
      sum_a += Q_after[i];
    }
  }
  double ref = fmax(fabs(sum_b), 1e-15);
  return fabs(sum_a - sum_b) / ref;
}

/* Phase 5: compute per-carrier conservation error into inst->carrier_cons_error[]. */
static void update_carrier_conservation_errors(GSSK_Instance *inst,
                                                const double *Q_before,
                                                const double *Q_after) {
  if (!inst->carrier_count || !inst->carrier_cons_error) return;
  for (size_t ci = 0; ci < inst->carrier_count; ci++) {
    if (!inst->carriers[ci].conserved) {
      inst->carrier_cons_error[ci] = 0.0;
      continue;
    }
    double sum_b = 0.0, sum_a = 0.0;
    for (size_t ni = 0; ni < inst->node_count; ni++) {
      if (!inst->nodes[ni].active || inst->nodes[ni].type != NODE_STORAGE) continue;
      if (strcmp(inst->nodes[ni].carrier, inst->carriers[ci].id) != 0) continue;
      sum_b += Q_before[ni];
      sum_a += Q_after[ni];
    }
    double ref = fmax(fabs(sum_b), 1e-15);
    inst->carrier_cons_error[ci] = fabs(sum_a - sum_b) / ref;
  }
}

/**
 * Inner adaptive step: advance inst->state by EXACTLY target_dt using
 * repeated DOPRI5 sub-steps with PI step control.
 *
 * t_abs_base — absolute simulation time at the entry of this call (used for
 *   event timing); equals inst->current_t for the top-level GSSK_Step call
 *   and inst->current_t + partial_elapsed for nested GSSK_StepAdaptive calls.
 *
 * After return:
 *   inst->state   — advanced by target_dt
 *   inst->h_next  — suggested h for the next call
 *   inst->h_last  — h used in the final accepted sub-step
 *   inst->conservation_error — relative total-Q change
 */
static GSSK_Status adaptive_step_ex(GSSK_Instance *inst,
                                     double target_dt, double t_abs_base) {
  size_t n = inst->node_count;
  double atol = inst->config.abs_tol;
  double rtol = inst->config.rel_tol;

  double h_min = inst->config.h_min > 0.0 ? inst->config.h_min
                                           : target_dt * 1e-6;
  double h_max = inst->config.h_max > 0.0 ? inst->config.h_max : target_dt;

  /* Spectral-radius heuristic for h_max */
  if (inst->config.h_max == 0.0) {
    double lam = gershgorin_spectral_bound(inst, inst->state);
    if (lam > 1e-15)
      h_max = fmin(h_max, 3.5 / lam);
  }

  /* Initial step suggestion */
  double h = inst->h_next > 0.0 ? inst->h_next : target_dt;
  h = fmin(h, h_max);
  h = fmax(h, h_min);

  double *Q_err    = malloc(n * sizeof(double));
  double *Q_before = malloc(n * sizeof(double));
  if (!Q_err || !Q_before) { free(Q_err); free(Q_before); return GSSK_ERR_UNKNOWN; }
  memcpy(Q_before, inst->state, n * sizeof(double));

  double t_elapsed = 0.0;
  GSSK_Status ret  = GSSK_SUCCESS;

  for (int substep = 0; substep < 100000; substep++) {
    double t_rem = target_dt - t_elapsed;
    if (t_rem <= 1e-14 * target_dt) break;

    /* Cap to remaining interval */
    if (h > t_rem) h = t_rem;

    /* DOPRI5 trial step → inst->tmp_state; error → Q_err */
    dopri5_step(inst, inst->state, inst->tmp_state, Q_err, h);

    double err_norm = dopri5_err_norm(inst->state, inst->tmp_state, Q_err,
                                       n, atol, rtol);

    double h_new = dopri5_new_h(err_norm, h, h_min, h_max);

    bool accepted = (err_norm <= 1.0) || (h <= h_min * (1.0 + 1e-6));

    if (accepted) {
      /* Threshold event detection integrated with adaptive step */
      bool has_threshold = false;
      for (size_t ei = 0; ei < inst->edge_count && !has_threshold; ei++)
        has_threshold = inst->edges[ei].active &&
                        inst->edges[ei].logic == GSSK_LOGIC_THRESHOLD;
      if (has_threshold)
        do_threshold_substep(inst, inst->state, inst->tmp_state,
                             h, t_abs_base + t_elapsed, inst->tmp_state);

      memcpy(inst->state, inst->tmp_state, n * sizeof(double));

      /* Non-negativity clamp per sub-step */
      for (size_t i = 0; i < n; i++)
        if (inst->state[i] < 0.0) inst->state[i] = 0.0;

      /* Divergence check */
      for (size_t i = 0; i < n; i++) {
        if (isnan(inst->state[i]) || isinf(inst->state[i])) {
          free(Q_err); free(Q_before);
          return GSSK_ERR_DIVERGENCE;
        }
      }

      inst->h_last = h;
      t_elapsed   += h;

      if (inst->diag_hooks.on_step)
        inst->diag_hooks.on_step(inst->diag_hooks.ctx,
                                  t_abs_base + t_elapsed, h, err_norm);

      if (err_norm > 1.0) ret = GSSK_WARN_SOLVER_DIVERGENCE; /* forced h_min accept */
    }

    h = h_new;
  }

  inst->h_next = h;

  /* Conservation diagnostic over full target_dt */
  inst->conservation_error = closed_system_conservation_error(
      inst, Q_before, inst->state);
  if (inst->conservation_error > rtol &&
      inst->diag_hooks.on_conservation_warning)
    inst->diag_hooks.on_conservation_warning(inst->diag_hooks.ctx,
        t_abs_base + target_dt, inst->conservation_error);

  /* Phase 5 — per-carrier conservation errors */
  update_carrier_conservation_errors(inst, Q_before, inst->state);

  free(Q_err);
  free(Q_before);
  return ret;
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
    case GSSK_LOGIC_LIMIT: {
      double C = -1.0;
      if (e->control_idx != -1) C = state[e->control_idx];
      else if (e->threshold > 0.0) C = e->threshold;
      if (C > 1e-9) f = (e->k * Q) / (1.0 + Q / C);
      break;
    }
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
 * Phase 3 — Sensitivity Analysis helpers
 *
 * 3.1 Forward sensitivity: augmented ODE dS/dt = J·S + B, where
 *   J[i][j] = ∂(dQ_i/dt)/∂Q_j  (n×n Jacobian of flow)
 *   B[i][j] = ∂(dQ_i/dt)/∂k_j  (n×m parameter derivative matrix)
 *   S[i][j] = ∂Q_i/∂k_j         (n×m sensitivity matrix, maintained in inst)
 *
 * 3.2 Adjoint: backward integration of λ satisfying dλ/dt = −Jᵀ·λ,
 *   giving gradient ∂L/∂k_j = ∫ λᵀ · B_j dt via quadrature.
 *
 * 3.3 Transformity sensitivity via implicit differentiation:
 *   M·(∂Tr/∂k_j) = (∂F/∂k_j)·Tr, where M=(I−F) is the quality system matrix.
 * ========================================================================= */

/**
 * Build n×n Jacobian J[i][j] = ∂(dQ_i/dt)/∂Q_j at given state.
 * Only storage nodes have non-zero rows; source/constant rows zeroed.
 */
static void build_jacobian(GSSK_Instance *inst, const double *state, double *J) {
  size_t n = inst->node_count;
  memset(J, 0, n * n * sizeof(double));

  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    int orig = e->origin_idx;
    int tgt  = e->target_idx;
    int ctrl = e->control_idx;
    double Q = state[orig];
    double dF_dQ_orig = 0.0, dF_dQ_ctrl = 0.0;

    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:
      break;
    case GSSK_LOGIC_LINEAR:
      dF_dQ_orig = e->k;
      break;
    case GSSK_LOGIC_INTERACTION:
      if (ctrl >= 0) {
        dF_dQ_orig = e->k * state[ctrl];
        dF_dQ_ctrl = e->k * Q;
      }
      break;
    case GSSK_LOGIC_LIMIT: {
      double C = -1.0;
      if (ctrl >= 0) C = state[ctrl];
      else if (e->threshold > 0.0) C = e->threshold;
      if (C > 1e-9) {
        double d = C + Q;
        dF_dQ_orig = e->k * C * C / (d * d);
        if (ctrl >= 0)
          dF_dQ_ctrl = e->k * Q * Q / (d * d);
      }
      break;
    }
    case GSSK_LOGIC_THRESHOLD:
      break; /* step function → 0 derivative almost everywhere */
    }

    J[(size_t)orig * n + (size_t)orig] -= dF_dQ_orig;
    J[(size_t)tgt  * n + (size_t)orig] += dF_dQ_orig;
    if (ctrl >= 0 && (dF_dQ_ctrl != 0.0)) {
      J[(size_t)orig * n + (size_t)ctrl] -= dF_dQ_ctrl;
      J[(size_t)tgt  * n + (size_t)ctrl] += dF_dQ_ctrl;
    }
  }

  /* Phase 7 — processing node Jacobian contributions.
   * Each helper mirrors the flow computation in compute_*_node(). */
  for (size_t ni = 0; ni < n; ni++) {
    if (!inst->nodes[ni].active || !is_processing_node(inst->nodes[ni].type))
      continue;

    /* Count outputs once (shared by all per-type cases) */
    size_t out_count = 0;
    for (size_t ei = 0; ei < inst->edge_count; ei++) {
      GSSK_EdgeInternal *e = &inst->edges[ei];
      if (e->active && (size_t)e->origin_idx == ni) out_count++;
    }
    if (out_count == 0) continue;

    switch (inst->nodes[ni].type) {

    case NODE_INTERACTION: {
      /* F = node_k × ∏ Q_j over all inputs; energy_orig = first input */
      int in_orig[GSSK_MAX_ARCH_NODES]; size_t in_n = 0;
      int energy_orig = -1;
      double F = inst->nodes[ni].node_k;
      for (size_t ei = 0; ei < inst->edge_count && in_n < GSSK_MAX_ARCH_NODES; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->target_idx != ni) continue;
        if (in_n == 0) energy_orig = e->origin_idx;
        in_orig[in_n++] = e->origin_idx;
        F *= state[e->origin_idx];
      }
      if (in_n == 0) break;
      for (size_t ji = 0; ji < in_n; ji++) {
        int j = in_orig[ji];
        double dF_dQj = (fabs(state[j]) > 1e-12) ? F / state[j] : 0.0;
        if (energy_orig >= 0)
          J[(size_t)energy_orig * n + (size_t)j] -= dF_dQj;
        for (size_t ei = 0; ei < inst->edge_count; ei++) {
          GSSK_EdgeInternal *e = &inst->edges[ei];
          if (!e->active || (size_t)e->origin_idx != ni) continue;
          J[(size_t)e->target_idx * n + (size_t)j] += dF_dQj / (double)out_count;
        }
      }
      break;
    }

    case NODE_GAIN: {
      /* F = node_k × Q_control; optionally draws from energy source */
      int ctrl = -1, esrc = -1; size_t seen = 0;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->target_idx != ni) continue;
        if (seen == 0) ctrl = e->origin_idx;
        else if (seen == 1) esrc = e->origin_idx;
        seen++;
      }
      if (ctrl < 0) break;
      double dF = inst->nodes[ni].node_k;
      if (esrc >= 0 && inst->nodes[esrc].type == NODE_STORAGE)
        J[(size_t)esrc * n + (size_t)ctrl] -= dF;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->origin_idx != ni) continue;
        J[(size_t)e->target_idx * n + (size_t)ctrl] += dF / (double)out_count;
      }
      break;
    }

    case NODE_LOOP_LIMITED: {
      /* F = node_k × Q_in × C / (C + Q_in) */
      int in_orig = -1;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->target_idx != ni) continue;
        in_orig = e->origin_idx; break;
      }
      if (in_orig < 0) break;
      double Q_in = state[in_orig];
      double C = inst->nodes[ni].node_C > 1e-9 ? inst->nodes[ni].node_C : 1.0;
      double d = C + Q_in;
      double dF = inst->nodes[ni].node_k * C * C / (d * d);
      J[(size_t)in_orig * n + (size_t)in_orig] -= dF;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->origin_idx != ni) continue;
        J[(size_t)e->target_idx * n + (size_t)in_orig] += dF / (double)out_count;
      }
      break;
    }

    case NODE_SWITCH: {
      /* F = node_k × Q_flow when Q_sensor > threshold */
      int flow_o = -1, sens_o = -1; size_t seen = 0;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->target_idx != ni) continue;
        if (seen == 0) flow_o = e->origin_idx;
        else if (seen == 1) sens_o = e->origin_idx;
        seen++;
      }
      if (flow_o < 0) break;
      if (sens_o < 0) sens_o = flow_o;
      if (state[sens_o] <= inst->nodes[ni].node_threshold) break;
      double dF = inst->nodes[ni].node_k;
      J[(size_t)flow_o * n + (size_t)flow_o] -= dF;
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        GSSK_EdgeInternal *e = &inst->edges[ei];
        if (!e->active || (size_t)e->origin_idx != ni) continue;
        J[(size_t)e->target_idx * n + (size_t)flow_o] += dF / (double)out_count;
      }
      break;
    }

    case NODE_EXCHANGE: {
      /* F_goods = node_k × Q_goods_in (× Q_money_in if present).
       * Legs and price come from the shared primitive (ADR 0001) so this
       * Jacobian cannot disagree with compute_exchange_node about which edges
       * form the diamond or what it trades at. */
      ExchangeLegs L = resolve_exchange_legs(inst, ni);
      int gi = L.goods_in, mi = L.money_in, go = L.goods_out, mo = L.money_out;
      if (gi < 0) break;
      double Qg = state[gi];
      double Qm = (mi >= 0) ? state[mi] : 1.0;
      double k  = inst->nodes[ni].node_k;
      double p  = exchange_price(inst, ni);
      /* ∂/∂Q_goods_in */
      double dFg_dQg = k * Qm;
      J[(size_t)gi * n + (size_t)gi] -= dFg_dQg;
      if (go >= 0) J[(size_t)go * n + (size_t)gi] += dFg_dQg;
      if (mi >= 0) J[(size_t)mi * n + (size_t)gi] -= p * dFg_dQg;
      if (mo >= 0) J[(size_t)mo * n + (size_t)gi] += p * dFg_dQg;
      /* ∂/∂Q_money_in */
      if (mi >= 0) {
        double dFg_dQm = k * Qg;
        J[(size_t)gi * n + (size_t)mi] -= dFg_dQm;
        if (go >= 0) J[(size_t)go * n + (size_t)mi] += dFg_dQm;
        J[(size_t)mi * n + (size_t)mi]  -= p * dFg_dQm;
        if (mo >= 0) J[(size_t)mo * n + (size_t)mi] += p * dFg_dQm;
      }
      break;
    }

    default: break;
    }
  }

  for (size_t i = 0; i < n; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT)
      for (size_t j = 0; j < n; j++) J[i * n + j] = 0.0;
  }
}

/**
 * Compute n-vector ∂f/∂k_{edge_idx} at given state.
 * b[orig] -= dF/dk, b[tgt] += dF/dk.
 */
static void compute_param_deriv(GSSK_Instance *inst, const double *state,
                                 size_t edge_idx, double *b) {
  size_t n = inst->node_count;
  memset(b, 0, n * sizeof(double));
  if (edge_idx >= inst->edge_count) return;

  GSSK_EdgeInternal *e = &inst->edges[edge_idx];
  if (!e->active) return;
  int orig = e->origin_idx;
  int tgt  = e->target_idx;
  int ctrl = e->control_idx;
  double Q = state[orig];
  double dF_dk = 0.0;

  switch (e->logic) {
  case GSSK_LOGIC_CONSTANT:    dF_dk = 1.0; break;
  case GSSK_LOGIC_LINEAR:      dF_dk = Q;   break;
  case GSSK_LOGIC_INTERACTION:
    if (ctrl >= 0) dF_dk = Q * state[ctrl];
    break;
  case GSSK_LOGIC_LIMIT: {
    double C = -1.0;
    if (ctrl >= 0) C = state[ctrl];
    else if (e->threshold > 0.0) C = e->threshold;
    if (C > 1e-9) dF_dk = Q * C / (C + Q);
    break;
  }
  case GSSK_LOGIC_THRESHOLD:
    dF_dk = (Q > e->threshold) ? 1.0 : 0.0;
    break;
  }

  b[(size_t)orig] -= dF_dk;
  b[(size_t)tgt]  += dF_dk;
}

/**
 * One Euler step on the sensitivity matrix: S += dt·(J·S + B).
 * Called at the START of GSSK_Step with the pre-step state, so S and Q
 * advance together in a first-order explicit coupled scheme.
 */
static void sens_euler_step(GSSK_Instance *inst, const double *state,
                             double dt) {
  size_t n = inst->node_count;
  size_t m = inst->sens_param_count;
  if (m == 0 || !inst->sens_matrix || !inst->sens_param_idx) return;

  double *J  = malloc(n * n * sizeof(double));
  double *B  = malloc(n * m * sizeof(double));
  double *dS = malloc(n * m * sizeof(double));
  double *bj = malloc(n * sizeof(double));
  if (!J || !B || !dS || !bj) { free(J); free(B); free(dS); free(bj); return; }

  build_jacobian(inst, state, J);

  for (size_t j = 0; j < m; j++) {
    compute_param_deriv(inst, state, inst->sens_param_idx[j], bj);
    for (size_t i = 0; i < n; i++) B[i * m + j] = bj[i];
  }

  /* dS[i][j] = Σ_k J[i][k]·S[k][j] + B[i][j] */
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++) {
      double sum = B[i * m + j];
      for (size_t k = 0; k < n; k++)
        sum += J[i * n + k] * inst->sens_matrix[k * m + j];
      dS[i * m + j] = sum;
    }

  for (size_t ij = 0; ij < n * m; ij++)
    inst->sens_matrix[ij] += dt * dS[ij];

  free(J); free(B); free(dS); free(bj);
}

/**
 * Compute ∂Tr/∂k_{edge_idx} using implicit differentiation of M·Tr=b.
 * On return, out_dTr[n] holds ∂Tr[i]/∂k_j for all nodes i.
 * Requires quality accounting to be enabled and transformity to be valid.
 */
static void compute_quality_sensitivity(GSSK_Instance *inst, size_t edge_idx,
                                         double *out_dTr) {
  size_t n = inst->node_count;
  memset(out_dTr, 0, n * sizeof(double));
  if (!inst->quality_enabled || !inst->transformity) return;
  if (edge_idx >= inst->edge_count) return;

  GSSK_EdgeInternal *ej = &inst->edges[edge_idx];
  if (!ej->active) return;

  /* Recompute flows and outsums (same as quality pass) */
  double *flow   = calloc(inst->edge_count, sizeof(double));
  double *outsum = calloc(n, sizeof(double));
  if (!flow || !outsum) { free(flow); free(outsum); return; }

  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active) continue;
    double Q = inst->state[e->origin_idx];
    double f = 0.0;
    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:    f = e->k; break;
    case GSSK_LOGIC_LINEAR:      f = e->k * Q; break;
    case GSSK_LOGIC_INTERACTION:
      if (e->control_idx != -1) f = e->k * Q * inst->state[e->control_idx];
      break;
    case GSSK_LOGIC_LIMIT: {
      double C = -1.0;
      if (e->control_idx != -1) C = inst->state[e->control_idx];
      else if (e->threshold > 0.0) C = e->threshold;
      if (C > 1e-9) f = (e->k * Q) / (1.0 + Q / C);
      break;
    }
    case GSSK_LOGIC_THRESHOLD:
      f = (Q > e->threshold) ? e->k : 0.0;
      break;
    }
    flow[ei] = f > 0.0 ? f : 0.0;
    outsum[e->origin_idx] += flow[ei];
  }

  /* Build M = (I - F) matrix — same as quality pass */
  double *M = calloc(n * n, sizeof(double));
  double *b = calloc(n, sizeof(double));       /* quality boundary conditions */
  double *rhs = calloc(n, sizeof(double));     /* RHS for sensitivity solve */
  if (!M || !b || !rhs) {
    free(flow); free(outsum); free(M); free(b); free(rhs); return;
  }

  for (size_t i = 0; i < n; i++) {
    M[i * n + i] = 1.0;
    if (inst->nodes[i].type == NODE_SOURCE || inst->nodes[i].type == NODE_CONSTANT)
      b[i] = inst->nodes[i].quality_input;
  }
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active || flow[ei] <= 0.0) continue;
    int orig = e->origin_idx, tgt = e->target_idx;
    double frac = (e->output_mode == OUTPUT_REPLICATE)
                  ? 1.0
                  : ((outsum[orig] > 1e-12) ? flow[ei] / outsum[orig] : 0.0);
    M[(size_t)tgt * n + (size_t)orig] -= frac;
  }

  /* Build RHS = (∂F/∂k_j)·Tr for the target edge j */
  int ej_orig = ej->origin_idx, ej_tgt = ej->target_idx;
  double Tr_orig = inst->transformity[ej_orig];

  /* ∂flow_j/∂k_j depends on logic type */
  double Q = inst->state[ej_orig];
  double dflow_dk = 0.0;
  switch (ej->logic) {
  case GSSK_LOGIC_CONSTANT:    dflow_dk = 1.0; break;
  case GSSK_LOGIC_LINEAR:      dflow_dk = Q;   break;
  case GSSK_LOGIC_INTERACTION:
    if (ej->control_idx >= 0) dflow_dk = Q * inst->state[ej->control_idx];
    break;
  case GSSK_LOGIC_LIMIT: {
    double C = -1.0;
    if (ej->control_idx >= 0) C = inst->state[ej->control_idx];
    else if (ej->threshold > 0.0) C = ej->threshold;
    if (C > 1e-9) dflow_dk = Q * C / (C + Q);
    break;
  }
  case GSSK_LOGIC_THRESHOLD:
    dflow_dk = (Q > ej->threshold) ? 1.0 : 0.0;
    break;
  }

  /* For partition output: accumulate ∂F/∂k_j effects on all targets from same origin */
  if (ej->output_mode != OUTPUT_REPLICATE && dflow_dk != 0.0) {
    double os = outsum[ej_orig];
    if (os > 1e-12) {
      double dfrac_ej_dk = dflow_dk * (os - flow[edge_idx]) / (os * os);
      rhs[(size_t)ej_tgt] += dfrac_ej_dk * Tr_orig;

      /* Other edges from same origin: ∂frac_i/∂k_j = -flow_i·dflow_j/os² */
      for (size_t ei = 0; ei < inst->edge_count; ei++) {
        if (ei == edge_idx) continue;
        GSSK_EdgeInternal *other = &inst->edges[ei];
        if (!other->active || other->origin_idx != ej_orig) continue;
        if (other->output_mode == OUTPUT_REPLICATE) continue;
        double dfrac_oi_dk = -flow[ei] * dflow_dk / (os * os);
        rhs[(size_t)other->target_idx] += dfrac_oi_dk * Tr_orig;
      }
    }
  }
  /* Replicate mode: frac=1 regardless of k → ∂F/∂k_j=0 → rhs stays 0 */

  /* Solve M·x = rhs → x = ∂Tr/∂k_j */
  gaussian_solve(M, rhs, n);
  memcpy(out_dTr, rhs, n * sizeof(double));

  free(flow); free(outsum); free(M); free(b); free(rhs);
}

/* =========================================================================
 * Network reclassification
 * ========================================================================= */

GSSK_Status GSSK_ReclassifyNetwork(GSSK_Instance *inst) {
  if (!inst) return GSSK_ERR_UNKNOWN;
  /* Phase 1: all edge types now have IDC treatment (limit via linearisation,
   * threshold via constant forcing), so IDC is always eligible. */
  inst->incipient_eligible = true;
  return GSSK_SUCCESS;
}

/* =========================================================================
 * Phase 8 — Archetype helpers (built-in registration, lookup, expansion)
 * ========================================================================= */

/* Register the four built-in archetypes (producer, consumer, misc_box,
 * system_frame).  Built-in archetypes use literal node ids that we
 * namespace-prefix at expansion time. */
static void register_builtin_archetypes(GSSK_Instance *inst) {
  GSSK_ADefn *d;

  /* ---- producer: storage + interaction (self-feedback) + sink ----
   * Nodes: __body (storage), __gate (interaction), __heat (sink).
   * Internal edges: feed_a, feed_b (body->gate ×2), prod (gate->body),
   *                 resp (body->heat linear, k_respiration). */
  d = &inst->arch_defns[inst->arch_count++];
  memset(d, 0, sizeof(*d));
  strncpy(d->name, "producer", 63);
  d->is_structural = false;
  /* nodes */
  strncpy(d->nodes[0].id, "body", 63);
  strncpy(d->nodes[0].type_str, "storage", 31);
  d->nodes[0].value = 0.0;
  strncpy(d->nodes[1].id, "gate", 63);
  strncpy(d->nodes[1].type_str, "interaction", 31);
  d->nodes[1].node_k = 0.01;
  strncpy(d->nodes[2].id, "heat", 63);
  strncpy(d->nodes[2].type_str, "sink", 31);
  d->node_count = 3;
  /* edges */
  strncpy(d->edges[0].id, "feed_a", 63);
  strncpy(d->edges[0].origin, "body", 63);
  strncpy(d->edges[0].target, "gate", 63);
  strncpy(d->edges[0].logic, "linear", 31);
  d->edges[0].k = 1.0;
  strncpy(d->edges[1].id, "feed_b", 63);
  strncpy(d->edges[1].origin, "body", 63);
  strncpy(d->edges[1].target, "gate", 63);
  strncpy(d->edges[1].logic, "linear", 31);
  d->edges[1].k = 1.0;
  strncpy(d->edges[2].id, "prod", 63);
  strncpy(d->edges[2].origin, "gate", 63);
  strncpy(d->edges[2].target, "body", 63);
  strncpy(d->edges[2].logic, "linear", 31);
  d->edges[2].k = 1.0;
  strncpy(d->edges[3].id, "resp", 63);
  strncpy(d->edges[3].origin, "body", 63);
  strncpy(d->edges[3].target, "heat", 63);
  strncpy(d->edges[3].logic, "linear", 31);
  d->edges[3].k = 0.05;
  d->edge_count = 4;
  strncpy(d->default_in,  "body", 63);
  strncpy(d->default_out, "body", 63);

  /* ---- consumer: storage + sink (metabolism) ---- */
  d = &inst->arch_defns[inst->arch_count++];
  memset(d, 0, sizeof(*d));
  strncpy(d->name, "consumer", 63);
  d->is_structural = false;
  strncpy(d->nodes[0].id, "body", 63);
  strncpy(d->nodes[0].type_str, "storage", 31);
  strncpy(d->nodes[1].id, "heat", 63);
  strncpy(d->nodes[1].type_str, "sink", 31);
  d->node_count = 2;
  strncpy(d->edges[0].id, "metab", 63);
  strncpy(d->edges[0].origin, "body", 63);
  strncpy(d->edges[0].target, "heat", 63);
  strncpy(d->edges[0].logic, "linear", 31);
  d->edges[0].k = 0.1;
  d->edge_count = 1;
  strncpy(d->default_in,  "body", 63);
  strncpy(d->default_out, "body", 63);

  /* ---- misc_box: generic unspecified processing unit ---- */
  d = &inst->arch_defns[inst->arch_count++];
  memset(d, 0, sizeof(*d));
  strncpy(d->name, "misc_box", 63);
  d->is_structural = false;
  strncpy(d->nodes[0].id, "box", 63);
  strncpy(d->nodes[0].type_str, "storage", 31);
  d->node_count = 1;
  d->edge_count = 0;
  strncpy(d->default_in,  "box", 63);
  strncpy(d->default_out, "box", 63);

  /* ---- system_frame: structural-only namespace boundary ---- */
  d = &inst->arch_defns[inst->arch_count++];
  memset(d, 0, sizeof(*d));
  strncpy(d->name, "system_frame", 63);
  d->is_structural = true;
  d->node_count = 0;
  d->edge_count = 0;
}

static GSSK_ADefn *find_archetype(GSSK_Instance *inst, const char *type_str) {
  if (!inst || !type_str) return NULL;
  for (size_t i = 0; i < inst->arch_count; i++)
    if (strcmp(inst->arch_defns[i].name, type_str) == 0)
      return &inst->arch_defns[i];
  return NULL;
}

/* Resolve an external edge endpoint that names a composite ID.
 * Returns the inst->nodes[] slot of the composite's default_in node, or -1. */
static int resolve_composite_in(GSSK_Instance *inst, const char *id) {
  if (!inst || !id) return -1;
  for (size_t i = 0; i < inst->composite_count; i++)
    if (strcmp(inst->composites[i].composite_id, id) == 0)
      return inst->composites[i].in_node_idx;
  return -1;
}

static int resolve_composite_out(GSSK_Instance *inst, const char *id) {
  if (!inst || !id) return -1;
  for (size_t i = 0; i < inst->composite_count; i++)
    if (strcmp(inst->composites[i].composite_id, id) == 0)
      return inst->composites[i].out_node_idx;
  return -1;
}

/* Parse user-defined archetypes from the top-level "archetypes" object.
 * Each entry has the form:
 *   "name": {
 *     "nodes": [...], "edges": [...], "ports": { "<name>": "<internal_id>" }
 *   }
 * Returns GSSK_SUCCESS or an error code (sets inst->error_msg on failure). */
static GSSK_Status parse_user_archetypes(GSSK_Instance *inst, cJSON *root) {
  cJSON *arch_obj = cJSON_GetObjectItem(root, "archetypes");
  if (!cJSON_IsObject(arch_obj)) return GSSK_SUCCESS;

  cJSON *a = NULL;
  cJSON_ArrayForEach(a, arch_obj) {
    if (!a || !a->string) continue;
    if (inst->arch_count >= GSSK_MAX_ARCHETYPES) {
      snprintf(inst->error_msg, sizeof(inst->error_msg),
               "Phase 8: too many archetypes (>%d).", GSSK_MAX_ARCHETYPES);
      return GSSK_ERR_SCHEMA_VIOLATION;
    }
    GSSK_ADefn *def = &inst->arch_defns[inst->arch_count];
    memset(def, 0, sizeof(*def));
    strncpy(def->name, a->string, 63);
    def->name[63] = '\0';
    def->is_structural = false;
    /* Local copy so gcc -Wrestrict doesn't flag snprintf(inst->error_msg, …, def->name)
     * after inlining — both def and error_msg live inside inst. */
    char arch_name[64];
    memcpy(arch_name, def->name, sizeof(arch_name));

    /* Parse nodes */
    cJSON *anodes = cJSON_GetObjectItem(a, "nodes");
    if (cJSON_IsArray(anodes)) {
      int an = cJSON_GetArraySize(anodes);
      if (an > GSSK_MAX_ARCH_NODES) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Phase 8: archetype '%s' has too many nodes (>%d).",
                 arch_name, GSSK_MAX_ARCH_NODES);
        return GSSK_ERR_SCHEMA_VIOLATION;
      }
      for (int i = 0; i < an; i++) {
        cJSON *n = cJSON_GetArrayItem(anodes, i);
        cJSON *nid = cJSON_GetObjectItem(n, "id");
        cJSON *nty = cJSON_GetObjectItem(n, "type");
        cJSON *nvl = cJSON_GetObjectItem(n, "value");
        cJSON *nca = cJSON_GetObjectItem(n, "carrier");
        cJSON *npp = cJSON_GetObjectItem(n, "params");
        if (!cJSON_IsString(nid) || !cJSON_IsString(nty)) {
          snprintf(inst->error_msg, sizeof(inst->error_msg),
                   "Phase 8: archetype '%s' node missing id/type.", arch_name);
          return GSSK_ERR_SCHEMA_VIOLATION;
        }
        GSSK_ANodeTmpl *nt = &def->nodes[def->node_count++];
        strncpy(nt->id, nid->valuestring, 63);
        strncpy(nt->type_str, nty->valuestring, 31);
        nt->value = cJSON_IsNumber(nvl) ? nvl->valuedouble : 0.0;
        if (cJSON_IsString(nca)) strncpy(nt->carrier, nca->valuestring, 31);
        if (cJSON_IsObject(npp)) {
          cJSON *pk  = cJSON_GetObjectItem(npp, "k");
          cJSON *pC  = cJSON_GetObjectItem(npp, "C");
          cJSON *pth = cJSON_GetObjectItem(npp, "threshold");
          cJSON *ppr = cJSON_GetObjectItem(npp, "price");
          if (cJSON_IsNumber(pk))  nt->node_k         = pk->valuedouble;
          if (cJSON_IsNumber(pC))  nt->node_C         = pC->valuedouble;
          if (cJSON_IsNumber(pth)) nt->node_threshold = pth->valuedouble;
          if (cJSON_IsNumber(ppr)) nt->node_price     = ppr->valuedouble;
        }
      }
    }

    /* Parse edges */
    cJSON *aedges = cJSON_GetObjectItem(a, "edges");
    if (cJSON_IsArray(aedges)) {
      int ae = cJSON_GetArraySize(aedges);
      if (ae > GSSK_MAX_ARCH_EDGES) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Phase 8: archetype '%s' has too many edges (>%d).",
                 arch_name, GSSK_MAX_ARCH_EDGES);
        return GSSK_ERR_SCHEMA_VIOLATION;
      }
      for (int i = 0; i < ae; i++) {
        cJSON *e = cJSON_GetArrayItem(aedges, i);
        cJSON *eid = cJSON_GetObjectItem(e, "id");
        cJSON *eor = cJSON_GetObjectItem(e, "origin");
        cJSON *etg = cJSON_GetObjectItem(e, "target");
        cJSON *elg = cJSON_GetObjectItem(e, "logic");
        cJSON *eca = cJSON_GetObjectItem(e, "carrier");
        cJSON *epp = cJSON_GetObjectItem(e, "params");
        if (!cJSON_IsString(eor) || !cJSON_IsString(etg)) {
          snprintf(inst->error_msg, sizeof(inst->error_msg),
                   "Phase 8: archetype '%s' edge missing origin/target.",
                   arch_name);
          return GSSK_ERR_SCHEMA_VIOLATION;
        }
        GSSK_AEdgeTmpl *et = &def->edges[def->edge_count++];
        if (cJSON_IsString(eid)) strncpy(et->id, eid->valuestring, 63);
        strncpy(et->origin, eor->valuestring, 63);
        strncpy(et->target, etg->valuestring, 63);
        if (cJSON_IsString(elg)) strncpy(et->logic, elg->valuestring, 31);
        else                     strncpy(et->logic, "linear", 31);
        if (cJSON_IsString(eca)) strncpy(et->carrier, eca->valuestring, 31);
        et->k = 1.0;
        if (cJSON_IsObject(epp)) {
          cJSON *pk  = cJSON_GetObjectItem(epp, "k");
          cJSON *pth = cJSON_GetObjectItem(epp, "threshold");
          if (cJSON_IsNumber(pk))  et->k         = pk->valuedouble;
          if (cJSON_IsNumber(pth)) et->threshold = pth->valuedouble;
        }
      }
    }

    /* Parse ports — also defines default_in/default_out */
    cJSON *aports = cJSON_GetObjectItem(a, "ports");
    if (cJSON_IsObject(aports)) {
      cJSON *p = NULL;
      cJSON_ArrayForEach(p, aports) {
        if (!p || !p->string || !cJSON_IsString(p)) continue;
        if (def->port_count >= GSSK_MAX_ARCH_PORTS) break;
        strncpy(def->port_names[def->port_count], p->string, 31);
        strncpy(def->port_nodes[def->port_count], p->valuestring, 63);
        def->port_count++;
      }
    }
    if (def->port_count > 0) {
      strncpy(def->default_in,  def->port_nodes[0], 63);
      strncpy(def->default_out, def->port_nodes[def->port_count - 1], 63);
    } else if (def->node_count > 0) {
      strncpy(def->default_in,  def->nodes[0].id, 63);
      strncpy(def->default_out, def->nodes[0].id, 63);
    }

    inst->arch_count++;
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

  /* ---- 0. Metadata (v3) ---- */
  inst->schema_version = 3;  /* default to v3 */
  strncpy(inst->kernel_version, GSK_VERSION_STRING, sizeof(inst->kernel_version) - 1);
  strncpy(inst->created_at, "2000-01-01T00:00:00Z", sizeof(inst->created_at) - 1);

  cJSON *metadata = cJSON_GetObjectItem(root, "metadata");
  if (cJSON_IsObject(metadata)) {
    cJSON *schema_ver = cJSON_GetObjectItem(metadata, "schema_version");
    if (cJSON_IsNumber(schema_ver)) {
      inst->schema_version = schema_ver->valueint;
      if (inst->schema_version != 2 && inst->schema_version != 3 &&
          inst->schema_version != 4) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Unsupported schema_version: %d. Supported: 2, 3, 4.",
                 inst->schema_version);
        status = GSSK_ERR_UNSUPPORTED_SCHEMA_VERSION;
        goto cleanup;
      }
    }

    /* Extract optional metadata fields */
    cJSON *name = cJSON_GetObjectItem(metadata, "name");
    if (cJSON_IsString(name)) {
      strncpy(inst->model_name, name->valuestring, sizeof(inst->model_name) - 1);
    }

    cJSON *desc = cJSON_GetObjectItem(metadata, "description");
    if (cJSON_IsString(desc)) {
      strncpy(inst->model_description, desc->valuestring, sizeof(inst->model_description) - 1);
    }

    cJSON *author = cJSON_GetObjectItem(metadata, "author");
    if (cJSON_IsString(author)) {
      strncpy(inst->model_author, author->valuestring, sizeof(inst->model_author) - 1);
    }

    cJSON *created = cJSON_GetObjectItem(metadata, "created_at");
    if (cJSON_IsString(created)) {
      strncpy(inst->created_at, created->valuestring, sizeof(inst->created_at) - 1);
    }

    cJSON *kv = cJSON_GetObjectItem(metadata, "kernel_version");
    if (cJSON_IsString(kv)) {
      strncpy(inst->kernel_version, kv->valuestring, sizeof(inst->kernel_version) - 1);
    }

    cJSON *hash = cJSON_GetObjectItem(metadata, "model_hash");
    if (cJSON_IsString(hash)) {
      strncpy(inst->model_hash, hash->valuestring, sizeof(inst->model_hash) - 1);
    }
  } else if (inst->schema_version == 2) {
    /* v2 model without metadata → warn and upgrade to v3 */
    fprintf(stderr, "WARNING: v2 model detected (no metadata block). Auto-upgrading to v3. "
                    "Consider migration with: gsk migrate --from 2 <your-model.json>\n");
    inst->schema_version = 3;
  }
  /* If schema_version is 3 but no metadata present, use auto-generated defaults */

  /* ---- 0.5. Carriers (Phase 5, optional) ---- */
  cJSON *carriers_arr = cJSON_GetObjectItem(root, "carriers");
  if (cJSON_IsArray(carriers_arr)) {
    int nc = cJSON_GetArraySize(carriers_arr);
    inst->carrier_count = (size_t)nc;
    if (nc > 0) {
      inst->carriers = calloc((size_t)nc, sizeof(GSSK_Carrier));
      inst->carrier_cons_error = calloc((size_t)nc, sizeof(double));
      if (!inst->carriers || !inst->carrier_cons_error) {
        status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
      }
      for (int ci = 0; ci < nc; ci++) {
        cJSON *c = cJSON_GetArrayItem(carriers_arr, ci);
        cJSON *cid   = cJSON_GetObjectItem(c, "id");
        cJSON *cunit = cJSON_GetObjectItem(c, "unit");
        cJSON *ccons = cJSON_GetObjectItem(c, "conserved");
        if (cJSON_IsString(cid))  { strncpy(inst->carriers[ci].id,   cid->valuestring,   31); }
        if (cJSON_IsString(cunit)){ strncpy(inst->carriers[ci].unit, cunit->valuestring, 31); }
        inst->carriers[ci].conserved = cJSON_IsTrue(ccons);
      }
    }
  }

  /* ---- 0.7 Archetypes (Phase 8) ----
   * Register built-in archetypes first; user archetypes follow.  Built-ins
   * occupy arch_defns[0..3] (producer, consumer, misc_box, system_frame). */
  inst->arch_count      = 0;
  inst->composite_count = 0;
  register_builtin_archetypes(inst);
  status = parse_user_archetypes(inst, root);
  if (status != GSSK_SUCCESS) goto cleanup;

  /* ---- 1. Nodes ---- */
  cJSON *nodes_arr = cJSON_GetObjectItem(root, "nodes");
  if (!cJSON_IsArray(nodes_arr)) {
    snprintf(inst->error_msg, sizeof(inst->error_msg),
             "Schema Error: 'nodes' must be an array.");
    status = GSSK_ERR_SCHEMA_VIOLATION;
    goto cleanup;
  }

  /* Pre-count nodes and extra edges after composite expansion */
  int n_json_nodes = cJSON_GetArraySize(nodes_arr);
  size_t total_nodes       = 0;
  size_t total_extra_edges = 0;
  for (int i = 0; i < n_json_nodes; i++) {
    cJSON *nn = cJSON_GetArrayItem(nodes_arr, i);
    cJSON *tn = cJSON_GetObjectItem(nn, "type");
    const char *ts = cJSON_IsString(tn) ? tn->valuestring : "storage";
    GSSK_ADefn *def = find_archetype(inst, ts);
    if (def && !def->is_structural) {
      total_nodes       += def->node_count;
      total_extra_edges += def->edge_count;
    } else {
      total_nodes += 1;
    }
  }

  inst->node_count = total_nodes;
  inst->nodes  = calloc(inst->node_count ? inst->node_count : 1,
                        sizeof(GSSK_NodeInternal));
  inst->state  = calloc(inst->node_count ? inst->node_count : 1,
                        sizeof(double));
  inst->dQ     = calloc(inst->node_count ? inst->node_count : 1,
                        sizeof(double));

  if (!inst->nodes || !inst->state || !inst->dQ) {
    status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
  }

  bool any_quality = false;
  size_t node_slot = 0;
  for (int i = 0; i < n_json_nodes; i++) {
    cJSON *node = cJSON_GetArrayItem(nodes_arr, i);
    cJSON *id   = cJSON_GetObjectItem(node, "id");
    cJSON *type = cJSON_GetObjectItem(node, "type");
    cJSON *val  = cJSON_GetObjectItem(node, "value");

    if (!cJSON_IsString(id) || !cJSON_IsString(type) || !cJSON_IsNumber(val)) {
      snprintf(inst->error_msg, sizeof(inst->error_msg),
               "Schema Error: Node %d missing id/type/value.", i);
      status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
    }

    /* Duplicate-id check (against already-emitted primitive node ids) */
    for (size_t j = 0; j < node_slot; j++) {
      if (strcmp(inst->nodes[j].id, id->valuestring) == 0) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Duplicate node ID '%s'.", id->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
    }

    /* Optional fields shared between primitive and composite paths */
    cJSON *nc      = cJSON_GetObjectItem(node, "carrier");
    const char *carrier_str = cJSON_IsString(nc) ? nc->valuestring : NULL;
    cJSON *nparams = cJSON_GetObjectItem(node, "params");

    GSSK_ADefn *def = find_archetype(inst, type->valuestring);
    if (def && !def->is_structural) {
      /* ---- Composite node expansion ---- */
      if (inst->composite_count >= GSSK_MAX_COMPOSITES) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Phase 8: too many composite instances (>%d).",
                 GSSK_MAX_COMPOSITES);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      /* Edges go into a temporary buffer once we know the original edge_count;
       * for now allocate them inline into a pending-list area at the end of
       * the edges array (created in section 2 below).  We defer actual edge
       * placement until edge allocation, but the templates need to know the
       * inst->nodes[] slots.  So we record the composite map and emit nodes
       * here; the internal edges are emitted after the user edges array is
       * sized in section 2. */
      int in_idx = -1, out_idx = -1;
      size_t dummy = 0;
      /* Place nodes only (edges handled in step 2 below via re-call) */
      for (size_t j = 0; j < def->node_count; j++) {
        GSSK_ANodeTmpl *t = &def->nodes[j];
        GSSK_NodeInternal *N = &inst->nodes[node_slot + j];
        memset(N, 0, sizeof(*N));
        snprintf(N->id, sizeof(N->id), "%.29s__%.29s", id->valuestring, t->id);
        N->id[63] = '\0';
        N->type   = parse_node_type(t->type_str);
        N->initial_value = (strcmp(t->id, def->default_in) == 0)
                             ? val->valuedouble : t->value;
        N->active = true;
        inst->state[node_slot + j] = N->initial_value;
        if (t->carrier[0]) strncpy(N->carrier, t->carrier, 31);
        else if (carrier_str) strncpy(N->carrier, carrier_str, 31);
        N->node_k         = t->node_k;
        N->node_C         = t->node_C;
        N->node_threshold = t->node_threshold;
        N->node_price     = t->node_price;
        /* Built-in param overrides (producer.gate, etc.) */
        if (cJSON_IsObject(nparams)) {
          if (strcmp(def->name, "producer") == 0 && strcmp(t->id, "gate") == 0) {
            cJSON *kp = cJSON_GetObjectItem(nparams, "k_production");
            if (cJSON_IsNumber(kp)) N->node_k = kp->valuedouble;
          }
        }
        if (strcmp(t->id, def->default_in)  == 0) in_idx  = (int)(node_slot + j);
        if (strcmp(t->id, def->default_out) == 0) out_idx = (int)(node_slot + j);
      }
      GSSK_CompositeMap *cm = &inst->composites[inst->composite_count++];
      strncpy(cm->composite_id, id->valuestring, 63);
      cm->composite_id[63] = '\0';
      cm->in_node_idx  = in_idx;
      cm->out_node_idx = out_idx;
      node_slot += def->node_count;
      (void)dummy;
      continue;
    }

    /* ---- system_frame: structural-only — record as NODE_CONSTANT ---- */
    if (def && def->is_structural) {
      strncpy(inst->nodes[node_slot].id, id->valuestring, 63);
      inst->nodes[node_slot].id[63] = '\0';
      inst->nodes[node_slot].type = NODE_CONSTANT;
      inst->nodes[node_slot].initial_value = val->valuedouble;
      inst->nodes[node_slot].active = true;
      inst->state[node_slot] = val->valuedouble;
      if (carrier_str) strncpy(inst->nodes[node_slot].carrier, carrier_str, 31);
      node_slot++;
      continue;
    }

    /* ---- Primitive node parsing (storage / source / sink / processing) ---- */
    strncpy(inst->nodes[node_slot].id, id->valuestring, 63);
    inst->nodes[node_slot].id[63]        = '\0';
    inst->nodes[node_slot].type          = parse_node_type(type->valuestring);
    inst->nodes[node_slot].initial_value = val->valuedouble;
    inst->nodes[node_slot].active        = true;
    inst->state[node_slot]               = val->valuedouble;

    /* Optional v2 fields */
    cJSON *qi = cJSON_GetObjectItem(node, "quality_input");
    if (cJSON_IsNumber(qi) && qi->valuedouble > 0.0) {
      inst->nodes[node_slot].quality_input = qi->valuedouble;
      any_quality = true;
    }

    cJSON *om = cJSON_GetObjectItem(node, "output_mode");
    inst->nodes[node_slot].output_mode = parse_output_mode(
        cJSON_IsString(om) ? om->valuestring : NULL);

    if (carrier_str) {
      strncpy(inst->nodes[node_slot].carrier, carrier_str, 31);
    }

    /* Phase 7 — processing-node params block */
    if (cJSON_IsObject(nparams)) {
      cJSON *nk  = cJSON_GetObjectItem(nparams, "k");
      cJSON *nC  = cJSON_GetObjectItem(nparams, "C");
      cJSON *nth = cJSON_GetObjectItem(nparams, "threshold");
      cJSON *npr = cJSON_GetObjectItem(nparams, "price");
      if (cJSON_IsNumber(nk))  inst->nodes[node_slot].node_k         = nk->valuedouble;
      if (cJSON_IsNumber(nC))  inst->nodes[node_slot].node_C         = nC->valuedouble;
      if (cJSON_IsNumber(nth)) inst->nodes[node_slot].node_threshold = nth->valuedouble;
      if (cJSON_IsNumber(npr)) inst->nodes[node_slot].node_price     = npr->valuedouble;
    }

    node_slot++;
  }
  /* Final node count = number of primitive slots emitted */
  inst->node_count = node_slot;

  /* ---- 2. Edges ---- */
  cJSON *edges_arr = cJSON_GetObjectItem(root, "edges");
  size_t user_edge_count = 0;
  if (cJSON_IsArray(edges_arr))
    user_edge_count = (size_t)cJSON_GetArraySize(edges_arr);

  /* Allocate edges array with room for composite-internal edges */
  size_t edges_capacity = user_edge_count + total_extra_edges;
  inst->edges = calloc(edges_capacity ? edges_capacity : 1,
                        sizeof(GSSK_EdgeInternal));
  if (!inst->edges && edges_capacity > 0) {
    status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
  }

  if (cJSON_IsArray(edges_arr)) {
    inst->edge_count = user_edge_count;

    for (int i = 0; i < (int)inst->edge_count; i++) {
      cJSON *edge      = cJSON_GetArrayItem(edges_arr, i);
      cJSON *origin    = cJSON_GetObjectItem(edge, "origin");
      cJSON *target    = cJSON_GetObjectItem(edge, "target");
      cJSON *logic_str = cJSON_GetObjectItem(edge, "logic");
      cJSON *params    = cJSON_GetObjectItem(edge, "params");

      if (!cJSON_IsString(origin) || !cJSON_IsString(target)) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Edge %d missing origin/target.", i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      /* Optional id */
      cJSON *eid = cJSON_GetObjectItem(edge, "id");
      if (cJSON_IsString(eid)) {
        strncpy(inst->edges[i].id, eid->valuestring, 63);
        inst->edges[i].id[63] = '\0';
      }

      inst->edges[i].origin_idx = find_node_idx(inst, origin->valuestring);
      if (inst->edges[i].origin_idx == -1) {
        int ci = resolve_composite_out(inst, origin->valuestring);
        if (ci != -1) inst->edges[i].origin_idx = ci;
      }
      inst->edges[i].target_idx = find_node_idx(inst, target->valuestring);
      if (inst->edges[i].target_idx == -1) {
        int ci = resolve_composite_in(inst, target->valuestring);
        if (ci != -1) inst->edges[i].target_idx = ci;
      }
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

      /* Phase 7 — v4 backward-compatible parse: logic/params are optional for
       * edges whose origin OR target is a processing-node type.  Missing logic
       * defaults to LINEAR; missing k defaults to 1.0.  v3 edges that don't
       * touch a processing node still must provide logic + params.k. */
      bool touches_proc =
          is_processing_node(inst->nodes[inst->edges[i].origin_idx].type) ||
          is_processing_node(inst->nodes[inst->edges[i].target_idx].type);

      int lt = GSSK_LOGIC_LINEAR;
      if (cJSON_IsString(logic_str)) {
        lt = parse_logic_type(logic_str->valuestring);
        if (lt == -1) {
          snprintf(inst->error_msg, sizeof(inst->error_msg),
                   "Logic Error: Unknown logic '%s' in edge %d.",
                   logic_str->valuestring, i);
          status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
        }
      } else if (!touches_proc) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Edge %d missing 'logic'.", i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      inst->edges[i].logic = (GSSK_LogicType)lt;

      cJSON *k_item = cJSON_IsObject(params)
                       ? cJSON_GetObjectItem(params, "k") : NULL;
      if (cJSON_IsNumber(k_item)) {
        inst->edges[i].k = k_item->valuedouble;
      } else if (touches_proc) {
        inst->edges[i].k = 1.0;
      } else {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Schema Error: Edge %d missing 'k'.", i);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      cJSON *ctrl = cJSON_IsObject(params)
                     ? cJSON_GetObjectItem(params, "control_node") : NULL;
      inst->edges[i].control_idx = cJSON_IsString(ctrl)
          ? find_node_idx(inst, ctrl->valuestring) : -1;
      if (cJSON_IsString(ctrl) && inst->edges[i].control_idx == -1) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Linkage Error: Edge %d unknown control_node '%s'.",
                 i, ctrl->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }

      cJSON *thr = cJSON_IsObject(params)
                    ? cJSON_GetObjectItem(params, "threshold") : NULL;
      inst->edges[i].threshold = cJSON_IsNumber(thr) ? thr->valuedouble : 0.0;

      /* INTERACTION requires control_node; LIMIT accepts control_node or
       * threshold > 0.  These checks only apply to v3 edges (non-processing). */
      if (!touches_proc) {
        if (inst->edges[i].logic == GSSK_LOGIC_INTERACTION &&
            inst->edges[i].control_idx == -1) {
          snprintf(inst->error_msg, sizeof(inst->error_msg),
                   "Logic Error: Edge %d (%s) requires control_node.",
                   i, logic_str ? logic_str->valuestring : "interaction");
          status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
        }
        if (inst->edges[i].logic == GSSK_LOGIC_LIMIT &&
            inst->edges[i].control_idx == -1 &&
            inst->edges[i].threshold <= 0.0) {
          snprintf(inst->error_msg, sizeof(inst->error_msg),
                   "Logic Error: Edge %d (limit) requires control_node or threshold > 0.",
                   i);
          status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
        }
      }

      /* Optional v2 fields */
      cJSON *om = cJSON_GetObjectItem(edge, "output_mode");
      inst->edges[i].output_mode = parse_output_mode(
          cJSON_IsString(om) ? om->valuestring : NULL);

      /* carrier: Odum Position 1 — metadata only, no effect on ODE */
      cJSON *ec = cJSON_GetObjectItem(edge, "carrier");
      if (cJSON_IsString(ec)) { strncpy(inst->edges[i].carrier, ec->valuestring, 31); }
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

  /* ---- 2.5 Append composite-internal edges (Phase 8) ----
   * Walk JSON nodes again, find composites, and emit their archetype's edges
   * using the recorded composite map.  Composite IDs are namespaced so we
   * locate inst->nodes[] slots by "{composite_id}__{template_id}". */
  for (int i = 0; i < n_json_nodes; i++) {
    cJSON *node = cJSON_GetArrayItem(nodes_arr, i);
    cJSON *id   = cJSON_GetObjectItem(node, "id");
    cJSON *type = cJSON_GetObjectItem(node, "type");
    cJSON *nparams = cJSON_GetObjectItem(node, "params");
    if (!cJSON_IsString(id) || !cJSON_IsString(type)) continue;
    GSSK_ADefn *def = find_archetype(inst, type->valuestring);
    if (!def || def->is_structural || def->edge_count == 0) continue;

    double k_respiration = -1.0, k_metabolism = -1.0;
    if (cJSON_IsObject(nparams)) {
      cJSON *kr = cJSON_GetObjectItem(nparams, "k_respiration");
      cJSON *km = cJSON_GetObjectItem(nparams, "k_metabolism");
      if (cJSON_IsNumber(kr)) k_respiration = kr->valuedouble;
      if (cJSON_IsNumber(km)) k_metabolism  = km->valuedouble;
    }

    for (size_t j = 0; j < def->edge_count; j++) {
      GSSK_AEdgeTmpl *te = &def->edges[j];
      char origin_id[160], target_id[160];
      snprintf(origin_id, sizeof(origin_id), "%s__%s",
               id->valuestring, te->origin);
      snprintf(target_id, sizeof(target_id), "%s__%s",
               id->valuestring, te->target);
      int o = find_node_idx(inst, origin_id);
      int t = find_node_idx(inst, target_id);
      if (o < 0 || t < 0) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Phase 8: failed to expand archetype '%s' for '%s'.",
                 def->name, id->valuestring);
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      if (inst->edge_count >= edges_capacity) {
        snprintf(inst->error_msg, sizeof(inst->error_msg),
                 "Phase 8: edge capacity exceeded during composite expansion.");
        status = GSSK_ERR_SCHEMA_VIOLATION; goto cleanup;
      }
      GSSK_EdgeInternal *E = &inst->edges[inst->edge_count];
      memset(E, 0, sizeof(*E));
      if (te->id[0])
        snprintf(E->id, sizeof(E->id), "%.29s__%.29s", id->valuestring, te->id);
      E->origin_idx  = o;
      E->target_idx  = t;
      E->control_idx = -1;
      E->coupled_idx = -1;
      E->active      = true;
      int lt = parse_logic_type(te->logic[0] ? te->logic : "linear");
      if (lt < 0) lt = GSSK_LOGIC_LINEAR;
      E->logic     = (GSSK_LogicType)lt;
      E->k         = te->k;
      E->threshold = te->threshold;
      if (te->carrier[0]) strncpy(E->carrier, te->carrier, 31);

      /* Built-in parameter overrides */
      if (strcmp(def->name, "producer") == 0 &&
          strcmp(te->id, "resp") == 0 && k_respiration >= 0.0) {
        E->k = k_respiration;
      }
      if (strcmp(def->name, "consumer") == 0 &&
          strcmp(te->id, "metab") == 0 && k_metabolism >= 0.0) {
        E->k = k_metabolism;
      }
      inst->edge_count++;
    }
  }

  /* ---- 3. Config ---- */
  cJSON *config = cJSON_GetObjectItem(root, "config");
  inst->config.t_start          = 0.0;
  inst->config.t_end            = 100.0;
  inst->config.dt               = 0.1;
  inst->config.method           = GSSK_METHOD_AUTO;
  inst->config.solver_tolerance = 1e-6;
  inst->config.rel_tol          = 1e-6;
  inst->config.abs_tol          = 1e-9;
  inst->config.h_min            = 0.0;
  inst->config.h_max            = 0.0;

  if (cJSON_IsObject(config)) {
    cJSON *ts   = cJSON_GetObjectItem(config, "t_start");
    cJSON *te   = cJSON_GetObjectItem(config, "t_end");
    cJSON *dt   = cJSON_GetObjectItem(config, "dt");
    cJSON *tol  = cJSON_GetObjectItem(config, "solver_tolerance");
    cJSON *rtol = cJSON_GetObjectItem(config, "rel_tol");
    cJSON *atol = cJSON_GetObjectItem(config, "abs_tol");
    cJSON *hmin = cJSON_GetObjectItem(config, "h_min");
    cJSON *hmax = cJSON_GetObjectItem(config, "h_max");

    if (cJSON_IsNumber(ts))   inst->config.t_start          = ts->valuedouble;
    if (cJSON_IsNumber(te))   inst->config.t_end             = te->valuedouble;
    if (cJSON_IsNumber(dt))   inst->config.dt                = dt->valuedouble;
    if (cJSON_IsNumber(tol))  inst->config.solver_tolerance  = tol->valuedouble;
    if (cJSON_IsNumber(rtol)) inst->config.rel_tol            = rtol->valuedouble;
    if (cJSON_IsNumber(atol)) inst->config.abs_tol            = atol->valuedouble;
    if (cJSON_IsNumber(hmin)) inst->config.h_min              = hmin->valuedouble;
    if (cJSON_IsNumber(hmax)) inst->config.h_max              = hmax->valuedouble;

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
      else if (strcmp(meth->valuestring, "adaptive") == 0)
        inst->config.method = GSSK_METHOD_ADAPTIVE;
      else
        inst->config.method = GSSK_METHOD_AUTO; /* "auto" or unknown → auto */
    }
  }

  /* ---- 3.5. Init clock ---- */
  inst->current_t  = inst->config.t_start;
  inst->step_count = 0;

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

  /* Phase 2 — DOPRI5 stages and adaptive state */
  inst->k5 = calloc(n, sizeof(double));
  inst->k6 = calloc(n, sizeof(double));
  inst->k7 = calloc(n, sizeof(double));
  if (!inst->k5 || !inst->k6 || !inst->k7) {
    status = GSSK_ERR_MALLOC_FAILED; goto cleanup;
  }
  inst->h_next             = inst->config.dt;
  inst->h_last             = inst->config.dt;
  inst->err_norm_prev      = 1.0;
  inst->conservation_error = 0.0;
  memset(&inst->diag_hooks, 0, sizeof(inst->diag_hooks));
  inst->sens_matrix      = NULL;
  inst->sens_param_idx   = NULL;
  inst->sens_param_count = 0;

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

  /* Phase 1 — per-edge error array */
  inst->edge_error = calloc(inst->edge_count ? inst->edge_count : 1,
                             sizeof(double));
  if (!inst->edge_error) { status = GSSK_ERR_MALLOC_FAILED; goto cleanup; }

  /* ---- 5. Classify network ---- */
  GSSK_ReclassifyNetwork(inst);

  /* ---- 5.5. Snapshot restore (v3) ---- *
   * If the JSON carries a snapshot block, restore live state from it.
   * The topology (nodes/edges) is always parsed from the normal arrays above;
   * snapshot only patches Q[], Tr[], per-edge k, current_t, step_count, and
   * solver confidence. This keeps topology and state cleanly separated.
   */
  cJSON *snap = cJSON_GetObjectItem(root, "snapshot");
  if (cJSON_IsObject(snap)) {
    cJSON *t_val   = cJSON_GetObjectItem(snap, "t");
    cJSON *step_v  = cJSON_GetObjectItem(snap, "step");
    cJSON *state_a = cJSON_GetObjectItem(snap, "state");
    cJSON *ek_a    = cJSON_GetObjectItem(snap, "edge_k");
    cJSON *solver  = cJSON_GetObjectItem(snap, "solver");

    if (cJSON_IsNumber(t_val))  inst->current_t  = t_val->valuedouble;
    if (cJSON_IsNumber(step_v)) inst->step_count = (size_t)step_v->valueint;

    if (cJSON_IsArray(state_a)) {
      int sa_sz = cJSON_GetArraySize(state_a);
      for (int si = 0; si < sa_sz; si++) {
        cJSON *se  = cJSON_GetArrayItem(state_a, si);
        cJSON *sid = cJSON_GetObjectItem(se, "id");
        cJSON *sq  = cJSON_GetObjectItem(se, "Q");
        cJSON *str = cJSON_GetObjectItem(se, "Tr");
        if (!cJSON_IsString(sid) || !cJSON_IsNumber(sq)) continue;
        int ni = find_node_idx(inst, sid->valuestring);
        if (ni < 0) continue;
        inst->state[ni] = sq->valuedouble;
        if (inst->quality_enabled && inst->transformity && cJSON_IsNumber(str))
          inst->transformity[ni] = str->valuedouble;
      }
    }

    if (cJSON_IsArray(ek_a)) {
      int ek_sz = cJSON_GetArraySize(ek_a);
      for (int ei = 0; ei < ek_sz; ei++) {
        cJSON *ee  = cJSON_GetArrayItem(ek_a, ei);
        cJSON *sid = cJSON_GetObjectItem(ee, "id");
        cJSON *ek  = cJSON_GetObjectItem(ee, "k");
        if (!cJSON_IsString(sid) || !cJSON_IsNumber(ek)) continue;
        int idx = find_edge_idx(inst, sid->valuestring);
        if (idx < 0) continue;
        inst->edges[idx].k = ek->valuedouble;
      }
    }

    if (cJSON_IsObject(solver)) {
      cJSON *conf = cJSON_GetObjectItem(solver, "confidence");
      if (cJSON_IsString(conf) && strcmp(conf->valuestring, "degraded") == 0)
        inst->confidence = GSSK_CONFIDENCE_DEGRADED;
      cJSON *elig = cJSON_GetObjectItem(solver, "incipient_eligible");
      if (cJSON_IsBool(elig))
        inst->incipient_eligible = cJSON_IsTrue(elig);
    }

    /* Restore mutation log */
    cJSON *mlog_a = cJSON_GetObjectItem(snap, "mutation_log");
    if (cJSON_IsArray(mlog_a)) {
      int ml_sz = cJSON_GetArraySize(mlog_a);
      for (int mi = 0; mi < ml_sz; mi++) {
        cJSON *mo = cJSON_GetArrayItem(mlog_a, mi);
        cJSON *mt  = cJSON_GetObjectItem(mo, "t");
        cJSON *mop = cJSON_GetObjectItem(mo, "op");
        cJSON *mti = cJSON_GetObjectItem(mo, "target_id");
        cJSON *mpl = cJSON_GetObjectItem(mo, "payload");
        cJSON *mca = cJSON_GetObjectItem(mo, "cause");

        if (inst->mutation_count >= inst->mutation_capacity) {
          size_t new_cap = inst->mutation_capacity == 0 ? 16 : inst->mutation_capacity * 2;
          GSSK_MutationRecord *new_log = realloc(inst->mutation_log,
                                                  new_cap * sizeof(GSSK_MutationRecord));
          if (!new_log) break;
          inst->mutation_log      = new_log;
          inst->mutation_capacity = new_cap;
        }
        GSSK_MutationRecord *r = &inst->mutation_log[inst->mutation_count++];
        memset(r, 0, sizeof(GSSK_MutationRecord));
        r->t  = cJSON_IsNumber(mt) ? mt->valuedouble : 0.0;
        r->op = parse_mutation_op(cJSON_IsString(mop) ? mop->valuestring : NULL);
        if (cJSON_IsString(mti)) { strncpy(r->target_id, mti->valuestring, 63); }
        if (cJSON_IsString(mpl)) { strncpy(r->payload,   mpl->valuestring, 255); }
        if (cJSON_IsString(mca)) { strncpy(r->cause,     mca->valuestring, 63); }
      }
    }

    GSSK_ReclassifyNetwork(inst); /* re-run after k restoration */
  }

cleanup:
  cJSON_Delete(root);
  return status;
}

/* =========================================================================
 * GSSK_Step — Phase 1: IDC as Baseline (No Silent Fallback)
 *
 * For all methods except EULER:
 *   1. RK4 step → inst->tmp_state (always, ground truth)
 *   2. Threshold event detection via Illinois algorithm
 *   3. IDC step → inst->idc_state (AUTO/INCIPIENT only)
 *   4. Per-edge error estimates (IDC vs RK4 flows)
 *   5. Solver selection: IDC if step_error < tolerance (HIGH),
 *      else RK4 (DEGRADED) — no silent fallback to RK4 for non-IDC networks
 * ========================================================================= */

GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt) {
  if (!inst) return GSSK_ERR_UNKNOWN;

  size_t n = inst->node_count;
  GSSK_Status ret = GSSK_SUCCESS;

  /* ---- Phase 3: forward sensitivity Euler step (pre-step, using Q(t)) ---- */
  if (inst->sens_param_count > 0 && inst->sens_matrix)
    sens_euler_step(inst, inst->state, dt);

  /* ---- Phase 5: save Q_before for per-carrier conservation (reuse idc_state) ---- */
  if (inst->carrier_count > 0)
    memcpy(inst->idc_state, inst->state, n * sizeof(double));

  /* ---- ADAPTIVE: DOPRI5 spans dt using PI-controlled sub-steps ---- */
  if (inst->config.method == GSSK_METHOD_ADAPTIVE) {
    GSSK_Status st = adaptive_step_ex(inst, dt, inst->current_t);
    if (st == GSSK_ERR_DIVERGENCE) return st;
    ret = st;
    goto post_step;
  }

  /* ---- EULER: independent simple path ---- */
  if (inst->config.method == GSSK_METHOD_EULER) {
    compute_derivatives(inst, inst->state, inst->dQ);
    for (size_t i = 0; i < n; i++)
      inst->state[i] += inst->dQ[i] * dt;
    goto post_step;
  }

  /* ---- RK4 step → inst->tmp_state (ground truth for all non-EULER modes) ---- */
  rk4_step_ex(inst, inst->state, inst->tmp_state, dt);

  /* ---- Threshold event detection with sub-stepping (Phase 1.3) ---- */
  /* do_threshold_substep handles simultaneous events, degenerate starts,
   * and integrator restarts from each crossing point.  It reads inst->state
   * (before) and inst->tmp_state (Q_rk4 trial), then overwrites inst->tmp_state
   * with the sub-stepped final state. */
  {
    bool has_threshold = false;
    for (size_t ei = 0; ei < inst->edge_count && !has_threshold; ei++)
      has_threshold = inst->edges[ei].active &&
                      inst->edges[ei].logic == GSSK_LOGIC_THRESHOLD;
    if (has_threshold)
      do_threshold_substep(inst, inst->state, inst->tmp_state,
                           dt, inst->current_t, inst->tmp_state);
  }

  /* ---- Pure RK4: apply and skip IDC ---- */
  if (inst->config.method == GSSK_METHOD_RK4) {
    memcpy(inst->state, inst->tmp_state, n * sizeof(double));
    goto post_step;
  }

  /* ---- IDC step → inst->idc_state (AUTO / INCIPIENT) ---- */
  /* incipient_eligible is always true (Phase 1.4); idc_step_ex handles
   * all edge types (limit via linearisation, threshold via forcing). */
  idc_step_ex(inst, inst->state, inst->idc_state, dt);

  /* ---- Per-edge error estimates ---- */
  compute_per_edge_errors(inst, inst->tmp_state, inst->idc_state);

  /* ---- Solver selection ---- */
  if (inst->config.method == GSSK_METHOD_AUTO) {
    if (inst->step_error < inst->config.solver_tolerance) {
      inst->confidence = GSSK_CONFIDENCE_HIGH;
      memcpy(inst->state, inst->idc_state, n * sizeof(double));
    } else {
      inst->confidence = GSSK_CONFIDENCE_DEGRADED;
      memcpy(inst->state, inst->tmp_state, n * sizeof(double));
      ret = GSSK_WARN_SOLVER_DIVERGENCE;
    }
  } else { /* INCIPIENT: always IDC */
    inst->confidence = GSSK_CONFIDENCE_HIGH;
    memcpy(inst->state, inst->idc_state, n * sizeof(double));
  }

post_step:
  /* ---- Stability check and non-negativity clamp ---- */
  for (size_t i = 0; i < n; i++) {
    if (isnan(inst->state[i]) || isinf(inst->state[i])) {
      if (inst->diag_hooks.on_divergence)
        inst->diag_hooks.on_divergence(inst->diag_hooks.ctx, inst->current_t,
                                       inst->nodes[i].id, inst->state[i]);
      return GSSK_ERR_DIVERGENCE;
    }
    if (inst->state[i] < 0.0)
      inst->state[i] = 0.0;
  }

  /* ---- Phase 5: per-carrier conservation errors (Q_before saved in idc_state) ---- */
  if (inst->carrier_count > 0 && inst->config.method != GSSK_METHOD_ADAPTIVE)
    update_carrier_conservation_errors(inst, inst->idc_state, inst->state);

  /* ---- Quality accounting pass ---- */
  if (inst->quality_enabled)
    compute_quality_pass(inst, inst->state);

  inst->current_t += dt;
  inst->step_count++;
  scan_motifs_internal(inst);
  return ret;
}

/* =========================================================================
 * Phase 9 — Runtime Pattern Discovery
 * ========================================================================= */

/* Build canonical form for a 3-node motif.  Enumerates all 6 permutations,
 * keeps only those that maintain non-decreasing type order, and picks the
 * lexicographically smallest — giving a proper isomorphism-invariant form. */
static void safe_str_copy(char *dst, const char *src, size_t cap) {
  if (!dst || cap == 0) return;
  size_t len = (src && *src) ? strlen(src) : 0;
  if (len >= cap) len = cap - 1;
  if (len > 0) memcpy(dst, src, len);
  dst[len] = '\0';
}

static void make_canon_3(char *out, size_t cap,
                          const char *t[3], const bool e[3][3],
                          char best_types[3][32], uint8_t *out_bits) {
  static const int perms[6][3] = {
    {0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}
  };
  char best[128] = "";
  uint8_t best_bits = 0;
  char best_t[3][32] = {"","",""};

  for (int pi = 0; pi < 6; pi++) {
    int p0 = perms[pi][0], p1 = perms[pi][1], p2 = perms[pi][2];
    if (strcmp(t[p0], t[p1]) > 0 || strcmp(t[p1], t[p2]) > 0) continue;
    uint8_t bits = 0;
    if (e[p0][p1]) bits |= 1;
    if (e[p0][p2]) bits |= 2;
    if (e[p1][p0]) bits |= 4;
    if (e[p1][p2]) bits |= 8;
    if (e[p2][p0]) bits |= 16;
    if (e[p2][p1]) bits |= 32;
    char cand[128];
    snprintf(cand, sizeof(cand), "3:%s:%s:%s:%u", t[p0], t[p1], t[p2], (unsigned)bits);
    if (best[0] == '\0' || strcmp(cand, best) < 0) {
      safe_str_copy(best, cand, sizeof(best));
      best_bits = bits;
      safe_str_copy(best_t[0], t[p0], sizeof(best_t[0]));
      safe_str_copy(best_t[1], t[p1], sizeof(best_t[1]));
      safe_str_copy(best_t[2], t[p2], sizeof(best_t[2]));
    }
  }
  safe_str_copy(out, best[0] ? best : "3:?:?:?:0", cap);
  if (best_types) {
    safe_str_copy(best_types[0], best_t[0], 32);
    safe_str_copy(best_types[1], best_t[1], 32);
    safe_str_copy(best_types[2], best_t[2], 32);
  }
  if (out_bits) *out_bits = best_bits;
}

static void record_motif_internal(GSSK_Instance *inst, const char *canon,
                                   uint8_t size, uint8_t edge_bits,
                                   const char node_types[3][32]) {
  for (size_t i = 0; i < inst->motif_count; i++) {
    if (strcmp(inst->motifs[i].canon, canon) == 0) {
      inst->motifs[i].occurrence++;
      return;
    }
  }
  if (inst->motif_count >= GSSK_MOTIF_TABLE_CAP) return;
  GSSK_MotifEntry *m = &inst->motifs[inst->motif_count++];
  memset(m, 0, sizeof(*m));
  safe_str_copy(m->canon, canon, sizeof(m->canon));
  m->size = size;
  m->edge_bits = edge_bits;
  int ec = 0;
  for (int b = 0; b < 8; b++) if (edge_bits & (1u << b)) ec++;
  m->complexity = (double)ec / size;
  for (int j = 0; j < size && j < 3; j++) {
    safe_str_copy(m->node_types[j], node_types[j], sizeof(m->node_types[j]));
  }
  m->occurrence = 1;
}

static void scan_motifs_internal(GSSK_Instance *inst) {
  size_t n = inst->node_count;
  if (n == 0 || n > GSSK_MOTIF_SCAN_NODE_LIMIT) return;

  /* Build flat adjacency matrix adj[a*n+b] = true if active edge a→b */
  bool adj[GSSK_MOTIF_SCAN_NODE_LIMIT * GSSK_MOTIF_SCAN_NODE_LIMIT];
  memset(adj, 0, n * n * sizeof(bool));
  for (size_t ei = 0; ei < inst->edge_count; ei++) {
    GSSK_EdgeInternal *e = &inst->edges[ei];
    if (!e->active || e->origin_idx < 0 || e->target_idx < 0) continue;
    adj[(size_t)e->origin_idx * n + (size_t)e->target_idx] = true;
  }

  /* Reset occurrence counts from last step */
  for (size_t mi = 0; mi < inst->motif_count; mi++)
    inst->motifs[mi].occurrence = 0;

  /* ---- 2-node motifs: each pair (a < b) with at least one edge ---- */
  for (size_t a = 0; a < n; a++) {
    if (!inst->nodes[a].active) continue;
    for (size_t b = a + 1; b < n; b++) {
      if (!inst->nodes[b].active) continue;
      bool ab = adj[a * n + b], ba = adj[b * n + a];
      if (!ab && !ba) continue;
      const char *ta = node_type_str(inst->nodes[a].type);
      const char *tb = node_type_str(inst->nodes[b].type);
      char canon[128];
      uint8_t bits = 0;
      char types_canon[3][32] = {"","",""};
      /* pick which ordering makes the lex-smaller string */
      uint8_t bits_ab = (uint8_t)((ab ? 1 : 0) | (ba ? 2 : 0));
      uint8_t bits_ba = (uint8_t)((ba ? 1 : 0) | (ab ? 2 : 0));
      char cand_ab[128], cand_ba[128];
      snprintf(cand_ab, sizeof(cand_ab), "2:%s:%s:%u", ta, tb, (unsigned)bits_ab);
      snprintf(cand_ba, sizeof(cand_ba), "2:%s:%s:%u", tb, ta, (unsigned)bits_ba);
      if (strcmp(cand_ab, cand_ba) <= 0) {
        strncpy(canon, cand_ab, sizeof(canon) - 1); canon[sizeof(canon)-1] = '\0';
        bits = bits_ab;
        strncpy(types_canon[0], ta, 31); strncpy(types_canon[1], tb, 31);
      } else {
        strncpy(canon, cand_ba, sizeof(canon) - 1); canon[sizeof(canon)-1] = '\0';
        bits = bits_ba;
        strncpy(types_canon[0], tb, 31); strncpy(types_canon[1], ta, 31);
      }
      record_motif_internal(inst, canon, 2, bits, (const char (*)[32])types_canon);
    }
  }

  /* ---- 3-node motifs: each triple (a < b < c) with ≥ 2 edges ---- */
  for (size_t a = 0; a < n; a++) {
    if (!inst->nodes[a].active) continue;
    for (size_t b = a + 1; b < n; b++) {
      if (!inst->nodes[b].active) continue;
      for (size_t c = b + 1; c < n; c++) {
        if (!inst->nodes[c].active) continue;
        bool eab = adj[a*n+b], eba = adj[b*n+a];
        bool eac = adj[a*n+c], eca = adj[c*n+a];
        bool ebc = adj[b*n+c], ecb = adj[c*n+b];
        int edge_cnt = (eab?1:0)+(eba?1:0)+(eac?1:0)+(eca?1:0)+(ebc?1:0)+(ecb?1:0);
        if (edge_cnt < 2) continue;
        const char *types[3] = {
          node_type_str(inst->nodes[a].type),
          node_type_str(inst->nodes[b].type),
          node_type_str(inst->nodes[c].type)
        };
        bool e[3][3] = {
          {false, eab, eac},
          {eba, false, ebc},
          {eca, ecb, false}
        };
        char canon[128];
        char best_types[3][32];
        uint8_t bits = 0;
        make_canon_3(canon, sizeof(canon), types, e, best_types, &bits);
        if (canon[0])
          record_motif_internal(inst, canon, 3, bits, (const char (*)[32])best_types);
      }
    }
  }

  /* ---- Update stability counters and generativity index ---- */
  size_t new_candidates = 0;
  double total_complexity = 0.0;
  for (size_t mi = 0; mi < inst->motif_count; mi++) {
    GSSK_MotifEntry *m = &inst->motifs[mi];
    if (m->occurrence >= GSSK_MOTIF_MIN_COUNT) {
      m->stable_steps++;
      if (!m->is_candidate && m->stable_steps >= GSSK_MOTIF_MIN_STEPS) {
        m->is_candidate = true;
        new_candidates++;
        total_complexity += m->complexity;
      }
    } else {
      m->stable_steps = 0;
    }
  }
  double dt = inst->config.dt > 1e-15 ? inst->config.dt : 1.0;
  double avg_c = new_candidates > 0 ? total_complexity / (double)new_candidates : 0.0;
  inst->generativity_index = (double)new_candidates * avg_c / dt;
}

/* =========================================================================
 * Phase 9 — Public API
 * ========================================================================= */

size_t GSSK_GetMotifCount(GSSK_Instance *inst) {
  return inst ? inst->motif_count : 0;
}

const char *GSSK_GetMotifCanon(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return NULL;
  return inst->motifs[idx].canon;
}

size_t GSSK_GetMotifOccurrence(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return 0;
  return inst->motifs[idx].occurrence;
}

size_t GSSK_GetMotifStableSteps(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return 0;
  return inst->motifs[idx].stable_steps;
}

bool GSSK_IsMotifCandidate(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return false;
  return inst->motifs[idx].is_candidate;
}

size_t GSSK_GetMotifSize(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return 0;
  return inst->motifs[idx].size;
}

double GSSK_GetMotifComplexity(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->motif_count) return 0.0;
  return inst->motifs[idx].complexity;
}

double GSSK_GetGenerativityIndex(GSSK_Instance *inst) {
  return inst ? inst->generativity_index : 0.0;
}

GSSK_Status GSSK_ProposeArchetype(GSSK_Instance *inst, size_t motif_idx,
                                   const char *name) {
  if (!inst || !name) return GSSK_ERR_NOT_FOUND;
  if (motif_idx >= inst->motif_count) return GSSK_ERR_NOT_FOUND;
  GSSK_MotifEntry *m = &inst->motifs[motif_idx];
  if (!m->is_candidate) return GSSK_ERR_NOT_FOUND;

  /* Check name uniqueness */
  for (size_t i = 0; i < inst->arch_count; i++) {
    if (strcmp(inst->arch_defns[i].name, name) == 0)
      return GSSK_ERR_SCHEMA_VIOLATION;
  }
  if (inst->arch_count >= GSSK_MAX_ARCHETYPES) return GSSK_ERR_SCHEMA_VIOLATION;

  /* Build archetype definition from motif */
  GSSK_ADefn *def = &inst->arch_defns[inst->arch_count];
  memset(def, 0, sizeof(*def));
  strncpy(def->name, name, sizeof(def->name) - 1);
  def->node_count = m->size;
  def->is_structural = false;

  for (size_t ni = 0; ni < m->size; ni++) {
    snprintf(def->nodes[ni].id, sizeof(def->nodes[ni].id), "node%zu", ni);
    strncpy(def->nodes[ni].type_str, m->node_types[ni],
            sizeof(def->nodes[ni].type_str) - 1);
    def->nodes[ni].value = 0.0;
  }

  /* Wire edges from adjacency bits */
  /* Bit encoding for 2-node: bit0=0→1, bit1=1→0
     Bit encoding for 3-node: bit0=0→1,bit1=0→2,bit2=1→0,bit3=1→2,bit4=2→0,bit5=2→1 */
  size_t ei = 0;
  if (m->size == 2) {
    if (m->edge_bits & 1) {
      strncpy(def->edges[ei].origin, "node0", 63);
      strncpy(def->edges[ei].target, "node1", 63);
      strncpy(def->edges[ei].logic, "linear", 31);
      def->edges[ei].k = 1.0; ei++;
    }
    if (m->edge_bits & 2) {
      strncpy(def->edges[ei].origin, "node1", 63);
      strncpy(def->edges[ei].target, "node0", 63);
      strncpy(def->edges[ei].logic, "linear", 31);
      def->edges[ei].k = 1.0; ei++;
    }
  } else {
    static const int src3[6] = {0,0,1,1,2,2};
    static const int tgt3[6] = {1,2,0,2,0,1};
    for (int b = 0; b < 6; b++) {
      if (!(m->edge_bits & (1u << b))) continue;
      snprintf(def->edges[ei].origin, sizeof(def->edges[ei].origin), "node%d", src3[b]);
      snprintf(def->edges[ei].target, sizeof(def->edges[ei].target), "node%d", tgt3[b]);
      strncpy(def->edges[ei].logic, "linear", 31);
      def->edges[ei].k = 1.0; ei++;
    }
  }
  def->edge_count = ei;

  /* Default ports */
  strncpy(def->default_in,  def->nodes[0].id, sizeof(def->default_in) - 1);
  strncpy(def->default_out, def->nodes[m->size-1].id, sizeof(def->default_out) - 1);
  inst->arch_count++;

  /* Log to mutation log */
  append_mutation(inst, GSSK_MUT_ARCHETYPE_PROPOSAL, name, m->canon);

  return GSSK_SUCCESS;
}

/* =========================================================================
 * Phase 4 — Mutation log helpers
 * ========================================================================= */

static void append_mutation(GSSK_Instance *inst, GSSK_MutationOp op,
                             const char *target_id, const char *payload) {
  if (!inst) return;

  if (inst->mutation_count >= inst->mutation_capacity) {
    size_t new_cap = inst->mutation_capacity == 0 ? 16 : inst->mutation_capacity * 2;
    GSSK_MutationRecord *new_log = realloc(inst->mutation_log,
                                            new_cap * sizeof(GSSK_MutationRecord));
    if (!new_log) return; /* silently skip on OOM */
    inst->mutation_log     = new_log;
    inst->mutation_capacity = new_cap;
  }

  GSSK_MutationRecord *r = &inst->mutation_log[inst->mutation_count++];
  memset(r, 0, sizeof(GSSK_MutationRecord));
  r->t  = inst->current_t;
  r->op = op;
  if (target_id) { strncpy(r->target_id, target_id, 63); r->target_id[63] = '\0'; }
  if (payload)   { strncpy(r->payload,   payload,   255); r->payload[255]  = '\0'; }
  if (inst->pending_cause[0])
    snprintf(r->cause, sizeof(r->cause), "%s", inst->pending_cause);
  else
    snprintf(r->cause, sizeof(r->cause), "%s", "user");
  inst->pending_cause[0] = '\0';

  if (inst->diag_hooks.on_mutation)
    inst->diag_hooks.on_mutation(inst->diag_hooks.ctx, r);
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
  double *new_k5    = realloc(inst->k5,    new_n * sizeof(double));
  double *new_k6    = realloc(inst->k6,    new_n * sizeof(double));
  double *new_k7    = realloc(inst->k7,    new_n * sizeof(double));
  double *new_tmp   = realloc(inst->tmp_state, new_n * sizeof(double));
  double *new_idc   = realloc(inst->idc_state, new_n * sizeof(double));

  if (!new_nodes || !new_state || !new_dQ || !new_k2 ||
      !new_k3 || !new_k4 || !new_k5 || !new_k6 || !new_k7 ||
      !new_tmp || !new_idc) {
    cJSON_Delete(node);
    return GSSK_ERR_MALLOC_FAILED;
  }

  inst->nodes     = new_nodes;
  inst->state     = new_state;
  inst->dQ        = new_dQ;
  inst->k2        = new_k2;
  inst->k3        = new_k3;
  inst->k4        = new_k4;
  inst->k5        = new_k5;
  inst->k6        = new_k6;
  inst->k7        = new_k7;
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

  /* Grow sensitivity matrix if enabled (add zero row for new node) */
  if (inst->sens_param_count > 0 && inst->sens_matrix) {
    size_t m = inst->sens_param_count;
    double *new_sens = realloc(inst->sens_matrix, new_n * m * sizeof(double));
    if (!new_sens) { cJSON_Delete(node); return GSSK_ERR_MALLOC_FAILED; }
    inst->sens_matrix = new_sens;
    memset(inst->sens_matrix + inst->node_count * m, 0, m * sizeof(double));
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
  inst->k5[idx] = inst->k6[idx] = inst->k7[idx] = 0.0;
  inst->tmp_state[idx] = inst->idc_state[idx] = 0.0;

  cJSON *qi = cJSON_GetObjectItem(node, "quality_input");
  if (cJSON_IsNumber(qi) && qi->valuedouble > 0.0)
    inst->nodes[idx].quality_input = qi->valuedouble;

  cJSON *om = cJSON_GetObjectItem(node, "output_mode");
  inst->nodes[idx].output_mode = parse_output_mode(
      cJSON_IsString(om) ? om->valuestring : NULL);

  cJSON *nc_add = cJSON_GetObjectItem(node, "carrier");
  if (cJSON_IsString(nc_add)) { strncpy(inst->nodes[idx].carrier, nc_add->valuestring, 31); }

  inst->node_count = new_n;
  cJSON_Delete(node);
  append_mutation(inst, GSSK_MUT_ADD_NODE, inst->nodes[idx].id, json_node_fragment);
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

  /* Grow per-edge error array */
  double *new_err = realloc(inst->edge_error, new_ec * sizeof(double));
  if (!new_err) { cJSON_Delete(edge); return GSSK_ERR_MALLOC_FAILED; }
  inst->edge_error = new_err;
  inst->edge_error[new_ec - 1] = 0.0;

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

  cJSON *ec_add = cJSON_GetObjectItem(edge, "carrier");
  if (cJSON_IsString(ec_add)) { strncpy(inst->edges[ei].carrier, ec_add->valuestring, 31); }

  inst->edge_count = new_ec;
  cJSON_Delete(edge);
  append_mutation(inst, GSSK_MUT_ADD_EDGE,
                  inst->edges[ei].id[0] ? inst->edges[ei].id : "",
                  json_edge_fragment);
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
  append_mutation(inst, GSSK_MUT_DEACTIVATE_EDGE, edge_id, NULL);
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
  append_mutation(inst, GSSK_MUT_DEACTIVATE_NODE, node_id, NULL);
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
  if (inst->edge_error)
    memset(inst->edge_error, 0, inst->edge_count * sizeof(double));
  inst->step_error         = 0.0;
  inst->event_count        = 0; /* retain capacity, just reset count */
  inst->confidence         = GSSK_CONFIDENCE_HIGH;
  inst->current_t          = inst->config.t_start;
  inst->step_count         = 0;
  inst->h_next             = inst->config.dt;
  inst->h_last             = inst->config.dt;
  inst->err_norm_prev      = 1.0;
  inst->conservation_error = 0.0;
  if (inst->sens_matrix)
    memset(inst->sens_matrix, 0,
           inst->node_count * inst->sens_param_count * sizeof(double));
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

const char *GSSK_GetNodeTypeString(GSSK_Instance *inst, size_t node_idx) {
  if (!inst || node_idx >= inst->node_count) return "storage";
  return node_type_str(inst->nodes[node_idx].type);
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
  char kbuf[32];
  snprintf(kbuf, sizeof(kbuf), "%.17g", k);
  append_mutation(inst, GSSK_MUT_SET_EDGE_K,
                  inst->edges[index].id[0] ? inst->edges[index].id : "",
                  kbuf);
}

double GSSK_GetTStart(GSSK_Instance *inst) { return inst ? inst->config.t_start : 0.0; }
double GSSK_GetTEnd(GSSK_Instance *inst)   { return inst ? inst->config.t_end   : 0.0; }
double GSSK_GetDt(GSSK_Instance *inst)     { return inst ? inst->config.dt      : 0.0; }

/* Metadata accessors */
int GSSK_GetSchemaVersion(GSSK_Instance *inst) {
  return inst ? inst->schema_version : 0;
}

const char *GSSK_GetModelName(GSSK_Instance *inst) {
  return inst ? inst->model_name : "";
}

const char *GSSK_GetModelDescription(GSSK_Instance *inst) {
  return inst ? inst->model_description : "";
}

const char *GSSK_GetModelKernelVersion(GSSK_Instance *inst) {
  return inst ? inst->kernel_version : "";
}

const char *GSSK_GetModelHash(GSSK_Instance *inst) {
  return inst ? inst->model_hash : "";
}

/* Version functions */
const char *GSSK_GetVersionString(void) {
  return GSK_VERSION_STRING;
}

uint32_t GSSK_GetVersionCode(void) {
  return GSK_VERSION_CODE(GSK_VERSION_MAJOR, GSK_VERSION_MINOR, GSK_VERSION_PATCH);
}

double GSSK_GetCurrentTime(GSSK_Instance *inst) {
  return inst ? inst->current_t : 0.0;
}

size_t GSSK_GetStepCount(GSSK_Instance *inst) {
  return inst ? inst->step_count : 0;
}

/* =========================================================================
 * Phase 1 accessors — error estimates and event log
 * ========================================================================= */

double GSSK_GetEdgeErrorEstimate(GSSK_Instance *inst, size_t edge_idx) {
  if (!inst || !inst->edge_error || edge_idx >= inst->edge_count) return 0.0;
  return inst->edge_error[edge_idx];
}

double GSSK_GetStepErrorEstimate(GSSK_Instance *inst) {
  return inst ? inst->step_error : 0.0;
}

size_t GSSK_GetEventCount(GSSK_Instance *inst) {
  return inst ? inst->event_count : 0;
}

double GSSK_GetEventTime(GSSK_Instance *inst, size_t event_idx) {
  if (!inst || event_idx >= inst->event_count) return 0.0;
  return inst->events[event_idx].t;
}

const char *GSSK_GetEventEdgeID(GSSK_Instance *inst, size_t event_idx) {
  if (!inst || event_idx >= inst->event_count) return NULL;
  return inst->events[event_idx].edge_id;
}

int GSSK_GetEventDirection(GSSK_Instance *inst, size_t event_idx) {
  if (!inst || event_idx >= inst->event_count) return 0;
  return inst->events[event_idx].direction;
}

/* =========================================================================
 * Phase 2 — Adaptive Numerics public API
 * ========================================================================= */

GSSK_Status GSSK_StepAdaptive(GSSK_Instance *inst) {
  if (!inst) return GSSK_ERR_UNKNOWN;
  size_t n = inst->node_count;

  /* Determine h for this step */
  double h = inst->h_next > 0.0 ? inst->h_next : inst->config.dt;

  /* Don't advance past t_end */
  double t_rem = inst->config.t_end - inst->current_t;
  if (t_rem <= 0.0) return GSSK_SUCCESS;
  if (h > t_rem) h = t_rem;

  /* Run adaptive step for exactly h */
  GSSK_Status st = adaptive_step_ex(inst, h, inst->current_t);
  if (st == GSSK_ERR_DIVERGENCE) return st;

  /* h_last exposed as the externally visible step size for this call */
  inst->h_last = h;
  /* h_next was updated by adaptive_step_ex (PI suggestion from last sub-step) */

  if (inst->quality_enabled)
    compute_quality_pass(inst, inst->state);

  for (size_t i = 0; i < n; i++)
    if (inst->state[i] < 0.0) inst->state[i] = 0.0;

  inst->current_t += h;
  inst->step_count++;
  scan_motifs_internal(inst);
  return st;
}

double GSSK_GetLastStepSize(GSSK_Instance *inst) {
  return inst ? inst->h_last : 0.0;
}

double GSSK_GetNextStepSize(GSSK_Instance *inst) {
  return inst ? inst->h_next : 0.0;
}

double GSSK_GetConservationError(GSSK_Instance *inst) {
  return inst ? inst->conservation_error : 0.0;
}

void GSSK_SetDiagHooks(GSSK_Instance *inst, const GSSK_DiagHooks *hooks) {
  if (!inst) return;
  if (hooks)
    inst->diag_hooks = *hooks;
  else
    memset(&inst->diag_hooks, 0, sizeof(inst->diag_hooks));
}

/* =========================================================================
 * Phase 4 — Mutation log public API
 * ========================================================================= */

size_t GSSK_GetMutationCount(GSSK_Instance *inst) {
  return inst ? inst->mutation_count : 0;
}

const GSSK_MutationRecord *GSSK_GetMutationRecord(GSSK_Instance *inst,
                                                   size_t idx) {
  if (!inst || idx >= inst->mutation_count) return NULL;
  return &inst->mutation_log[idx];
}

void GSSK_SetMutationCause(GSSK_Instance *inst, const char *cause) {
  if (!inst) return;
  if (cause && cause[0]) {
    strncpy(inst->pending_cause, cause, 63);
    inst->pending_cause[63] = '\0';
  } else {
    inst->pending_cause[0] = '\0';
  }
}

void GSSK_ClearMutationLog(GSSK_Instance *inst) {
  if (!inst) return;
  inst->mutation_count = 0;
}

GSSK_Status GSSK_ExportMutationLog(GSSK_Instance *inst, char **out_json) {
  if (!inst || !out_json) return GSSK_ERR_UNKNOWN;
  *out_json = NULL;

  cJSON *arr = cJSON_CreateArray();
  if (!arr) return GSSK_ERR_MALLOC_FAILED;

  static const char *op_names[] = {
    "add_node", "add_edge", "deactivate_edge", "deactivate_node", "set_edge_k"
  };

  for (size_t i = 0; i < inst->mutation_count; i++) {
    const GSSK_MutationRecord *r = &inst->mutation_log[i];
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "t",         r->t);
    cJSON_AddStringToObject(obj, "op",        op_names[r->op]);
    cJSON_AddStringToObject(obj, "target_id", r->target_id);
    cJSON_AddStringToObject(obj, "payload",   r->payload);
    cJSON_AddStringToObject(obj, "cause",     r->cause);
    cJSON_AddItemToArray(arr, obj);
  }

  *out_json = cJSON_Print(arr);
  cJSON_Delete(arr);
  return *out_json ? GSSK_SUCCESS : GSSK_ERR_MALLOC_FAILED;
}

/* =========================================================================
 * Phase 4.2 — Replay Engine
 * ========================================================================= */

static GSSK_MutationOp parse_mutation_op(const char *s) {
  if (!s) return GSSK_MUT_SET_EDGE_K;
  if (strcmp(s, "add_node")        == 0) return GSSK_MUT_ADD_NODE;
  if (strcmp(s, "add_edge")        == 0) return GSSK_MUT_ADD_EDGE;
  if (strcmp(s, "deactivate_edge") == 0) return GSSK_MUT_DEACTIVATE_EDGE;
  if (strcmp(s, "deactivate_node") == 0) return GSSK_MUT_DEACTIVATE_NODE;
  return GSSK_MUT_SET_EDGE_K;
}

static void apply_mutation_record(GSSK_Instance *inst,
                                   const GSSK_MutationRecord *r) {
  GSSK_SetMutationCause(inst, r->cause[0] ? r->cause : "replay");
  switch (r->op) {
    case GSSK_MUT_ADD_NODE:
      GSSK_AddNode(inst, r->payload);
      break;
    case GSSK_MUT_ADD_EDGE:
      GSSK_AddEdge(inst, r->payload);
      break;
    case GSSK_MUT_DEACTIVATE_EDGE:
      GSSK_DeactivateEdge(inst, r->target_id);
      break;
    case GSSK_MUT_DEACTIVATE_NODE:
      GSSK_DeactivateNode(inst, r->target_id);
      break;
    case GSSK_MUT_SET_EDGE_K: {
      int idx = GSSK_FindEdgeIdx(inst, r->target_id);
      if (idx >= 0)
        GSSK_SetEdgeK(inst, (size_t)idx, atof(r->payload));
      break;
    }
    case GSSK_MUT_ARCHETYPE_PROPOSAL:
      /* Replay: find motif by canon string stored in payload and propose */
      for (size_t mi = 0; mi < inst->motif_count; mi++) {
        if (strcmp(inst->motifs[mi].canon, r->payload) == 0) {
          inst->motifs[mi].is_candidate = true; /* mark candidate for replay */
          GSSK_ProposeArchetype(inst, mi, r->target_id);
          break;
        }
      }
      break;
  }
}

GSSK_Status GSSK_Replay(const char *initial_json,
                         const char *mutations_json,
                         double target_t,
                         GSSK_Instance **out_inst) {
  if (!out_inst) return GSSK_ERR_UNKNOWN;
  *out_inst = NULL;

  GSSK_Instance *inst = NULL;
  GSSK_Status status = GSSK_Init(initial_json, &inst);
  *out_inst = inst;
  if (status != GSSK_SUCCESS) return status;

  /* Parse mutation list */
  size_t mut_count = 0;
  GSSK_MutationRecord *muts = NULL;

  if (mutations_json && mutations_json[0]) {
    cJSON *arr = cJSON_Parse(mutations_json);
    if (arr && cJSON_IsArray(arr)) {
      mut_count = (size_t)cJSON_GetArraySize(arr);
      if (mut_count > 0) {
        muts = calloc(mut_count, sizeof(GSSK_MutationRecord));
        if (!muts) { cJSON_Delete(arr); return GSSK_ERR_MALLOC_FAILED; }
        for (int i = 0; i < (int)mut_count; i++) {
          cJSON *m  = cJSON_GetArrayItem(arr, i);
          cJSON *t  = cJSON_GetObjectItem(m, "t");
          cJSON *op = cJSON_GetObjectItem(m, "op");
          cJSON *ti = cJSON_GetObjectItem(m, "target_id");
          cJSON *pl = cJSON_GetObjectItem(m, "payload");
          cJSON *ca = cJSON_GetObjectItem(m, "cause");
          muts[i].t  = cJSON_IsNumber(t) ? t->valuedouble : 0.0;
          muts[i].op = parse_mutation_op(cJSON_IsString(op) ? op->valuestring : NULL);
          if (cJSON_IsString(ti)) { strncpy(muts[i].target_id, ti->valuestring, 63); }
          if (cJSON_IsString(pl)) { strncpy(muts[i].payload,   pl->valuestring, 255); }
          if (cJSON_IsString(ca)) { strncpy(muts[i].cause,     ca->valuestring, 63); }
        }
      }
      cJSON_Delete(arr);
    }
  }

  double dt     = inst->config.dt;
  size_t next_m = 0;
  const double eps = dt * 1e-9;

  /* Apply any t=t_start mutations before the first step */
  while (next_m < mut_count && muts[next_m].t <= inst->current_t + eps)
    apply_mutation_record(inst, &muts[next_m++]);

  while (inst->current_t < target_t - eps) {
    status = GSSK_Step(inst, dt);
    if (status != GSSK_SUCCESS && status != GSSK_WARN_SOLVER_DIVERGENCE)
      break;
    /* Apply mutations that fell at the new current_t */
    while (next_m < mut_count && muts[next_m].t <= inst->current_t + eps)
      apply_mutation_record(inst, &muts[next_m++]);
  }

  free(muts);
  return status;
}

/* =========================================================================
 * Phase 5 — Multi-carrier public accessors
 * ========================================================================= */

size_t GSSK_GetCarrierCount(GSSK_Instance *inst) {
  return inst ? inst->carrier_count : 0;
}

const GSSK_Carrier *GSSK_GetCarrier(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->carrier_count) return NULL;
  return &inst->carriers[idx];
}

const char *GSSK_GetNodeCarrier(GSSK_Instance *inst, size_t node_idx) {
  if (!inst || node_idx >= inst->node_count) return "";
  return inst->nodes[node_idx].carrier;
}

const char *GSSK_GetEdgeCarrier(GSSK_Instance *inst, size_t edge_idx) {
  if (!inst || edge_idx >= inst->edge_count) return "";
  return inst->edges[edge_idx].carrier;
}

double GSSK_GetCarrierConservationError(GSSK_Instance *inst, size_t carrier_idx) {
  if (!inst || carrier_idx >= inst->carrier_count || !inst->carrier_cons_error)
    return 0.0;
  return inst->carrier_cons_error[carrier_idx];
}

/* =========================================================================
 * Phase 8 — Archetype accessors
 * ========================================================================= */

size_t GSSK_GetArchetypeCount(GSSK_Instance *inst) {
  return inst ? inst->arch_count : 0;
}

const char *GSSK_GetArchetypeName(GSSK_Instance *inst, size_t idx) {
  if (!inst || idx >= inst->arch_count) return NULL;
  return inst->arch_defns[idx].name;
}

size_t GSSK_GetCompositeCount(GSSK_Instance *inst) {
  return inst ? inst->composite_count : 0;
}

const char *GSSK_GetCompositeID(GSSK_Instance *inst, size_t composite_idx) {
  if (!inst || composite_idx >= inst->composite_count) return NULL;
  return inst->composites[composite_idx].composite_id;
}

/* =========================================================================
 * Serialization helpers
 * ========================================================================= */

static const char *node_type_str(GSSK_NodeType t) {
  switch (t) {
    case NODE_SOURCE:       return "source";
    case NODE_SINK:         return "sink";
    case NODE_CONSTANT:     return "constant";
    case NODE_INTERACTION:  return "interaction";
    case NODE_GAIN:         return "gain";
    case NODE_LOOP_LIMITED: return "loop_limited";
    case NODE_EXCHANGE:     return "exchange";
    case NODE_SWITCH:       return "switch";
    default:                return "storage";
  }
}

static const char *logic_type_str(GSSK_LogicType lt) {
  switch (lt) {
    case GSSK_LOGIC_CONSTANT:    return "constant";
    case GSSK_LOGIC_LINEAR:      return "linear";
    case GSSK_LOGIC_INTERACTION: return "interaction";
    case GSSK_LOGIC_LIMIT:       return "limit";
    case GSSK_LOGIC_THRESHOLD:   return "threshold";
    default:                     return "linear";
  }
}

static const char *method_str(GSSK_Method m) {
  switch (m) {
    case GSSK_METHOD_EULER:     return "euler";
    case GSSK_METHOD_RK4:       return "rk4";
    case GSSK_METHOD_INCIPIENT: return "incipient";
    case GSSK_METHOD_ADAPTIVE:  return "adaptive";
    default:                    return "auto";
  }
}

static cJSON *build_topology_json(GSSK_Instance *inst) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;

  /* metadata */
  cJSON *meta = cJSON_CreateObject();
  cJSON_AddNumberToObject(meta, "schema_version",
                          inst->schema_version > 0 ? inst->schema_version : 3);
  if (inst->model_name[0])        cJSON_AddStringToObject(meta, "name",           inst->model_name);
  if (inst->model_description[0]) cJSON_AddStringToObject(meta, "description",    inst->model_description);
  if (inst->model_author[0])      cJSON_AddStringToObject(meta, "author",         inst->model_author);
  if (inst->created_at[0])        cJSON_AddStringToObject(meta, "created_at",     inst->created_at);
  cJSON_AddStringToObject(meta, "kernel_version", GSK_VERSION_STRING);
  if (inst->model_hash[0])        cJSON_AddStringToObject(meta, "model_hash",     inst->model_hash);
  cJSON_AddItemToObject(root, "metadata", meta);

  /* carriers (Phase 5) */
  if (inst->carrier_count > 0) {
    cJSON *carr_arr = cJSON_CreateArray();
    for (size_t ci = 0; ci < inst->carrier_count; ci++) {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddStringToObject(c, "id",   inst->carriers[ci].id);
      cJSON_AddStringToObject(c, "unit", inst->carriers[ci].unit);
      if (inst->carriers[ci].conserved)
        cJSON_AddTrueToObject(c, "conserved");
      else
        cJSON_AddFalseToObject(c, "conserved");
      cJSON_AddItemToArray(carr_arr, c);
    }
    cJSON_AddItemToObject(root, "carriers", carr_arr);
  }

  /* nodes — value = initial_value (topology IC, not live state) */
  cJSON *nodes = cJSON_CreateArray();
  for (size_t i = 0; i < inst->node_count; i++) {
    GSSK_NodeInternal *nd = &inst->nodes[i];
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "id",    nd->id);
    cJSON_AddStringToObject(n, "type",  node_type_str(nd->type));
    cJSON_AddNumberToObject(n, "value", nd->initial_value);
    if (nd->carrier[0])
      cJSON_AddStringToObject(n, "carrier", nd->carrier);
    if (nd->quality_input > 0.0)
      cJSON_AddNumberToObject(n, "quality_input", nd->quality_input);
    if (nd->output_mode == OUTPUT_REPLICATE)
      cJSON_AddStringToObject(n, "output_mode", "replicate");

    /* Phase 7 — params block for processing-node types */
    if (is_processing_node(nd->type)) {
      cJSON *np = cJSON_CreateObject();
      cJSON_AddNumberToObject(np, "k", nd->node_k);
      if (nd->type == NODE_LOOP_LIMITED)
        cJSON_AddNumberToObject(np, "C", nd->node_C);
      else if (nd->type == NODE_SWITCH)
        cJSON_AddNumberToObject(np, "threshold", nd->node_threshold);
      else if (nd->type == NODE_EXCHANGE)
        cJSON_AddNumberToObject(np, "price", nd->node_price);
      cJSON_AddItemToObject(n, "params", np);
    }

    cJSON_AddItemToArray(nodes, n);
  }
  cJSON_AddItemToObject(root, "nodes", nodes);

  /* edges — k = current k (captures cybernetic adjustments) */
  cJSON *edges = cJSON_CreateArray();
  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *ed = &inst->edges[i];
    cJSON *e = cJSON_CreateObject();
    if (ed->id[0]) cJSON_AddStringToObject(e, "id", ed->id);
    cJSON_AddStringToObject(e, "origin", inst->nodes[ed->origin_idx].id);
    cJSON_AddStringToObject(e, "target", inst->nodes[ed->target_idx].id);
    if (ed->carrier[0]) cJSON_AddStringToObject(e, "carrier", ed->carrier);
    cJSON_AddStringToObject(e, "logic",  logic_type_str(ed->logic));

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "k", ed->k);
    if (ed->control_idx >= 0)
      cJSON_AddStringToObject(params, "control_node",
                              inst->nodes[ed->control_idx].id);
    if (ed->logic == GSSK_LOGIC_THRESHOLD)
      cJSON_AddNumberToObject(params, "threshold", ed->threshold);
    cJSON_AddItemToObject(e, "params", params);

    if (ed->output_mode == OUTPUT_REPLICATE)
      cJSON_AddStringToObject(e, "output_mode", "replicate");
    if (!ed->active)
      cJSON_AddFalseToObject(e, "active");
    cJSON_AddItemToArray(edges, e);
  }
  cJSON_AddItemToObject(root, "edges", edges);

  /* config */
  cJSON *cfg = cJSON_CreateObject();
  cJSON_AddNumberToObject(cfg, "t_start",          inst->config.t_start);
  cJSON_AddNumberToObject(cfg, "t_end",            inst->config.t_end);
  cJSON_AddNumberToObject(cfg, "dt",               inst->config.dt);
  cJSON_AddStringToObject(cfg, "method",           method_str(inst->config.method));
  cJSON_AddNumberToObject(cfg, "solver_tolerance", inst->config.solver_tolerance);
  if (inst->config.rel_tol != 1e-6)
    cJSON_AddNumberToObject(cfg, "rel_tol", inst->config.rel_tol);
  if (inst->config.abs_tol != 1e-9)
    cJSON_AddNumberToObject(cfg, "abs_tol", inst->config.abs_tol);
  if (inst->config.h_min > 0.0)
    cJSON_AddNumberToObject(cfg, "h_min", inst->config.h_min);
  if (inst->config.h_max > 0.0)
    cJSON_AddNumberToObject(cfg, "h_max", inst->config.h_max);
  cJSON_AddItemToObject(root, "config", cfg);

  return root;
}

/* =========================================================================
 * Phase 3.1 — Forward Sensitivity public API
 * ========================================================================= */

GSSK_Status GSSK_EnableForwardSensitivity(GSSK_Instance *inst,
                                           const size_t *param_edge_indices,
                                           size_t param_count) {
  if (!inst || param_count == 0 || !param_edge_indices)
    return GSSK_ERR_UNKNOWN;

  /* Validate all edge indices */
  for (size_t j = 0; j < param_count; j++)
    if (param_edge_indices[j] >= inst->edge_count)
      return GSSK_ERR_NOT_FOUND;

  /* Free previous allocation */
  free(inst->sens_matrix);
  free(inst->sens_param_idx);
  inst->sens_matrix = NULL;
  inst->sens_param_idx = NULL;
  inst->sens_param_count = 0;

  size_t n = inst->node_count;
  size_t m = param_count;
  inst->sens_matrix    = calloc(n * m, sizeof(double));
  inst->sens_param_idx = malloc(m * sizeof(size_t));
  if (!inst->sens_matrix || !inst->sens_param_idx) {
    free(inst->sens_matrix); free(inst->sens_param_idx);
    inst->sens_matrix = NULL; inst->sens_param_idx = NULL;
    return GSSK_ERR_MALLOC_FAILED;
  }
  memcpy(inst->sens_param_idx, param_edge_indices, m * sizeof(size_t));
  inst->sens_param_count = m;
  return GSSK_SUCCESS;
}

void GSSK_DisableForwardSensitivity(GSSK_Instance *inst) {
  if (!inst) return;
  free(inst->sens_matrix);
  free(inst->sens_param_idx);
  inst->sens_matrix      = NULL;
  inst->sens_param_idx   = NULL;
  inst->sens_param_count = 0;
}

double GSSK_GetSensitivity(GSSK_Instance *inst, size_t node_idx,
                            size_t param_idx) {
  if (!inst || !inst->sens_matrix) return 0.0;
  if (node_idx >= inst->node_count || param_idx >= inst->sens_param_count)
    return 0.0;
  return inst->sens_matrix[node_idx * inst->sens_param_count + param_idx];
}

/* =========================================================================
 * Phase 3.2 — Adjoint Sensitivity
 * ========================================================================= */

GSSK_Status GSSK_RunAdjoint(GSSK_Instance *inst,
                             const GSSK_AdjointTarget *targets,
                             size_t target_count,
                             const size_t *param_edge_indices,
                             size_t param_count,
                             double *out_gradient) {
  if (!inst || !targets || target_count == 0 || !param_edge_indices ||
      param_count == 0 || !out_gradient)
    return GSSK_ERR_UNKNOWN;

  size_t n = inst->node_count;
  size_t m = param_count;
  double dt    = inst->config.dt;
  double t0    = inst->config.t_start;
  double t_end = inst->config.t_end;
  size_t steps = (t_end > t0 && dt > 0.0)
                 ? (size_t)((t_end - t0) / dt + 0.5) : 0;
  if (steps == 0) return GSSK_SUCCESS;

  /* Save current live state */
  double  *saved_state = malloc(n * sizeof(double));
  double   t_saved     = inst->current_t;
  size_t   step_saved  = inst->step_count;
  if (!saved_state) return GSSK_ERR_MALLOC_FAILED;
  memcpy(saved_state, inst->state, n * sizeof(double));

  /* Forward pass: run from t_start storing full trajectory Q[step][node] */
  double *traj = malloc(n * (steps + 1) * sizeof(double));
  if (!traj) { free(saved_state); return GSSK_ERR_MALLOC_FAILED; }

  GSSK_Reset(inst);
  memcpy(traj, inst->state, n * sizeof(double));
  for (size_t s = 0; s < steps; s++) {
    GSSK_Step(inst, dt);
    memcpy(traj + (s + 1) * n, inst->state, n * sizeof(double));
  }

  /* Initialize λ(T) = ∂L/∂Q(T) for L = ½ Σ w_i (Q_i(T) − target_i)² */
  double *lam     = calloc(n, sizeof(double));
  double *lam_new = malloc(n * sizeof(double));
  double *J       = malloc(n * n * sizeof(double));
  double *bj      = malloc(n * sizeof(double));
  if (!lam || !lam_new || !J || !bj) {
    free(saved_state); free(traj); free(lam); free(lam_new); free(J); free(bj);
    return GSSK_ERR_MALLOC_FAILED;
  }

  for (size_t ti = 0; ti < target_count; ti++) {
    size_t ni = targets[ti].node_idx;
    if (ni < n) {
      double Q_T = traj[steps * n + ni];
      lam[ni] += targets[ti].weight * (Q_T - targets[ti].target_value);
    }
  }

  memset(out_gradient, 0, m * sizeof(double));

  /* Backward integration: Euler on dλ/dt = -Jᵀ·λ (reversed: Δλ = +dt·Jᵀ·λ) */
  for (int s = (int)steps; s >= 0; s--) {
    double *Q_s = traj + (size_t)s * n;

    /* Accumulate gradient: g_j += dt · (λᵀ · B_j(Q_s)) */
    for (size_t j = 0; j < m; j++) {
      compute_param_deriv(inst, Q_s, param_edge_indices[j], bj);
      double c = 0.0;
      for (size_t k = 0; k < n; k++) c += lam[k] * bj[k];
      out_gradient[j] += dt * c;
    }

    if (s == 0) break;

    /* Adjoint step backward: λ(t-dt) = λ(t) + dt·Jᵀ(Q_s)·λ(t) */
    build_jacobian(inst, Q_s, J);
    for (size_t k = 0; k < n; k++) {
      double sum = 0.0;
      for (size_t i = 0; i < n; i++) sum += J[i * n + k] * lam[i];
      lam_new[k] = lam[k] + dt * sum;
    }
    memcpy(lam, lam_new, n * sizeof(double));
  }

  /* Restore live state */
  memcpy(inst->state, saved_state, n * sizeof(double));
  inst->current_t  = t_saved;
  inst->step_count = step_saved;

  free(saved_state); free(traj); free(lam); free(lam_new); free(J); free(bj);
  return GSSK_SUCCESS;
}

/* =========================================================================
 * Phase 3.3 — Transformity Sensitivity
 * ========================================================================= */

double GSSK_GetTransformitySensitivity(GSSK_Instance *inst,
                                        size_t node_idx, size_t edge_idx) {
  if (!inst || !inst->quality_enabled || !inst->transformity) return 0.0;
  if (node_idx >= inst->node_count || edge_idx >= inst->edge_count) return 0.0;

  double *dTr = malloc(inst->node_count * sizeof(double));
  if (!dTr) return 0.0;
  compute_quality_sensitivity(inst, edge_idx, dTr);
  double result = dTr[node_idx];
  free(dTr);
  return result;
}

void GSSK_FreeString(char *s) {
  free(s);
}

GSSK_Status GSSK_SerializeModel(GSSK_Instance *inst, char **out_json) {
  if (!inst || !out_json) return GSSK_ERR_UNKNOWN;
  *out_json = NULL;

  cJSON *root = build_topology_json(inst);
  if (!root) return GSSK_ERR_MALLOC_FAILED;

  *out_json = cJSON_Print(root);
  cJSON_Delete(root);
  return *out_json ? GSSK_SUCCESS : GSSK_ERR_MALLOC_FAILED;
}

GSSK_Status GSSK_SerializeSnapshot(GSSK_Instance *inst, char **out_json) {
  if (!inst || !out_json) return GSSK_ERR_UNKNOWN;
  *out_json = NULL;

  cJSON *root = build_topology_json(inst);
  if (!root) return GSSK_ERR_MALLOC_FAILED;

  /* snapshot block */
  cJSON *snap = cJSON_CreateObject();
  cJSON_AddNumberToObject(snap, "t",    inst->current_t);
  cJSON_AddNumberToObject(snap, "dt",   inst->config.dt);
  cJSON_AddNumberToObject(snap, "step", (double)inst->step_count);

  /* state: [{id, Q, Tr}] keyed by node ID */
  cJSON *state_arr = cJSON_CreateArray();
  for (size_t i = 0; i < inst->node_count; i++) {
    cJSON *se = cJSON_CreateObject();
    cJSON_AddStringToObject(se, "id", inst->nodes[i].id);
    cJSON_AddNumberToObject(se, "Q",  inst->state[i]);
    double tr = (inst->quality_enabled && inst->transformity)
                    ? inst->transformity[i] : 0.0;
    cJSON_AddNumberToObject(se, "Tr", tr);
    cJSON_AddItemToArray(state_arr, se);
  }
  cJSON_AddItemToObject(snap, "state", state_arr);

  /* edge_k: [{id, k}] — only named edges */
  cJSON *ek_arr = cJSON_CreateArray();
  for (size_t i = 0; i < inst->edge_count; i++) {
    if (!inst->edges[i].id[0]) continue;
    cJSON *ee = cJSON_CreateObject();
    cJSON_AddStringToObject(ee, "id", inst->edges[i].id);
    cJSON_AddNumberToObject(ee, "k",  inst->edges[i].k);
    cJSON_AddItemToArray(ek_arr, ee);
  }
  cJSON_AddItemToObject(snap, "edge_k", ek_arr);

  /* solver state */
  cJSON *slvr = cJSON_CreateObject();
  cJSON_AddStringToObject(slvr, "confidence",
      inst->confidence == GSSK_CONFIDENCE_HIGH ? "high" : "degraded");
  cJSON_AddBoolToObject(slvr, "incipient_eligible", inst->incipient_eligible);
  cJSON_AddItemToObject(snap, "solver", slvr);

  cJSON_AddNullToObject(snap, "rng_state");

  /* mutation_log: [{t, op, target_id, payload, cause}] */
  static const char *op_names_snap[] = {
    "add_node", "add_edge", "deactivate_edge", "deactivate_node", "set_edge_k"
  };
  cJSON *mlog = cJSON_CreateArray();
  for (size_t i = 0; i < inst->mutation_count; i++) {
    const GSSK_MutationRecord *r = &inst->mutation_log[i];
    cJSON *mo = cJSON_CreateObject();
    cJSON_AddNumberToObject(mo, "t",         r->t);
    cJSON_AddStringToObject(mo, "op",        op_names_snap[r->op]);
    cJSON_AddStringToObject(mo, "target_id", r->target_id);
    cJSON_AddStringToObject(mo, "payload",   r->payload);
    cJSON_AddStringToObject(mo, "cause",     r->cause);
    cJSON_AddItemToArray(mlog, mo);
  }
  cJSON_AddItemToObject(snap, "mutation_log", mlog);

  cJSON_AddItemToObject(root, "snapshot", snap);

  *out_json = cJSON_Print(root);
  cJSON_Delete(root);
  return *out_json ? GSSK_SUCCESS : GSSK_ERR_MALLOC_FAILED;
}

void GSSK_Free(GSSK_Instance *inst) {
  if (!inst) return;
  free(inst->state);
  free(inst->dQ);
  free(inst->k2);
  free(inst->k3);
  free(inst->k4);
  free(inst->k5);
  free(inst->k6);
  free(inst->k7);
  free(inst->tmp_state);
  free(inst->idc_state);
  free(inst->edge_error);
  free(inst->events);
  free(inst->nodes);
  free(inst->edges);
  free(inst->transformity);
  free(inst->quality_flow);
  free(inst->edge_qflow);
  free(inst->sens_matrix);
  free(inst->sens_param_idx);
  free(inst->mutation_log);
  free(inst->carriers);
  free(inst->carrier_cons_error);
  free(inst);
}
