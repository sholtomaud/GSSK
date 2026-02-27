/**
 * @file gssk.h
 * @brief General Systems Simulation Kernel (GSSK) Core API
 *
 * This file defines the public interface for the GSSK numerical engine.
 * The kernel is designed for high-performance simulation of complex
 * systems based on General Systems Theory and Odum logic.
 */

#ifndef GSSK_H
#define GSSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Logic types for flow calculations.
 */
typedef enum {
  GSSK_LOGIC_CONSTANT,    /**< Fixed flow rate */
  GSSK_LOGIC_LINEAR,      /**< Proportion to source (k * Q) */
  GSSK_LOGIC_INTERACTION, /**< Multiplier flow (k * Q1 * Q2) */
  GSSK_LOGIC_LIMIT,       /**< Saturation logic (Michaelis-Menten) */
  GSSK_LOGIC_THRESHOLD    /**< Boolean switch logic */
} GSSK_LogicType;

/**
 * @brief Integration methods supported by the solver.
 */
typedef enum { GSSK_METHOD_EULER, GSSK_METHOD_RK4 } GSSK_Method;

/**
 * @brief Opaque handle to a GSSK instance.
 */
typedef struct GSSK_Instance GSSK_Instance;

/**
 * @brief Error codes returned by the kernel.
 */
typedef enum {
  GSSK_SUCCESS = 0,
  GSSK_ERR_INVALID_JSON,
  GSSK_ERR_MALLOC_FAILED,
  GSSK_ERR_SCHEMA_VIOLATION,
  GSSK_ERR_DIVERGENCE, /**< Numerical instability detected (NaN/Inf) */
  GSSK_ERR_UNKNOWN
} GSSK_Status;

/**
 * @brief Initialize a GSSK instance from a JSON configuration string.
 *
 * @param json_data String containing the model topology and config.
 * @param out_inst Pointer to a GSSK_Instance pointer that will be populated.
 * @return GSSK_Status Initialization status. If GSSK_ERR_SCHEMA_VIOLATION is
 *         returned, out_inst will still contain an instance pointer that
 *         can be used with GSSK_GetErrorDescription to see the specific error.
 *         The caller must always call GSSK_Free on the returned instance if it
 *         is not NULL.
 */
GSSK_Status GSSK_Init(const char *json_data, GSSK_Instance **out_inst);

/**
 * @brief Get a detailed error description from the last operation.
 *
 * @param inst Pointer to the GSSK instance.
 * @return const char* Error message string.
 */
const char *GSSK_GetErrorDescription(GSSK_Instance *inst);

/**
 * @brief Perform one simulation step.
 *
 * @param inst Pointer to the GSSK instance.
 * @param dt Time step to advance the simulation.
 * @return GSSK_Status Current status of the simulation.
 */
GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt);

/**
 * @brief Reset the simulation instance to its initial state.
 *
 * @param inst Pointer to the GSSK instance.
 */
void GSSK_Reset(GSSK_Instance *inst);

/**
 * @brief Access the internal state vector.
 *
 * @param inst Pointer to the GSSK instance.
 * @return const double* Pointer to the Q vector (read-only).
 */
const double *GSSK_GetState(GSSK_Instance *inst);

/**
 * @brief Get the ID of a node at a given index.
 *
 * @param inst Pointer to the GSSK instance.
 * @param index Index of the node.
 * @return const char* ID of the node, or NULL if index is out of bounds.
 */
const char *GSSK_GetNodeID(GSSK_Instance *inst, size_t index);

/**
 * @brief Find the index of a node by its ID.
 *
 * @param inst Pointer to the GSSK instance.
 * @param id ID of the node.
 * @return int Index of the node, or -1 if not found.
 */
int GSSK_FindNodeIdx(GSSK_Instance *inst, const char *id);

/**
 * @brief Get the dimension of the state vector.
 *
 * @param inst Pointer to the GSSK instance.
 * @return size_t Number of storage nodes.
 */
size_t GSSK_GetStateSize(GSSK_Instance *inst);

/**
 * @brief Get the simulation start time.
 *
 * @param inst Pointer to the GSSK instance.
 * @return double Start time.
 */
double GSSK_GetTStart(GSSK_Instance *inst);

/**
 * @brief Get the simulation end time.
 *
 * @param inst Pointer to the GSSK instance.
 * @return double End time.
 */
double GSSK_GetTEnd(GSSK_Instance *inst);

/**
 * @brief Get the simulation time step.
 *
 * @param inst Pointer to the GSSK instance.
 * @return double Time step.
 */
double GSSK_GetDt(GSSK_Instance *inst);

/**
 * @brief Get the number of edges in the model.
 */
size_t GSSK_GetEdgeCount(GSSK_Instance *inst);

/**
 * @brief Get the coefficient 'k' of an edge.
 */
double GSSK_GetEdgeK(GSSK_Instance *inst, size_t index);

/**
 * @brief Set the coefficient 'k' of an edge.
 */
void GSSK_SetEdgeK(GSSK_Instance *inst, size_t index, double k);

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
  double *min_envelope;  /**< Size: node_count * step_count */
  double *max_envelope;  /**< Size: node_count * step_count */
  double *mean_envelope; /**< Size: node_count * step_count */
  size_t node_count;
  size_t step_count;
} GSSK_EnsembleResult;

/**
 * @brief Run ensemble forecasting.
 *
 * @param inst Base model instance.
 * @param runs Number of simulation runs.
 * @param perturbation Fractional perturbation (e.g., 0.1 for +/- 10%).
 * @return GSSK_EnsembleResult* Result containing the envelopes. Caller must free.
 */
GSSK_EnsembleResult *GSSK_EnsembleForecast(GSSK_Instance *inst, size_t runs,
                                           double perturbation);

/**
 * @brief Free ensemble results.
 */
void GSSK_FreeEnsembleResult(GSSK_EnsembleResult *res);

/**
 * @brief Run parameter calibration.
 *
 * @param inst Base model instance.
 * @param obs Array of node observations.
 * @param obs_count Number of nodes with observations.
 * @param iterations Number of optimizer iterations.
 * @return GSSK_Status Optimization status.
 */
GSSK_Status GSSK_Calibrate(GSSK_Instance *inst, GSSK_NodeObservations *obs,
                           size_t obs_count, int iterations);

/**
 * @brief Free all memory associated with an instance.
 *
 * @param inst Pointer to the GSSK instance.
 */
void GSSK_Free(GSSK_Instance *inst);

#ifdef __cplusplus
}
#endif

#endif // GSSK_H
