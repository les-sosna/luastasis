/*
** luaser_tojson_main.c
** CLI entry point for the luaser-to-JSON converter.
**
** Usage:  luaser_tojson <file.bin>
**         luaser_tojson < file.bin
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "luaser_tojson.h"

int main(int argc, char **argv) {
  FILE *f;
  if (argc > 1) {
    f = fopen(argv[1], "rb");
    if (!f) {
      fprintf(stderr, "luaser_tojson: cannot open '%s': %s\n",
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
        fprintf(stderr, "luaser_tojson: out of memory\n");
        free(buf);
        return 1;
      }
      buf = nb; cap = ncap;
    }
    buf[sz++] = (uint8_t)c;
  }
  if (f != stdin) fclose(f);

  int ret = luaser_tojson(buf, sz, stdout);
  free(buf);
  return ret;
}
