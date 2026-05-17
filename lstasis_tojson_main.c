/*
** lstasis_tojson_main.c
** CLI entry point for the lstasis-to-JSON converter.
**
** Usage:  lstasis_tojson <file.bin>
**         lstasis_tojson < file.bin
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "lstasis_tojson.h"

int main(int argc, char **argv) {
  FILE *f;
  if (argc > 1) {
    f = fopen(argv[1], "rb");
    if (!f) {
      fprintf(stderr, "lstasis_tojson: cannot open '%s': %s\n",
              argv[1], strerror(errno));
      return 1;
    }
  } else {
    f = stdin;
  }

  uint8_t *buf = NULL;
  size_t   cap = 0, sz = 0;
  int      c;
  while ((c = fgetc(f)) != EOF) {
    if (sz >= cap) {
      size_t ncap = cap ? cap * 2 : 8192;
      uint8_t *nb = (uint8_t *)realloc(buf, ncap);
      if (!nb) {
        fprintf(stderr, "lstasis_tojson: out of memory\n");
        free(buf);
        return 1;
      }
      buf = nb; cap = ncap;
    }
    buf[sz++] = (uint8_t)c;
  }
  if (f != stdin) fclose(f);

  int ret = lstasis_tojson(buf, sz, stdout);
  free(buf);
  return ret;
}
