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
 * @return GSSK_Instance* Pointer to initialized instance, or NULL on failure.
 */
GSSK_Instance *GSSK_Init(const char *json_data);

/**
 * @brief Perform one simulation step.
 *
 * @param inst Pointer to the GSSK instance.
 * @param dt Time step to advance the simulation.
 * @return GSSK_Status Current status of the simulation.
 */
GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt);

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
 * @brief Get the dimension of the state vector.
 *
 * @param inst Pointer to the GSSK instance.
 * @return size_t Number of storage nodes.
 */
size_t GSSK_GetStateSize(GSSK_Instance *inst);

/**
 * @brief Free all memory associated with an instance.
 *
 * @param inst Pointer to the GSSK instance.
 */
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
 * @brief Free all memory associated with an instance.
 *
 * @param inst Pointer to the GSSK instance.
 */
void GSSK_Free(GSSK_Instance *inst);

#ifdef __cplusplus
}
#endif

#endif // GSSK_H
