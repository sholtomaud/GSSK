#include "gssk.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror("Error opening file"); return NULL; }
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *data = malloc(length + 1);
  if (!data) { fclose(f); return NULL; }
  size_t n = fread(data, 1, length, f);
  fclose(f);
  if (n != (size_t)length) { free(data); return NULL; }
  data[length] = '\0';
  return data;
}

/* =========================================================================
 * Subcommands
 * ========================================================================= */

static int cmd_run(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "Usage: gssk [run] <model.json> [output.csv]\n");
    return EXIT_FAILURE;
  }

  char *data = read_file(argv[0]);
  if (!data) return EXIT_FAILURE;

  GSSK_Instance *kernel = NULL;
  GSSK_Status status = GSSK_Init(data, &kernel);
  free(data);

  if (status != GSSK_SUCCESS) {
    fprintf(stderr, "Failed to initialize GSSK kernel: %s\n",
            kernel ? GSSK_GetErrorDescription(kernel) : "Unknown Error");
    if (kernel) GSSK_Free(kernel);
    return EXIT_FAILURE;
  }

  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      perror("Error opening output file");
      GSSK_Free(kernel);
      return EXIT_FAILURE;
    }
  }

  fprintf(out, "time");
  size_t node_count = GSSK_GetStateSize(kernel);
  for (size_t i = 0; i < node_count; i++) {
    const char *id = GSSK_GetNodeID(kernel, i);
    fprintf(out, ",%s", id ? id : "unknown");
  }
  fprintf(out, "\n");

  double t     = GSSK_GetTStart(kernel);
  double t_end = GSSK_GetTEnd(kernel);
  double dt    = GSSK_GetDt(kernel);

  while (t <= t_end + (dt * 0.01)) {
    const double *state = GSSK_GetState(kernel);
    fprintf(out, "%.4f", t);
    for (size_t i = 0; i < node_count; i++)
      fprintf(out, ",%.6f", state[i]);
    fprintf(out, "\n");

    if (GSSK_Step(kernel, dt) != GSSK_SUCCESS) {
      fprintf(stderr, "Numerical divergence at t=%.4f\n", t);
      break;
    }
    t += dt;
  }

  if (out != stdout) fclose(out);
  GSSK_Free(kernel);
  return EXIT_SUCCESS;
}

/*
 * cmd_migrate: upgrade a v2 JSON model to v3 by injecting a metadata block.
 *
 * Usage: gssk migrate --from 2 <input.json> [output.json]
 *
 * If the model already has a metadata block its schema_version is bumped to 3.
 * If no metadata block exists, one is created with auto-generated timestamps
 * and the current kernel version.
 */
static int cmd_migrate(int argc, char **argv) {
  int from_ver = 2;
  const char *input_path  = NULL;
  const char *output_path = NULL;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
      from_ver = atoi(argv[++i]);
    } else if (!input_path) {
      input_path = argv[i];
    } else if (!output_path) {
      output_path = argv[i];
    }
  }

  if (!input_path) {
    fprintf(stderr, "Usage: gssk migrate --from <version> <input.json> [output.json]\n");
    return EXIT_FAILURE;
  }

  if (from_ver != 2 && from_ver != 3) {
    fprintf(stderr, "migrate: supported versions: --from 2 (v2→v3), --from 3 (v3→v4)\n");
    return EXIT_FAILURE;
  }

  char *data = read_file(input_path);
  if (!data) return EXIT_FAILURE;

  cJSON *root = cJSON_Parse(data);
  free(data);
  if (!root) {
    fprintf(stderr, "migrate: failed to parse JSON: %s\n", input_path);
    return EXIT_FAILURE;
  }

  /* Build or patch metadata block */
  cJSON *meta = cJSON_GetObjectItem(root, "metadata");
  if (!meta) {
    meta = cJSON_CreateObject();
    cJSON_AddItemToObjectCS(root, "metadata", meta);
  }

  /* Set/overwrite schema_version to 3 */
  cJSON *sv = cJSON_GetObjectItem(meta, "schema_version");
  if (sv) {
    cJSON_SetNumberValue(sv, 3);
  } else {
    cJSON_AddNumberToObject(meta, "schema_version", 3);
  }

  /* Set kernel_version if missing */
  if (!cJSON_GetObjectItem(meta, "kernel_version")) {
    cJSON_AddStringToObject(meta, "kernel_version", GSSK_GetVersionString());
  }

  /* Set created_at if missing */
  if (!cJSON_GetObjectItem(meta, "created_at")) {
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    cJSON_AddStringToObject(meta, "created_at", ts);
  }

  /* ── v3 → v4: promote interaction/limit edges to processing nodes ─────── */
  if (from_ver == 3) {
    /* Update schema_version to 4 */
    cJSON *sv3 = cJSON_GetObjectItem(meta, "schema_version");
    if (sv3) cJSON_SetNumberValue(sv3, 4);
    else cJSON_AddNumberToObject(meta, "schema_version", 4);

    cJSON *nodes_arr = cJSON_GetObjectItem(root, "nodes");
    cJSON *edges_arr = cJSON_GetObjectItem(root, "edges");
    if (!cJSON_IsArray(nodes_arr) || !cJSON_IsArray(edges_arr)) {
      fprintf(stderr, "migrate: model missing nodes/edges arrays\n");
      cJSON_Delete(root);
      return EXIT_FAILURE;
    }

    /* Collect edges that need promotion */
    cJSON *edge = NULL;
    cJSON *edges_to_remove = cJSON_CreateArray();
    cJSON *new_edges = cJSON_CreateArray();

    cJSON_ArrayForEach(edge, edges_arr) {
      cJSON *logic = cJSON_GetObjectItem(edge, "logic");
      if (!cJSON_IsString(logic)) continue;
      const char *lstr = logic->valuestring;

      int is_interaction = (strcmp(lstr, "interaction") == 0);
      int is_limit       = (strcmp(lstr, "limit")       == 0);
      if (!is_interaction && !is_limit) continue;

      cJSON *eid    = cJSON_GetObjectItem(edge, "id");
      cJSON *origin = cJSON_GetObjectItem(edge, "origin");
      cJSON *target = cJSON_GetObjectItem(edge, "target");
      cJSON *params = cJSON_GetObjectItem(edge, "params");
      cJSON *carrier = cJSON_GetObjectItem(edge, "carrier");
      if (!cJSON_IsString(origin) || !cJSON_IsString(target)) continue;

      const char *edge_id = cJSON_IsString(eid) ? eid->valuestring : "gate";
      double k = 1.0;
      cJSON *pk = params ? cJSON_GetObjectItem(params, "k") : NULL;
      if (cJSON_IsNumber(pk)) k = pk->valuedouble;

      if (is_interaction) {
        /* interaction edge → interaction node + 3 edges */
        cJSON *ctrl_item = params ? cJSON_GetObjectItem(params, "control_node") : NULL;
        const char *ctrl = cJSON_IsString(ctrl_item) ? ctrl_item->valuestring
                                                      : target->valuestring;

        /* New node: {id: edge_id, type: "interaction", value: 0, params: {k}} */
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "id", edge_id);
        cJSON_AddStringToObject(n, "type", "interaction");
        cJSON_AddNumberToObject(n, "value", 0.0);
        cJSON *np = cJSON_CreateObject();
        cJSON_AddNumberToObject(np, "k", k);
        cJSON_AddItemToObject(n, "params", np);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(n, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(nodes_arr, n);

        /* feed: origin → gate */
        char feed_id[128]; snprintf(feed_id, sizeof(feed_id), "%.60s__feed", edge_id);
        cJSON *e1 = cJSON_CreateObject();
        cJSON_AddStringToObject(e1, "id",     feed_id);
        cJSON_AddStringToObject(e1, "origin", origin->valuestring);
        cJSON_AddStringToObject(e1, "target", edge_id);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(e1, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(new_edges, e1);

        /* ctrl: control_node → gate (skip if same as feed) */
        if (strcmp(ctrl, origin->valuestring) != 0) {
          char ctrl_id[128]; snprintf(ctrl_id, sizeof(ctrl_id), "%.60s__ctrl", edge_id);
          cJSON *e2 = cJSON_CreateObject();
          cJSON_AddStringToObject(e2, "id",     ctrl_id);
          cJSON_AddStringToObject(e2, "origin", ctrl);
          cJSON_AddStringToObject(e2, "target", edge_id);
          if (cJSON_IsString(carrier)) cJSON_AddStringToObject(e2, "carrier", carrier->valuestring);
          cJSON_AddItemToArray(new_edges, e2);
        }

        /* out: gate → original target */
        char out_id[128]; snprintf(out_id, sizeof(out_id), "%.60s__out", edge_id);
        cJSON *e3 = cJSON_CreateObject();
        cJSON_AddStringToObject(e3, "id",     out_id);
        cJSON_AddStringToObject(e3, "origin", edge_id);
        cJSON_AddStringToObject(e3, "target", target->valuestring);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(e3, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(new_edges, e3);

      } else { /* is_limit */
        /* limit edge → loop_limited node + 2 edges */
        double C = 1.0;
        cJSON *pt = params ? cJSON_GetObjectItem(params, "threshold") : NULL;
        cJSON *pc = params ? cJSON_GetObjectItem(params, "C")         : NULL;
        if (cJSON_IsNumber(pt)) C = pt->valuedouble;
        else if (cJSON_IsNumber(pc)) C = pc->valuedouble;

        /* New node */
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "id", edge_id);
        cJSON_AddStringToObject(n, "type", "loop_limited");
        cJSON_AddNumberToObject(n, "value", 0.0);
        cJSON *np = cJSON_CreateObject();
        cJSON_AddNumberToObject(np, "k", k);
        cJSON_AddNumberToObject(np, "C", C);
        cJSON_AddItemToObject(n, "params", np);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(n, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(nodes_arr, n);

        /* feed: origin → node */
        char feed_id[128]; snprintf(feed_id, sizeof(feed_id), "%.60s__feed", edge_id);
        cJSON *e1 = cJSON_CreateObject();
        cJSON_AddStringToObject(e1, "id",     feed_id);
        cJSON_AddStringToObject(e1, "origin", origin->valuestring);
        cJSON_AddStringToObject(e1, "target", edge_id);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(e1, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(new_edges, e1);

        /* out: node → target */
        char out_id[128]; snprintf(out_id, sizeof(out_id), "%.60s__out", edge_id);
        cJSON *e2 = cJSON_CreateObject();
        cJSON_AddStringToObject(e2, "id",     out_id);
        cJSON_AddStringToObject(e2, "origin", edge_id);
        cJSON_AddStringToObject(e2, "target", target->valuestring);
        if (cJSON_IsString(carrier)) cJSON_AddStringToObject(e2, "carrier", carrier->valuestring);
        cJSON_AddItemToArray(new_edges, e2);
      }

      /* Mark original edge for removal */
      cJSON_AddItemToArray(edges_to_remove, cJSON_CreateString(
          cJSON_IsString(eid) ? eid->valuestring : ""));
    }

    /* Remove promoted edges */
    int rm_count = cJSON_GetArraySize(edges_to_remove);
    for (int ri = rm_count - 1; ri >= 0; ri--) {
      cJSON *rid = cJSON_GetArrayItem(edges_to_remove, ri);
      if (!cJSON_IsString(rid) || rid->valuestring[0] == '\0') continue;
      int idx = 0;
      cJSON *e2 = NULL;
      cJSON_ArrayForEach(e2, edges_arr) {
        cJSON *eid2 = cJSON_GetObjectItem(e2, "id");
        if (cJSON_IsString(eid2) && strcmp(eid2->valuestring, rid->valuestring) == 0) {
          cJSON_DeleteItemFromArray(edges_arr, idx);
          break;
        }
        idx++;
      }
    }
    cJSON_Delete(edges_to_remove);

    /* Append new edges */
    cJSON *ne = NULL;
    cJSON_ArrayForEach(ne, new_edges) {
      cJSON_AddItemToArray(edges_arr, cJSON_Duplicate(ne, 1));
    }
    cJSON_Delete(new_edges);

    fprintf(stderr, "migrate: upgraded to schema v4 (interaction/limit edges → nodes)\n");
  }

  char *out_json = cJSON_Print(root);
  cJSON_Delete(root);

  if (!out_json) {
    fprintf(stderr, "migrate: failed to serialise output JSON\n");
    return EXIT_FAILURE;
  }

  if (output_path) {
    FILE *f = fopen(output_path, "w");
    if (!f) {
      perror("migrate: failed to open output file");
      free(out_json);
      return EXIT_FAILURE;
    }
    fputs(out_json, f);
    fclose(f);
    fprintf(stderr, "migrate: wrote %s model to %s\n",
            from_ver == 3 ? "v4" : "v3", output_path);
  } else {
    puts(out_json);
  }

  free(out_json);
  return EXIT_SUCCESS;
}

static int cmd_version(void) {
  printf("gssk %s (schema v4)\n", GSSK_GetVersionString());
  return EXIT_SUCCESS;
}

/*
 * cmd_diff: compare two snapshot files node-by-node.
 *
 * Usage: gssk diff <snap_a.json> <snap_b.json>
 *
 * Prints a table showing Q[node] in A, B, and the delta for every node that
 * appears in both snapshots.  Nodes present in only one snapshot are flagged.
 */
static int cmd_diff(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: gssk diff <snapshot_a.json> <snapshot_b.json>\n");
    return EXIT_FAILURE;
  }

  char *data_a = read_file(argv[0]);
  char *data_b = read_file(argv[1]);
  if (!data_a || !data_b) {
    free(data_a); free(data_b);
    return EXIT_FAILURE;
  }

  GSSK_Instance *ka = NULL, *kb = NULL;
  GSSK_Status sa = GSSK_Init(data_a, &ka);
  GSSK_Status sb = GSSK_Init(data_b, &kb);
  free(data_a); free(data_b);

  if (sa != GSSK_SUCCESS) {
    fprintf(stderr, "diff: failed to load snapshot A: %s\n",
            ka ? GSSK_GetErrorDescription(ka) : "unknown");
    GSSK_Free(ka); GSSK_Free(kb);
    return EXIT_FAILURE;
  }
  if (sb != GSSK_SUCCESS) {
    fprintf(stderr, "diff: failed to load snapshot B: %s\n",
            kb ? GSSK_GetErrorDescription(kb) : "unknown");
    GSSK_Free(ka); GSSK_Free(kb);
    return EXIT_FAILURE;
  }

  printf("%-20s %14s %14s %14s\n", "node", "A", "B", "delta");
  printf("%-20s %14s %14s %14s\n", "----", "-", "-", "-----");

  const double *sa_state = GSSK_GetState(ka);
  const double *sb_state = GSSK_GetState(kb);
  size_t na = GSSK_GetStateSize(ka);
  size_t nb = GSSK_GetStateSize(kb);

  for (size_t i = 0; i < na; i++) {
    const char *id = GSSK_GetNodeID(ka, i);
    int jb = GSSK_FindNodeIdx(kb, id);
    if (jb < 0) {
      printf("%-20s %14.6f %14s %14s  [only in A]\n", id, sa_state[i], "-", "-");
    } else {
      double delta = sb_state[jb] - sa_state[i];
      printf("%-20s %14.6f %14.6f %14.6f\n", id, sa_state[i], sb_state[jb], delta);
    }
  }
  for (size_t j = 0; j < nb; j++) {
    const char *id = GSSK_GetNodeID(kb, j);
    if (GSSK_FindNodeIdx(ka, id) < 0)
      printf("%-20s %14s %14.6f %14s  [only in B]\n", id, "-", sb_state[j], "-");
  }

  printf("\ntime A=%.4f  time B=%.4f\n",
         GSSK_GetCurrentTime(ka), GSSK_GetCurrentTime(kb));

  GSSK_Free(ka); GSSK_Free(kb);
  return EXIT_SUCCESS;
}

/*
 * cmd_replay: replay a model with an optional mutation log, printing CSV.
 *
 * Usage: gssk replay <model.json> [mutations.json] [--until <t>]
 *
 * Initialises from model.json (topology only), applies mutations at their
 * recorded times, and emits state as CSV to stdout (or output.csv if given).
 */
static int cmd_replay(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr,
        "Usage: gssk replay <model.json> [mutations.json] [--until <t>] [output.csv]\n");
    return EXIT_FAILURE;
  }

  const char *model_path    = argv[0];
  const char *mut_path      = NULL;
  const char *output_path   = NULL;
  double      target_t      = -1.0; /* -1 means "use model t_end" */

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
      target_t = atof(argv[++i]);
    } else if (!mut_path && argv[i][0] != '-') {
      /* Heuristic: first non-flag arg after model is mutations file or output */
      if (i + 1 < argc && strcmp(argv[i+1], "--until") != 0 &&
          (strlen(argv[i]) > 5 &&
           strcmp(argv[i] + strlen(argv[i]) - 5, ".json") == 0)) {
        mut_path = argv[i];
      } else {
        output_path = argv[i];
      }
    } else if (!output_path && argv[i][0] != '-') {
      output_path = argv[i];
    }
  }

  char *model_data = read_file(model_path);
  if (!model_data) return EXIT_FAILURE;

  char *mut_data = NULL;
  if (mut_path) {
    mut_data = read_file(mut_path);
    if (!mut_data) { free(model_data); return EXIT_FAILURE; }
  }

  /* Determine target_t from model if not specified */
  if (target_t < 0.0) {
    GSSK_Instance *probe = NULL;
    GSSK_Init(model_data, &probe);
    if (probe) { target_t = GSSK_GetTEnd(probe); GSSK_Free(probe); }
  }

  GSSK_Instance *inst = NULL;
  GSSK_Status status = GSSK_Replay(model_data, mut_data, target_t, &inst);
  free(model_data);
  free(mut_data);

  if (!inst) return EXIT_FAILURE;

  if (status != GSSK_SUCCESS && status != GSSK_WARN_SOLVER_DIVERGENCE) {
    fprintf(stderr, "replay: %s\n", GSSK_GetErrorDescription(inst));
    GSSK_Free(inst);
    return EXIT_FAILURE;
  }

  /* Emit final state as a single-row CSV snapshot */
  FILE *out = stdout;
  if (output_path) {
    out = fopen(output_path, "w");
    if (!out) { perror("replay"); GSSK_Free(inst); return EXIT_FAILURE; }
  }

  size_t nc = GSSK_GetStateSize(inst);
  fprintf(out, "time");
  for (size_t i = 0; i < nc; i++) {
    const char *id = GSSK_GetNodeID(inst, i);
    fprintf(out, ",%s", id ? id : "unknown");
  }
  fprintf(out, "\n%.4f", GSSK_GetCurrentTime(inst));
  const double *st = GSSK_GetState(inst);
  for (size_t i = 0; i < nc; i++)
    fprintf(out, ",%.6f", st[i]);
  fprintf(out, "\n");

  if (out != stdout) fclose(out);
  GSSK_Free(inst);
  return EXIT_SUCCESS;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "Usage:\n"
      "  gssk <model.json> [output.csv]                     Run simulation\n"
      "  gssk run <model.json> [output.csv]                 Run simulation\n"
      "  gssk migrate --from 2 <input.json> [out.json]      Upgrade v2 → v3 schema\n"
      "  gssk migrate --from 3 <input.json> [out.json]      Upgrade v3 → v4 schema\n"
      "  gssk diff <snap_a.json> <snap_b.json>              Diff two snapshots\n"
      "  gssk replay <model.json> [muts.json] [--until <t>] Replay with mutations\n"
      "  gssk version                                       Print kernel version\n");
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "migrate") == 0) {
    return cmd_migrate(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
    return cmd_version();
  }
  if (strcmp(argv[1], "run") == 0) {
    return cmd_run(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "diff") == 0) {
    return cmd_diff(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "replay") == 0) {
    return cmd_replay(argc - 2, argv + 2);
  }

  /* Default: treat argv[1] as a model file path (backwards-compatible) */
  return cmd_run(argc - 1, argv + 1);
}
