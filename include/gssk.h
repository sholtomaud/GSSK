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
  GSSK_METHOD_INCIPIENT
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

#define GSK_VERSION_MAJOR 3
#define GSK_VERSION_MINOR 0
#define GSK_VERSION_PATCH 0
#define GSK_VERSION_STRING "3.0.0"

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
 * May be called manually after GSSK_SetEdgeK() if k changes logic type eligibility.
 *
 * Sets internal flag: incipient_eligible = true iff all active edges are
 * constant, linear, or interaction logic types.
 */
GSSK_Status GSSK_ReclassifyNetwork(GSSK_Instance *inst);

/* =========================================================================
 * Config accessors
 * ========================================================================= */

double GSSK_GetTStart(GSSK_Instance *inst);
double GSSK_GetTEnd(GSSK_Instance *inst);
double GSSK_GetDt(GSSK_Instance *inst);

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
 * @brief Run parameter calibration against observed data.
 */
GSSK_Status GSSK_Calibrate(GSSK_Instance *inst, GSSK_NodeObservations *obs,
                           size_t obs_count, int iterations);

#ifdef __cplusplus
}
#endif

#endif /* GSSK_H */
