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

  if (from_ver != 2) {
    fprintf(stderr, "migrate: only --from 2 is supported (migrates v2 → v3)\n");
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
    fprintf(stderr, "migrate: wrote v3 model to %s\n", output_path);
  } else {
    puts(out_json);
  }

  free(out_json);
  return EXIT_SUCCESS;
}

static int cmd_version(void) {
  printf("gssk %s (schema v3)\n", GSSK_GetVersionString());
  return EXIT_SUCCESS;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "Usage:\n"
      "  gssk <model.json> [output.csv]               Run simulation\n"
      "  gssk run <model.json> [output.csv]            Run simulation\n"
      "  gssk migrate --from 2 <input.json> [out.json] Upgrade v2 → v3 schema\n"
      "  gssk version                                  Print kernel version\n");
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

  /* Default: treat argv[1] as a model file path (backwards-compatible) */
  return cmd_run(argc - 1, argv + 1);
}
