#include "gssk.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Internal Node types
typedef enum {
  NODE_STORAGE,
  NODE_SOURCE,
  NODE_SINK,
  NODE_CONSTANT
} GSSK_NodeType;

// Internal Node structure for fast lookup
typedef struct {
  char id[64];
  GSSK_NodeType type;
  double initial_value;
} GSSK_NodeInternal;

// Internal Edge structure for the solver
typedef struct {
  int origin_idx;
  int target_idx;
  int control_idx;
  GSSK_LogicType logic;
  double k;
  double threshold;
} GSSK_EdgeInternal;

// Internal Instance structure
struct GSSK_Instance {
  double *state;
  double *dQ;

  // RK4 Scratchpads
  double *k2;
  double *k3;
  double *k4;
  double *tmp_state;

  size_t node_count;
  GSSK_NodeInternal *nodes;

  GSSK_EdgeInternal *edges;
  size_t edge_count;

  struct {
    double t_start;
    double t_end;
    double dt;
    GSSK_Method method;
  } config;
};

static GSSK_NodeType parse_node_type(const char *type_str) {
  if (strcmp(type_str, "storage") == 0)
    return NODE_STORAGE;
  if (strcmp(type_str, "source") == 0)
    return NODE_SOURCE;
  if (strcmp(type_str, "sink") == 0)
    return NODE_SINK;
  if (strcmp(type_str, "constant") == 0)
    return NODE_CONSTANT;
  return NODE_STORAGE;
}

// Helper to find node index by ID string
static int find_node_idx(GSSK_Instance *inst, const char *id) {
  if (!id)
    return -1;
  for (size_t i = 0; i < inst->node_count; i++) {
    if (strcmp(inst->nodes[i].id, id) == 0)
      return (int)i;
  }
  return -1;
}

static GSSK_LogicType parse_logic_type(const char *type_str) {
  if (strcmp(type_str, "constant") == 0)
    return GSSK_LOGIC_CONSTANT;
  if (strcmp(type_str, "linear") == 0)
    return GSSK_LOGIC_LINEAR;
  if (strcmp(type_str, "interaction") == 0)
    return GSSK_LOGIC_INTERACTION;
  if (strcmp(type_str, "limit") == 0)
    return GSSK_LOGIC_LIMIT;
  if (strcmp(type_str, "threshold") == 0)
    return GSSK_LOGIC_THRESHOLD;
  return -1;
}

GSSK_Instance *GSSK_Init(const char *json_data) {
  if (!json_data)
    return NULL;

  cJSON *root = cJSON_Parse(json_data);
  if (!root) {
    fprintf(stderr, "JSON Parse Error: %s\n", cJSON_GetErrorPtr());
    return NULL;
  }

  GSSK_Instance *inst = calloc(1, sizeof(GSSK_Instance));
  if (!inst) {
    cJSON_Delete(root);
    return NULL;
  }

  // 1. Parse Nodes
  cJSON *nodes_arr = cJSON_GetObjectItem(root, "nodes");
  if (!cJSON_IsArray(nodes_arr)) {
    fprintf(stderr, "Schema Error: 'nodes' must be an array.\n");
    goto error;
  }

  inst->node_count = (size_t)cJSON_GetArraySize(nodes_arr);
  inst->nodes = calloc(inst->node_count, sizeof(GSSK_NodeInternal));
  inst->state = calloc(inst->node_count, sizeof(double));
  inst->dQ = calloc(inst->node_count, sizeof(double));

  for (int i = 0; i < (int)inst->node_count; i++) {
    cJSON *node = cJSON_GetArrayItem(nodes_arr, i);
    cJSON *id = cJSON_GetObjectItem(node, "id");
    cJSON *type = cJSON_GetObjectItem(node, "type");
    cJSON *val = cJSON_GetObjectItem(node, "value");

    if (!cJSON_IsString(id) || !cJSON_IsString(type) || !cJSON_IsNumber(val)) {
      fprintf(stderr,
              "Schema Error: Node at index %d is missing required fields (id, "
              "type, value).\n",
              i);
      goto error;
    }

    // Check for duplicate IDs
    // Note: find_node_idx will search the entire inst->nodes array.
    // For the current node 'i', its ID is not yet stored in inst->nodes[i].id.
    // However, if a previous node 'j < i' had the same ID, it would be found.
    // This check is sufficient for detecting duplicates among already processed
    // nodes. If the current node's ID is a duplicate of a node *after* it in
    // the JSON, that will be caught when the later node is processed.
    for (int j = 0; j < i; ++j) {
      if (strcmp(inst->nodes[j].id, id->valuestring) == 0) {
        fprintf(stderr,
                "Schema Error: Duplicate node ID detected: '%s' (at index %d, "
                "first seen at index %d).\n",
                id->valuestring, i, j);
        goto error;
      }
    }

    strncpy(inst->nodes[i].id, id->valuestring, 63);
    inst->nodes[i].id[63] = '\0'; // Ensure null-termination
    inst->nodes[i].type = parse_node_type(type->valuestring);
    inst->nodes[i].initial_value = val->valuedouble;
    inst->state[i] = val->valuedouble;
  }

  // 2. Parse Edges
  cJSON *edges_arr = cJSON_GetObjectItem(root, "edges");
  if (cJSON_IsArray(edges_arr)) {
    inst->edge_count = (size_t)cJSON_GetArraySize(edges_arr);
    inst->edges = calloc(inst->edge_count, sizeof(GSSK_EdgeInternal));

    for (int i = 0; i < (int)inst->edge_count; i++) {
      cJSON *edge = cJSON_GetArrayItem(edges_arr, i);
      cJSON *origin = cJSON_GetObjectItem(edge, "origin");
      cJSON *target = cJSON_GetObjectItem(edge, "target");
      cJSON *logic_str = cJSON_GetObjectItem(edge, "logic");
      cJSON *params = cJSON_GetObjectItem(edge, "params");

      if (!cJSON_IsString(origin) || !cJSON_IsString(target) ||
          !cJSON_IsString(logic_str) || !cJSON_IsObject(params)) {
        fprintf(stderr,
                "Schema Error: Edge at index %d is missing required fields "
                "(origin, target, logic, params).\n",
                i);
        goto error;
      }

      inst->edges[i].origin_idx = find_node_idx(inst, origin->valuestring);
      inst->edges[i].target_idx = find_node_idx(inst, target->valuestring);

      if (inst->edges[i].origin_idx == -1) {
        fprintf(stderr,
                "Linkage Error: Edge %d references non-existent origin node "
                "'%s'.\n",
                i, origin->valuestring);
        goto error;
      }
      if (inst->edges[i].target_idx == -1) {
        fprintf(stderr,
                "Linkage Error: Edge %d references non-existent target node "
                "'%s'.\n",
                i, target->valuestring);
        goto error;
      }

      int l_type = parse_logic_type(logic_str->valuestring);
      if (l_type == -1) {
        fprintf(stderr, "Logic Error: Unknown logic type '%s' in edge %d.\n",
                logic_str->valuestring, i);
        goto error;
      }
      inst->edges[i].logic = (GSSK_LogicType)l_type;

      cJSON *k = cJSON_GetObjectItem(params, "k");
      if (!cJSON_IsNumber(k)) {
        fprintf(stderr,
                "Schema Error: Edge %d is missing required parameter 'k' or "
                "it's not a number.\n",
                i);
        goto error;
      }
      inst->edges[i].k = k->valuedouble;

      // Optional parameters
      cJSON *control = cJSON_GetObjectItem(params, "control_node");
      if (cJSON_IsString(control)) {
        inst->edges[i].control_idx = find_node_idx(inst, control->valuestring);
        if (inst->edges[i].control_idx == -1) {
          fprintf(stderr,
                  "Linkage Error: Edge %d references non-existent control node "
                  "'%s'.\n",
                  i, control->valuestring);
          goto error;
        }
      } else {
        inst->edges[i].control_idx = -1;
      }

      cJSON *threshold = cJSON_GetObjectItem(params, "threshold");
      if (cJSON_IsNumber(threshold))
        inst->edges[i].threshold = threshold->valuedouble;
      else
        inst->edges[i].threshold = 0.0; // Default threshold

      // Logic-specific validation
      if (inst->edges[i].logic == GSSK_LOGIC_INTERACTION ||
          inst->edges[i].logic == GSSK_LOGIC_LIMIT) {
        if (inst->edges[i].control_idx == -1) {
          fprintf(stderr,
                  "Logic Error: Edge %d (%s) requires 'control_node' in "
                  "params.\n",
                  i, logic_str->valuestring);
          goto error;
        }
      }
    }
  }

  // 3. Parse Config
  cJSON *config = cJSON_GetObjectItem(root, "config");
  if (cJSON_IsObject(config)) {
    cJSON *ts = cJSON_GetObjectItem(config, "t_start");
    cJSON *te = cJSON_GetObjectItem(config, "t_end");
    cJSON *dt = cJSON_GetObjectItem(config, "dt");

    if (cJSON_IsNumber(ts))
      inst->config.t_start = ts->valuedouble;
    else
      inst->config.t_start = 0.0;

    if (cJSON_IsNumber(te))
      inst->config.t_end = te->valuedouble;
    else
      inst->config.t_end = 100.0;

    if (cJSON_IsNumber(dt))
      inst->config.dt = dt->valuedouble;
    else
      inst->config.dt = 0.1;

    // Validation
    if (inst->config.t_end <= inst->config.t_start) {
      fprintf(stderr,
              "Config Error: t_end (%.2f) must be greater than t_start "
              "(%.2f).\n",
              inst->config.t_end, inst->config.t_start);
      goto error;
    }
    if (inst->config.dt <= 0.0) {
      fprintf(stderr, "Config Error: dt (%.4f) must be positive.\n",
              inst->config.dt);
      goto error;
    }

    cJSON *method = cJSON_GetObjectItem(config, "method");
    if (cJSON_IsString(method)) {
      if (strcmp(method->valuestring, "rk4") == 0)
        inst->config.method = GSSK_METHOD_RK4;
      else
        inst->config.method = GSSK_METHOD_EULER;
    } else {
      inst->config.method = GSSK_METHOD_EULER;
    }
  } else {
    // Defaults if config object is missing
    inst->config.t_start = 0.0;
    inst->config.t_end = 100.0;
    inst->config.dt = 0.1;
    inst->config.method = GSSK_METHOD_EULER;
  }

  // 4. Allocate RK4 Scratchpads if needed
  if (inst->config.method == GSSK_METHOD_RK4) {
    inst->k2 = calloc(inst->node_count, sizeof(double));
    inst->k3 = calloc(inst->node_count, sizeof(double));
    inst->k4 = calloc(inst->node_count, sizeof(double));
    inst->tmp_state = calloc(inst->node_count, sizeof(double));
    if (!inst->k2 || !inst->k3 || !inst->k4 || !inst->tmp_state)
      goto error;
  }

  cJSON_Delete(root);
  return inst;

error:
  GSSK_Free(inst);
  cJSON_Delete(root);
  return NULL;
}

static void compute_derivatives(GSSK_Instance *inst, const double *state,
                                double *deriv) {
  memset(deriv, 0, inst->node_count * sizeof(double));

  for (size_t i = 0; i < inst->edge_count; i++) {
    GSSK_EdgeInternal *e = &inst->edges[i];
    double flow = 0.0;
    double Q_orig = state[e->origin_idx];

    switch (e->logic) {
    case GSSK_LOGIC_CONSTANT:
      flow = e->k;
      break;
    case GSSK_LOGIC_LINEAR:
      flow = e->k * Q_orig;
      break;
    case GSSK_LOGIC_INTERACTION:
      if (e->control_idx != -1) {
        flow = e->k * Q_orig * state[e->control_idx];
      }
      break;
    case GSSK_LOGIC_LIMIT:
      if (e->control_idx != -1) {
        double C = state[e->control_idx];
        if (C > 1e-9) { // Avoid division by zero
          flow = (e->k * Q_orig) / (1.0 + (Q_orig / C));
        }
      }
      break;
    case GSSK_LOGIC_THRESHOLD:
      flow = (Q_orig > e->threshold) ? e->k : 0.0;
      break;
    }

    // Apply flow to derivatives
    deriv[e->origin_idx] -= flow;
    deriv[e->target_idx] += flow;
  }

  // Boundary Conditions: Non-storage nodes have dQ/dt = 0
  for (size_t i = 0; i < inst->node_count; i++) {
    if (inst->nodes[i].type == NODE_SOURCE ||
        inst->nodes[i].type == NODE_CONSTANT) {
      deriv[i] = 0.0;
    }
  }
}

#include <math.h>

GSSK_Status GSSK_Step(GSSK_Instance *inst, double dt) {
  if (!inst)
    return GSSK_ERR_UNKNOWN;

  size_t n = inst->node_count;

  if (inst->config.method == GSSK_METHOD_EULER) {
    compute_derivatives(inst, inst->state, inst->dQ);
    for (size_t i = 0; i < n; i++) {
      inst->state[i] += inst->dQ[i] * dt;
    }
  } else if (inst->config.method == GSSK_METHOD_RK4) {
    // k1 = f(y)
    compute_derivatives(inst, inst->state, inst->dQ);

    // k2 = f(y + h/2 * k1)
    for (size_t i = 0; i < n; i++)
      inst->tmp_state[i] = inst->state[i] + 0.5 * dt * inst->dQ[i];
    compute_derivatives(inst, inst->tmp_state, inst->k2);

    // k3 = f(y + h/2 * k2)
    for (size_t i = 0; i < n; i++)
      inst->tmp_state[i] = inst->state[i] + 0.5 * dt * inst->k2[i];
    compute_derivatives(inst, inst->tmp_state, inst->k3);

    // k4 = f(y + h * k3)
    for (size_t i = 0; i < n; i++)
      inst->tmp_state[i] = inst->state[i] + dt * inst->k3[i];
    compute_derivatives(inst, inst->tmp_state, inst->k4);

    // y = y + h/6 * (k1 + 2k2 + 2k3 + k4)
    for (size_t i = 0; i < n; i++) {
      inst->state[i] += (dt / 6.0) * (inst->dQ[i] + 2.0 * inst->k2[i] +
                                      2.0 * inst->k3[i] + inst->k4[i]);
    }
  }

  // Post-step processing: Numerical stability and constraints
  for (size_t i = 0; i < n; i++) {
    // 1. Check for divergence
    if (isnan(inst->state[i]) || isinf(inst->state[i])) {
      return GSSK_ERR_DIVERGENCE;
    }

    // 2. Clamping: Quantity cannot drop below 0.0 (Physical conservation)
    if (inst->state[i] < 0.0) {
      inst->state[i] = 0.0;
    }
  }

  return GSSK_SUCCESS;
}

const double *GSSK_GetState(GSSK_Instance *inst) {
  return inst ? inst->state : NULL;
}

size_t GSSK_GetStateSize(GSSK_Instance *inst) {
  return inst ? inst->node_count : 0;
}

const char *GSSK_GetNodeID(GSSK_Instance *inst, size_t index) {
  if (!inst || index >= inst->node_count)
    return NULL;
  return inst->nodes[index].id;
}

double GSSK_GetTStart(GSSK_Instance *inst) {
  return inst ? inst->config.t_start : 0.0;
}

double GSSK_GetTEnd(GSSK_Instance *inst) {
  return inst ? inst->config.t_end : 0.0;
}

double GSSK_GetDt(GSSK_Instance *inst) { return inst ? inst->config.dt : 0.0; }

void GSSK_Free(GSSK_Instance *inst) {
  if (inst) {
    free(inst->state);
    free(inst->dQ);
    free(inst->k2);
    free(inst->k3);
    free(inst->k4);
    free(inst->tmp_state);
    free(inst->nodes);
    free(inst->edges);
    free(inst);
  }
}
