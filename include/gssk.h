/**
 * @file gssk.h
 * @brief General Systems Kernel (GSK) Core API — v3
 *
 * This file defines the public interface for the GSK numerical engine.
 * The kernel is designed for high-performance simulation of complex systems
 * based on General Systems Theory and Odum's Energy Systems Language.
 *
 * Odum's inter-block four-position array (Modelling for All Scales, App. A):
 *   Position 1 — Code        → edge.carrier field (what is flowing)
 *   Position 2 — Force       → implicit in logic type + origin node Q
 *   Position 3 — Flow        → computed by GSK_Step() per logic type
 *   Position 4 — Transformity → computed by quality accounting pass
 */

#ifndef GSSK_H
#define GSSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Enumerations
 * ========================================================================= */

/**
 * @brief Logic types for flow calculations (Odum Force × Flow encoding).
 */
typedef enum {
  GSSK_LOGIC_CONSTANT,    /**< Fixed flow rate: F = k */
  GSSK_LOGIC_LINEAR,      /**< Proportional to source: F = k × Q_origin */
  GSSK_LOGIC_INTERACTION, /**< Work gate / Riccati: F = k × Q_origin × Q_control */
  GSSK_LOGIC_LIMIT,       /**< Saturation (Michaelis-Menten): F = kQ/(1+Q/C) */
  GSSK_LOGIC_THRESHOLD    /**< Boolean switch: F = k if Q > threshold, else 0 */
} GSSK_LogicType;

/**
 * @brief Integration methods.
 *
 * GSSK_METHOD_AUTO (default): kernel selects IDC where all edges are
 *   constant/linear/interaction; falls back to RK4 for limit/threshold edges.
 *   Runs both solvers in parallel and cross-validates each step.
 *
 * GSSK_METHOD_INCIPIENT: force IDC. Eligible edges use matrix-exponential;
 *   ineligible edges fall back silently to RK4 sub-step.
 *
 * GSSK_METHOD_EULER / GSSK_METHOD_RK4: classic numerical methods.
 *   Use for debugging or when IDC overhead is undesirable.
 */
typedef enum {
  GSSK_METHOD_AUTO,
  GSSK_METHOD_EULER,
  GSSK_METHOD_RK4,
  GSSK_METHOD_INCIPIENT,
  GSSK_METHOD_ADAPTIVE /**< Phase 2: DOPRI5 5(4) explicit + PI step-size control */
} GSSK_Method;

/**
 * @brief Dual-solver confidence level.
 *
 * In AUTO mode the kernel runs IDC and RK4 in parallel each step.
 * If max relative error < solver_tolerance → GSSK_CONFIDENCE_HIGH.
 * If error ≥ solver_tolerance → GSSK_CONFIDENCE_DEGRADED.
 * Cybernetic k-adjustments are frozen while DEGRADED.
 */
typedef enum {
  GSSK_CONFIDENCE_HIGH,     /**< IDC and RK4 agree within tolerance */
  GSSK_CONFIDENCE_DEGRADED  /**< Solvers have diverged — RK4 result used */
} GSSK_SolverConfidence;

/**
 * @brief Error / status codes returned by the kernel.
 */
typedef enum {
  GSSK_SUCCESS = 0,
  GSSK_ERR_INVALID_JSON,
  GSSK_ERR_MALLOC_FAILED,
  GSSK_ERR_SCHEMA_VIOLATION,
  GSSK_ERR_DIVERGENCE,           /**< Numerical instability: NaN/Inf detected */
  GSSK_ERR_NOT_FOUND,            /**< Node or edge ID not found */
  GSSK_ERR_UNSUPPORTED_SCHEMA_VERSION, /**< Schema version not supported */
  GSSK_ERR_UNKNOWN,
  GSSK_WARN_SOLVER_DIVERGENCE    /**< AUTO mode: IDC vs RK4 error > tolerance */
} GSSK_Status;

/* =========================================================================
 * Version information
 * ========================================================================= */

#define GSK_VERSION_MAJOR 4
#define GSK_VERSION_MINOR 0
#define GSK_VERSION_PATCH 0
#define GSK_VERSION_STRING "4.0.0"

/* Numeric version for comparison: (major << 16) | (minor << 8) | patch */
#define GSK_VERSION_CODE(major, minor, patch) \
  (((major) << 16) | ((minor) << 8) | (patch))

/* =========================================================================
 * Opaque instance handle
 * ========================================================================= */

/**
 * @brief Opaque handle to a GSSK instance. All state is stored here.
 *        Every GSSK_Init must have a corresponding GSSK_Free.
 */
typedef struct GSSK_Instance GSSK_Instance;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Initialize a GSSK instance from a JSON model string.
 *
 * Parses nodes, edges, config. Allocates all internal state.
 * Calls GSSK_ReclassifyNetwork() to determine solver eligibility.
 *
 * @param json_data  JSON model conforming to gssk.schema.json.
 * @param out_inst   Populated with a new instance. Caller must call GSSK_Free.
 *                   Always non-NULL on return even if status != GSSK_SUCCESS.
 * @return GSSK_Status  GSSK_SUCCESS or an error code.
 */
GSSK_Status GSSK_Init(const char *json_data, GSSK_Instance **out_inst);

/**
 * @brief Free all memory associated with an instance.
 */
void GSSK_Free(GSSK_Instance *inst);

/**
 * @brief Reset the simulation to its initial state (t=t_start, Q=initial values).
 */
void GSSK_Reset(GSSK_Instance *inst);

/* =========================================================================
 * Simulation
 * ========================================================================= */

/**
 * @brief Advance the simulation by one time step.
 *
 * In AUTO mode: runs IDC (where eligible) and RK4 in parallel, compares
 * results, updates solver confidence, and applies the quality accounting pass.
 *
 * @param inst  GSSK instance.
 * @param dt    Time step (positive). Typically matches config.dt.
 * @return GSSK_Status  GSSK_SUCCESS, GSSK_ERR_DIVERGENCE,
 *                      or GSSK_WARN_SOLVER_DIVERGENCE (AUTO mode only).
 */
GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt);

/* =========================================================================
 * State accessors
 * ========================================================================= */

/**
 * @brief Read the current state vector Q[].
 *        Index i corresponds to node at position i in the original JSON array.
 * @return const double*  Read-only array of length GSSK_GetStateSize().
 */
const double *GSSK_GetState(GSSK_Instance *inst);

/**
 * @brief Number of nodes (= length of GSSK_GetState()).
 */
size_t GSSK_GetStateSize(GSSK_Instance *inst);

/* =========================================================================
 * Quality accounting accessors
 * ========================================================================= */

/**
 * @brief Transformation ratio (Tr) per node, computed by quality accounting pass.
 *        Only valid after at least one GSSK_Step() and if any source node has
 *        quality_input > 0. Returns NULL if quality accounting is disabled.
 * @return const double*  Read-only array of length GSSK_GetStateSize().
 */
const double *GSSK_GetTransformationRatio(GSSK_Instance *inst);

/**
 * @brief Quality flow (Tr × flow rate) per node, summed over all inflows.
 *        Analogous to EmPower in Odum's emergy accounting.
 * @return const double*  Read-only array of length GSSK_GetStateSize().
 */
const double *GSSK_GetQualityFlow(GSSK_Instance *inst);

/**
 * @brief Quality flow on a specific edge: Tr[origin] × flow_rate.
 * @param edge_idx  Edge index (0 … GSSK_GetEdgeCount()-1).
 * @return double  Quality flow on this edge, or 0.0 if quality disabled.
 */
double GSSK_GetEdgeQualityFlow(GSSK_Instance *inst, size_t edge_idx);

/**
 * @brief Current dual-solver confidence level (AUTO mode).
 *        Always GSSK_CONFIDENCE_HIGH in EULER/RK4/INCIPIENT modes.
 */
GSSK_SolverConfidence GSSK_GetSolverConfidence(GSSK_Instance *inst);

/* =========================================================================
 * Metadata accessors
 * ========================================================================= */

/**
 * @brief Get the schema version of the loaded model (2 or 3).
 */
int GSSK_GetSchemaVersion(GSSK_Instance *inst);

/**
 * @brief Get model name from metadata.
 * @return const char*  Model name, or empty string if not set. Never NULL.
 */
const char *GSSK_GetModelName(GSSK_Instance *inst);

/**
 * @brief Get model description from metadata.
 * @return const char*  Model description, or empty string if not set. Never NULL.
 */
const char *GSSK_GetModelDescription(GSSK_Instance *inst);

/**
 * @brief Get kernel version that created this model.
 * @return const char*  Kernel version string (e.g. "3.0.0"), or empty if not set.
 */
const char *GSSK_GetModelKernelVersion(GSSK_Instance *inst);

/**
 * @brief Get model hash (SHA256) from metadata.
 * @return const char*  SHA256 hex string, or empty if not set.
 */
const char *GSSK_GetModelHash(GSSK_Instance *inst);

/**
 * @brief Get the current GSK kernel version.
 * @return const char*  Kernel version string (e.g. "3.0.0").
 */
const char *GSSK_GetVersionString(void);

/**
 * @brief Get the current GSK kernel version as an integer code.
 * Format: (major << 16) | (minor << 8) | patch.
 * For example, 3.0.0 = 0x030000, 3.1.2 = 0x030102.
 */
uint32_t GSSK_GetVersionCode(void);

/* =========================================================================
 * Node/Edge lookup
 * ========================================================================= */

/**
 * @brief Get node ID by index. Returns NULL if out of bounds.
 */
const char *GSSK_GetNodeID(GSSK_Instance *inst, size_t index);

/**
 * @brief Find node index by ID. Returns -1 if not found.
 */
int GSSK_FindNodeIdx(GSSK_Instance *inst, const char *id);

/**
 * @brief Get node type as a string ("source", "storage", "sink",
 *        "interaction", "gain", "loop_limited", "exchange", "switch").
 * @return const char* — valid for lifetime of inst. Never NULL.
 */
const char *GSSK_GetNodeTypeString(GSSK_Instance *inst, size_t node_idx);

/**
 * @brief Get edge ID by index. Returns NULL if edge has no id or index is OOB.
 */
const char *GSSK_GetEdgeID(GSSK_Instance *inst, size_t index);

/**
 * @brief Find edge index by ID. Returns -1 if not found.
 */
int GSSK_FindEdgeIdx(GSSK_Instance *inst, const char *id);

/**
 * @brief Number of edges (active + inactive).
 */
size_t GSSK_GetEdgeCount(GSSK_Instance *inst);

/* =========================================================================
 * Edge parameter mutation (cybernetic mode)
 * ========================================================================= */

/**
 * @brief Get edge conductance coefficient k.
 */
double GSSK_GetEdgeK(GSSK_Instance *inst, size_t index);

/**
 * @brief Set edge conductance coefficient k.
 *        Call GSSK_ReclassifyNetwork() after if k changes eligibility.
 */
void GSSK_SetEdgeK(GSSK_Instance *inst, size_t index, double k);

/* =========================================================================
 * Topology mutation API
 * ========================================================================= */

/**
 * @brief Add a new node at runtime.
 *
 * The new node is appended to the end of the state vector. Existing column
 * indices are unchanged. Callers must refresh their node manifest via
 * GSSK_GetNodeID() after calling this function.
 *
 * Automatically calls GSSK_ReclassifyNetwork().
 *
 * @param json_node_fragment  JSON object conforming to the Node schema, e.g.:
 *   {"id":"car","type":"storage","value":0.0}
 * @return GSSK_Status  GSSK_SUCCESS or error.
 */
GSSK_Status GSSK_AddNode(GSSK_Instance *inst, const char *json_node_fragment);

/**
 * @brief Add a new edge at runtime.
 *
 * Origin and target nodes must already exist (by id). Automatically calls
 * GSSK_ReclassifyNetwork().
 *
 * @param json_edge_fragment  JSON object conforming to the Edge schema, e.g.:
 *   {"id":"loan","origin":"account","target":"car","logic":"constant","params":{"k":500.0}}
 * @return GSSK_Status  GSSK_SUCCESS or error.
 */
GSSK_Status GSSK_AddEdge(GSSK_Instance *inst, const char *json_edge_fragment);

/**
 * @brief Deactivate an edge by ID (sets k=0, marks inactive).
 *        The edge slot is retained. Reactivate by calling GSSK_SetEdgeK().
 *        Automatically calls GSSK_ReclassifyNetwork().
 */
GSSK_Status GSSK_DeactivateEdge(GSSK_Instance *inst, const char *edge_id);

/**
 * @brief Deactivate a node by ID (treats as sink, zeroes all connected edges).
 *        Automatically calls GSSK_ReclassifyNetwork().
 */
GSSK_Status GSSK_DeactivateNode(GSSK_Instance *inst, const char *node_id);

/**
 * @brief Re-classify the network for IDC solver eligibility.
 *
 * Called automatically after GSSK_AddNode, GSSK_AddEdge, GSSK_DeactivateEdge/Node.
 *
 * Phase 1: always sets incipient_eligible = true. All edge types now have
 * IDC treatment (limit via effective-conductance linearisation; threshold via
 * constant forcing), so IDC runs on every step regardless of edge types.
 */
GSSK_Status GSSK_ReclassifyNetwork(GSSK_Instance *inst);

/* =========================================================================
 * Config accessors
 * ========================================================================= */

double GSSK_GetTStart(GSSK_Instance *inst);
double GSSK_GetTEnd(GSSK_Instance *inst);
double GSSK_GetDt(GSSK_Instance *inst);

/* =========================================================================
 * Simulation clock (0.3 Round-trip Serialization)
 * ========================================================================= */

/**
 * @brief Current simulation time. Starts at config.t_start; advances by dt
 *        with each successful GSSK_Step(). Restored from snapshot.t on resume.
 */
double GSSK_GetCurrentTime(GSSK_Instance *inst);

/**
 * @brief Number of steps taken since the last GSSK_Init() or GSSK_Reset().
 *        Restored from snapshot.step on resume.
 */
size_t GSSK_GetStepCount(GSSK_Instance *inst);

/* =========================================================================
 * Round-trip serialization (0.3)
 * ========================================================================= */

/**
 * @brief Serialize the current topology to JSON.
 *
 * Emits nodes (with initial_value ICs), edges (with current k, capturing
 * cybernetic adjustments), config, and metadata. Does NOT include a snapshot
 * block — the result is a fresh model that restarts from t_start on Init.
 *
 * @param inst      GSSK instance.
 * @param out_json  Populated with a heap-allocated JSON string.
 *                  Caller must free with GSSK_FreeString().
 * @return GSSK_SUCCESS or GSSK_ERR_MALLOC_FAILED.
 */
GSSK_Status GSSK_SerializeModel(GSSK_Instance *inst, char **out_json);

/**
 * @brief Serialize the current topology AND live state to JSON.
 *
 * Extends GSSK_SerializeModel with a top-level `snapshot` block containing:
 *   - t, dt, step counter
 *   - Q[] and Tr[] vectors keyed by stable node ID
 *   - per-edge k (captures cybernetic adjustments)
 *   - solver state (confidence, IDC eligibility)
 *   - rng_state (null placeholder until Phase 4 RNG is introduced)
 *
 * Passing the result back to GSSK_Init() resumes from t = snapshot.t rather
 * than t = config.t_start, satisfying the round-trip property:
 *   Init → Step×N → SerializeSnapshot → Init → Step×M
 *   is bit-identical to Init → Step×(N+M) for the same solver/dt.
 *
 * @param inst      GSSK instance.
 * @param out_json  Populated with a heap-allocated JSON string.
 *                  Caller must free with GSSK_FreeString().
 * @return GSSK_SUCCESS or GSSK_ERR_MALLOC_FAILED.
 */
GSSK_Status GSSK_SerializeSnapshot(GSSK_Instance *inst, char **out_json);

/**
 * @brief Free a JSON string returned by GSSK_SerializeModel or
 *        GSSK_SerializeSnapshot. Equivalent to free() but keeps ownership
 *        explicit across language boundaries.
 */
void GSSK_FreeString(char *s);

/* =========================================================================
 * Phase 1 — IDC error estimates and threshold event log
 * ========================================================================= */

/**
 * @brief Per-edge relative error between IDC and RK4 flows for the last step.
 *        |flow_idc - flow_rk4| / max(|flow_rk4|, 1e-12).
 *        Always 0.0 in EULER/RK4 modes.
 */
double GSSK_GetEdgeErrorEstimate(GSSK_Instance *inst, size_t edge_idx);

/**
 * @brief Step-level max error: max over all edges of GSSK_GetEdgeErrorEstimate.
 *        Used internally as the criterion for solver confidence.
 */
double GSSK_GetStepErrorEstimate(GSSK_Instance *inst);

/**
 * @brief Number of threshold crossing events recorded since last GSSK_Reset().
 */
size_t GSSK_GetEventCount(GSSK_Instance *inst);

/**
 * @brief Simulation time of event at event_idx.
 * @return 0.0 if event_idx out of range.
 */
double GSSK_GetEventTime(GSSK_Instance *inst, size_t event_idx);

/**
 * @brief Edge ID string of the threshold edge that crossed at event_idx.
 * @return Pointer to internal C string (valid for lifetime of inst).
 *         NULL if event_idx out of range.
 */
const char *GSSK_GetEventEdgeID(GSSK_Instance *inst, size_t event_idx);

/**
 * @brief Crossing direction at event_idx.
 * @return +1 if Q_origin crossed threshold upward, -1 downward, 0 if OOB.
 */
int GSSK_GetEventDirection(GSSK_Instance *inst, size_t event_idx);

/* =========================================================================
 * Phase 2 — Adaptive Numerics (DOPRI5 + Conservation Diagnostics)
 * ========================================================================= */

/**
 * @brief Diagnostic hook function types (Phase 2.2 / Phase 4.4).
 *
 * on_step      — called after each accepted DOPRI5 sub-step.
 * on_event     — called when a threshold event is emitted.
 * on_conservation_warning — called when |ΔQ_total / Q_total| > rel_tol.
 * on_mutation  — called after any successful topology mutation (Phase 4).
 * on_divergence — called when NaN/Inf is detected before GSSK_ERR_DIVERGENCE (Phase 4).
 * ctx          — user pointer forwarded to every callback.
 */
typedef void (*GSSK_OnStepFn)(void *ctx, double t, double h, double err_norm);
typedef void (*GSSK_OnEventFn)(void *ctx, double t, const char *edge_id, int dir);
typedef void (*GSSK_OnConservationWarningFn)(void *ctx, double t,
                                              double conservation_err);

/* Forward declaration — GSSK_MutationRecord defined in Phase 4 section below */
typedef struct GSSK_MutationRecord GSSK_MutationRecord;

typedef void (*GSSK_OnMutationFn)(void *ctx, const GSSK_MutationRecord *rec);
typedef void (*GSSK_OnDivergenceFn)(void *ctx, double t, const char *node_id,
                                     double value);

typedef struct {
  GSSK_OnStepFn                 on_step;
  GSSK_OnEventFn                on_event;
  GSSK_OnConservationWarningFn  on_conservation_warning;
  void                         *ctx;
  /* Phase 4 extensions — zero-initialize to disable */
  GSSK_OnMutationFn             on_mutation;
  GSSK_OnDivergenceFn           on_divergence;
} GSSK_DiagHooks;

/**
 * @brief Register diagnostic hooks.  Pass NULL to clear all hooks.
 */
void GSSK_SetDiagHooks(GSSK_Instance *inst, const GSSK_DiagHooks *hooks);

/**
 * @brief Advance the simulation by one adaptively-sized DOPRI5 step.
 *
 * Uses the internally managed step size (inst→h_next, initialised from
 * config.dt).  The actual step taken may be smaller due to error control or
 * proximity to t_end.  Always advances current_t by the accepted h.
 *
 * Only meaningful when method = GSSK_METHOD_ADAPTIVE.  Calling it for other
 * methods is equivalent to calling GSSK_Step with config.dt.
 *
 * @return GSSK_SUCCESS on acceptance, GSSK_ERR_DIVERGENCE on NaN/Inf,
 *         GSSK_WARN_SOLVER_DIVERGENCE if step could not meet tolerance
 *         even at h_min (step accepted with warning).
 */
GSSK_Status GSSK_StepAdaptive(GSSK_Instance *inst);

/**
 * @brief Actual step size h used in the most recently accepted step.
 *        For fixed-step modes this equals config.dt.
 */
double GSSK_GetLastStepSize(GSSK_Instance *inst);

/**
 * @brief Suggested step size for the next GSSK_StepAdaptive call.
 *        Updated by the PI controller after every accepted step.
 */
double GSSK_GetNextStepSize(GSSK_Instance *inst);

/**
 * @brief Relative change in total storage-Q over the last step.
 *
 * For a fully closed system (no source/sink nodes) this should be near
 * machine epsilon.  Non-zero values indicate either open-system flow or
 * numerical leakage.  Only meaningful after at least one step.
 */
double GSSK_GetConservationError(GSSK_Instance *inst);

/* =========================================================================
 * Phase 3 — Sensitivity Analysis
 * ========================================================================= */

/**
 * @brief Enable forward sensitivity tracking for a set of edge parameters.
 *
 * Allocates and zeroes a sensitivity matrix S[node_count × param_count] where
 * S[i][j] = ∂Q_i/∂k_j.  Updated each GSSK_Step call via the augmented ODE
 * dS/dt = J(Q)·S + B(Q), integrated with Euler (first-order).
 *
 * @param param_edge_indices  Edge indices whose k values are tracked.
 * @param param_count         Number of parameters (columns of S).
 * @return GSSK_SUCCESS or GSSK_ERR_MALLOC_FAILED / GSSK_ERR_NOT_FOUND.
 */
GSSK_Status GSSK_EnableForwardSensitivity(GSSK_Instance *inst,
                                           const size_t *param_edge_indices,
                                           size_t param_count);

/**
 * @brief Disable sensitivity tracking and free the sensitivity matrix.
 */
void GSSK_DisableForwardSensitivity(GSSK_Instance *inst);

/**
 * @brief Read ∂Q[node_idx] / ∂k[param_idx] from the sensitivity matrix.
 *
 * @param param_idx  Column of S — index into the param_edge_indices array
 *                   supplied to GSSK_EnableForwardSensitivity, not the edge
 *                   index itself.
 * @return 0.0 if sensitivity is disabled or indices are out of range.
 */
double GSSK_GetSensitivity(GSSK_Instance *inst, size_t node_idx,
                            size_t param_idx);

/**
 * @brief Observation / target for the adjoint solver.
 *
 * The adjoint objective is L = ½ Σ_i weight_i · (Q_i(T) − target_value_i)².
 * The terminal adjoint is λ_i(T) = weight_i · (Q_i(T) − target_value_i).
 */
typedef struct {
  size_t node_idx;      /**< State-vector index of the observed node. */
  double target_value;  /**< Desired Q_i(T). */
  double weight;        /**< Loss weight (1.0 = unit weight). */
} GSSK_AdjointTarget;

/**
 * @brief Compute gradient ∂L/∂k via adjoint (backward) integration.
 *
 * Runs a fresh forward pass from config.t_start to config.t_end (using fixed
 * config.dt), stores the trajectory, then integrates the adjoint ODE backward
 * to compute ∂L/∂k_j for each j in param_edge_indices.
 *
 * Restores inst->state and clock to their pre-call values when done.
 *
 * @param targets             Terminal objective terms (see GSSK_AdjointTarget).
 * @param target_count        Number of target terms.
 * @param param_edge_indices  Edges whose k sensitivity is computed.
 * @param param_count         Length of param_edge_indices.
 * @param out_gradient        Caller-allocated array of length param_count.
 *                            Filled with ∂L/∂k_j on return.
 */
GSSK_Status GSSK_RunAdjoint(GSSK_Instance *inst,
                             const GSSK_AdjointTarget *targets,
                             size_t target_count,
                             const size_t *param_edge_indices,
                             size_t param_count,
                             double *out_gradient);

/**
 * @brief ∂Tr[node_idx] / ∂k[edge_idx] via implicit differentiation.
 *
 * Differentiates the quality system M·Tr = b w.r.t. k_{edge_idx},
 * giving the sensitivity of every node's transformity to that parameter.
 * Only valid if quality accounting is enabled and GSSK_Step has been called.
 *
 * @return 0.0 if quality is disabled or indices are out of range.
 */
double GSSK_GetTransformitySensitivity(GSSK_Instance *inst,
                                        size_t node_idx, size_t edge_idx);

/* =========================================================================
 * Error reporting
 * ========================================================================= */

/**
 * @brief Get a human-readable description of the last error.
 */
const char *GSSK_GetErrorDescription(GSSK_Instance *inst);

/* =========================================================================
 * Calibration / Ensemble (advanced.c)
 * ========================================================================= */

/**
 * @brief Single observation point for calibration.
 */
typedef struct {
  double time;
  double value;
} GSSK_Observation;

/**
 * @brief Set of observations for a specific node.
 */
typedef struct {
  const char *node_id;
  GSSK_Observation *data;
  size_t count;
} GSSK_NodeObservations;

/**
 * @brief Result structure for ensemble forecasting.
 */
typedef struct {
  double *min_envelope;  /**< Size: node_count × step_count */
  double *max_envelope;  /**< Size: node_count × step_count */
  double *mean_envelope; /**< Size: node_count × step_count */
  size_t node_count;
  size_t step_count;
} GSSK_EnsembleResult;

/**
 * @brief Run ensemble forecasting with parameter perturbation.
 */
GSSK_EnsembleResult *GSSK_EnsembleForecast(GSSK_Instance *inst, size_t runs,
                                           double perturbation);

/**
 * @brief Free ensemble results.
 */
void GSSK_FreeEnsembleResult(GSSK_EnsembleResult *res);

/**
 * @brief Run parameter calibration against observed data (Monte-Carlo DE path).
 */
GSSK_Status GSSK_Calibrate(GSSK_Instance *inst, GSSK_NodeObservations *obs,
                           size_t obs_count, int iterations);

/**
 * @brief Calibrate using the original differential-evolution Monte-Carlo path.
 *        Equivalent to GSSK_Calibrate.
 */
GSSK_Status GSSK_CalibrateMonteCarlo(GSSK_Instance *inst,
                                      GSSK_NodeObservations *obs,
                                      size_t obs_count, int iterations);

/**
 * @brief Gradient-based calibration using Levenberg-Marquardt + forward sens.
 *
 * Iteratively updates param_edge_indices[j].k to minimise MSE against obs.
 *
 * @param param_edge_indices  Edges whose k is tuned.
 * @param param_count         Number of tunable parameters.
 * @param iterations          Maximum outer L-M iterations.
 */
GSSK_Status GSSK_CalibrateGradient(GSSK_Instance *inst,
                                    GSSK_NodeObservations *obs,
                                    size_t obs_count,
                                    const size_t *param_edge_indices,
                                    size_t param_count,
                                    int iterations);

/* =========================================================================
 * Phase 5 — Multi-Carrier Schema
 * ========================================================================= */

/**
 * @brief A carrier definition from the top-level `carriers` array.
 *
 * Carriers are the "code" in Odum's four-position inter-block channel (Position 1).
 * Each carrier has a human-readable unit and a conservation flag.  When
 * `conserved` is true the kernel tracks a per-carrier conservation error.
 */
typedef struct {
  char id[32];    /**< Unique identifier, e.g. "money", "energy", "material". */
  char unit[32];  /**< Physical unit string, e.g. "AUD", "kWh", "kg". */
  bool conserved; /**< If true, total storage-Q for this carrier is tracked. */
} GSSK_Carrier;

/** Number of carriers declared in the top-level `carriers` array. */
size_t GSSK_GetCarrierCount(GSSK_Instance *inst);

/**
 * @brief Pointer to carrier definition at index.
 * @return NULL if idx >= GSSK_GetCarrierCount().
 */
const GSSK_Carrier *GSSK_GetCarrier(GSSK_Instance *inst, size_t idx);

/**
 * @brief Carrier string declared on node at node_idx.
 * @return Empty string if no carrier is set.  Never NULL.
 */
const char *GSSK_GetNodeCarrier(GSSK_Instance *inst, size_t node_idx);

/**
 * @brief Carrier string declared on edge at edge_idx (Odum Position 1).
 * @return Empty string if no carrier is set.  Never NULL.
 */
const char *GSSK_GetEdgeCarrier(GSSK_Instance *inst, size_t edge_idx);

/**
 * @brief Per-carrier conservation error from the last step.
 *
 * Only meaningful for carriers declared with `conserved: true`.  Returns the
 * relative change in total storage-Q for all nodes whose carrier matches
 * carriers[carrier_idx].id.
 *
 * For an open system (source or sink nodes present for this carrier), this
 * reflects net inflow/outflow rather than a numerical leak.  Check against
 * your own tolerance.
 *
 * @return 0.0 if carrier_idx is out of range, carrier is not conserved,
 *         or no step has been taken yet.
 */
double GSSK_GetCarrierConservationError(GSSK_Instance *inst, size_t carrier_idx);

/* =========================================================================
 * Phase 4 — Replay & Observability
 * ========================================================================= */

/**
 * @brief Operation types for the mutation log.
 */
typedef enum {
  GSSK_MUT_ADD_NODE,
  GSSK_MUT_ADD_EDGE,
  GSSK_MUT_DEACTIVATE_EDGE,
  GSSK_MUT_DEACTIVATE_NODE,
  GSSK_MUT_SET_EDGE_K
} GSSK_MutationOp;

/**
 * @brief One entry in the mutation log.
 *
 * Appended on every successful topology mutation (AddNode, AddEdge,
 * DeactivateEdge, DeactivateNode, SetEdgeK).
 */
struct GSSK_MutationRecord {
  double           t;             /**< current_t when mutation occurred. */
  GSSK_MutationOp  op;            /**< Which operation was performed. */
  char             target_id[64]; /**< Node or edge ID affected. */
  char             payload[256];  /**< JSON fragment (add ops) or value string (set_k). */
  char             cause[64];     /**< "user", "calibration", "event:<id>", etc. */
};

/**
 * @brief Number of mutations recorded since Init (not cleared by Reset).
 */
size_t GSSK_GetMutationCount(GSSK_Instance *inst);

/**
 * @brief Read-only pointer to mutation record at idx.
 * @return NULL if idx >= GSSK_GetMutationCount().
 */
const GSSK_MutationRecord *GSSK_GetMutationRecord(GSSK_Instance *inst,
                                                   size_t idx);

/**
 * @brief Set the cause string for the NEXT mutation appended.
 *        Automatically cleared after one mutation is appended.
 *        Pass NULL or "" to revert to default ("user").
 *
 * Common values: "user", "calibration", "event:<edge_id>", "replay".
 */
void GSSK_SetMutationCause(GSSK_Instance *inst, const char *cause);

/**
 * @brief Remove all entries from the mutation log.
 *        Does not free the backing array (capacity retained for reuse).
 */
void GSSK_ClearMutationLog(GSSK_Instance *inst);

/**
 * @brief Serialize the mutation log as a standalone JSON array.
 *        Caller must free with GSSK_FreeString().
 * @return GSSK_SUCCESS or GSSK_ERR_MALLOC_FAILED.
 */
GSSK_Status GSSK_ExportMutationLog(GSSK_Instance *inst, char **out_json);

/**
 * @brief Replay a simulation from initial_json, applying logged mutations.
 *
 * Initialises a fresh instance from initial_json (topology only, no snapshot),
 * then steps forward to target_t applying each mutation at its recorded t.
 *
 * @param initial_json    JSON model string (topology, config, no snapshot block).
 * @param mutations_json  JSON array of mutation objects (from GSSK_ExportMutationLog),
 *                        or NULL / empty string for no mutations.
 * @param target_t        Stop time (inclusive). Use GSSK_GetTEnd(orig) for full replay.
 * @param out_inst        On return: new instance at state target_t.
 *                        Caller must free with GSSK_Free().
 * @return GSSK_SUCCESS, GSSK_ERR_DIVERGENCE, or GSSK_ERR_MALLOC_FAILED.
 */
GSSK_Status GSSK_Replay(const char *initial_json,
                         const char *mutations_json,
                         double target_t,
                         GSSK_Instance **out_inst);

#ifdef __cplusplus
}
#endif

#endif /* GSSK_H */
