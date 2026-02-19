#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 4096
#define TOLERANCE 1e-6

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <file1.csv> <file2.csv>\n", argv[0]);
    return 2;
  }

  FILE *f1 = fopen(argv[1], "r");
  FILE *f2 = fopen(argv[2], "r");

  if (!f1 || !f2) {
    perror("Error opening files");
    if (f1)
      fclose(f1);
    if (f2)
      fclose(f2);
    return 2;
  }

  char line1[MAX_LINE];
  char line2[MAX_LINE];
  int line_num = 0;

  while (fgets(line1, MAX_LINE, f1) && fgets(line2, MAX_LINE, f2)) {
    line_num++;

    // Remove newlines
    line1[strcspn(line1, "\r\n")] = 0;
    line2[strcspn(line2, "\r\n")] = 0;

    if (line_num == 1) { // Header check
      if (strcmp(line1, line2) != 0) {
        fprintf(stderr, "Header mismatch at line %d\n", line_num);
        fprintf(stderr, "  Exp: %s\n", line1);
        fprintf(stderr, "  Got: %s\n", line2);
        return 1;
      }
      continue;
    }

    // Compare values
    char *s1 = line1;
    char *s2 = line2;
    char *ctx1, *ctx2;
    char *tok1 = strtok_r(s1, ",", &ctx1);
    char *tok2 = strtok_r(s2, ",", &ctx2);
    int col = 0;

    while (tok1 && tok2) {
      col++;
      double v1 = atof(tok1);
      double v2 = atof(tok2);

      if (fabs(v1 - v2) > TOLERANCE) {
        fprintf(stderr, "Value mismatch at line %d, col %d\n", line_num, col);
        fprintf(stderr, "  Exp: %f\n", v1);
        fprintf(stderr, "  Got: %f\n", v2);
        fprintf(stderr, "  Diff: %e\n", fabs(v1 - v2));
        return 1;
      }
      tok1 = strtok_r(NULL, ",", &ctx1);
      tok2 = strtok_r(NULL, ",", &ctx2);
    }

    if (tok1 || tok2) {
      fprintf(stderr, "Column count mismatch at line %d\n", line_num);
      return 1;
    }
  }

  if (fgets(line1, MAX_LINE, f1) || fgets(line2, MAX_LINE, f2)) {
    fprintf(stderr, "File length mismatch\n");
    return 1;
  }

  fclose(f1);
  fclose(f2);
  return 0;
}
