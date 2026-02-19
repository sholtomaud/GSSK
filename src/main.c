#include "gssk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <model.json> [output.csv]\n", argv[0]);
    return EXIT_FAILURE;
  }

  // Read JSON file
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror("Error opening file");
    return EXIT_FAILURE;
  }

  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *data = malloc(length + 1);
  if (!data) {
    fclose(f);
    return EXIT_FAILURE;
  }
  size_t read_bytes = fread(data, 1, length, f);
  fclose(f);
  if (read_bytes != (size_t)length) {
    fprintf(stderr, "Error reading file\n");
    free(data);
    return EXIT_FAILURE;
  }
  data[length] = '\0';

  // Initialize Kernel
  GSSK_Instance *kernel = GSSK_Init(data);
  if (!kernel) {
    fprintf(stderr, "Failed to initialize GSSK kernel.\n");
    free(data);
    return EXIT_FAILURE;
  }

  // Prepare Output
  FILE *out = stdout;
  if (argc > 2) {
    out = fopen(argv[2], "w");
    if (!out) {
      perror("Error opening output file");
      GSSK_Free(kernel);
      free(data);
      return EXIT_FAILURE;
    }
  }

  // Header
  fprintf(out, "time");
  size_t node_count = GSSK_GetStateSize(kernel);
  for (size_t i = 0; i < node_count; i++) {
    const char *id = GSSK_GetNodeID(kernel, i);
    fprintf(out, ",%s", id ? id : "unknown");
  }
  fprintf(out, "\n");

  // Simulation Loop
  double t = GSSK_GetTStart(kernel);
  double t_end = GSSK_GetTEnd(kernel);
  double dt = GSSK_GetDt(kernel);

  while (t <= t_end + (dt * 0.01)) { // Small epsilon for float logic
    const double *state = GSSK_GetState(kernel);
    fprintf(out, "%.4f", t);
    for (size_t i = 0; i < node_count; i++) {
      fprintf(out, ",%.6f", state[i]);
    }
    fprintf(out, "\n");

    if (GSSK_Step(kernel, dt) != GSSK_SUCCESS) {
      fprintf(stderr, "Numerical divergence at t=%.4f\n", t);
      break;
    }
    t += dt;
  }

  // Cleanup
  if (out != stdout)
    fclose(out);
  GSSK_Free(kernel);
  free(data);

  return EXIT_SUCCESS;
}
