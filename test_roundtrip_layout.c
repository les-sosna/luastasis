/*
** test_roundtrip_layout.c
**
** Minimal repro for an lstasis save/load determinism gap.
**
** Expected:  for any state L, with
**   S1 = lstasis_save(L)
**   L2 = lstasis_load(S1)
**   S2 = lstasis_save(L2)
** then S1 == S2 byte-for-byte (in deterministic mode).
**
** Observed:  S1 != S2 once a single table has been built up by a
** non-trivial insertion sequence (inserts mixed with deletes).  Cause:
** lstasis_save walks each table in t->node[i] slot order, and
** lstasis_load reconstructs the hash part by replaying that order
** through luaH_set into a fresh hash array.  Lua's insertion +
** collision-displacement algorithm is path-dependent, so the resulting
** node layout in L2 doesn't match L's.  Discovery in the second save
** then walks the differently-laid-out nodes, assigns ids in a
** different order, and the output bytes diverge.
**
** This repro deliberately uses no standard libraries — just one
** globally-anchored table populated with grow-and-shrink churn — to
** isolate the table-layout pathway.
**
** Build & run:
**   make DETERMINISTIC=1 test_roundtrip_layout && ./test_roundtrip_layout
*/

#include <stdio.h>
#include "luaconf.h"

#if !LUASTASIS_DETERMINISTIC

int main(void) {
  printf("=== LuaStasis roundtrip layout test ===\n");
  printf("SKIP: requires deterministic build (make DETERMINISTIC=1).\n");
  return 0;
}

#else  /* LUASTASIS_DETERMINISTIC */

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lstasis.h"

/* Build one table whose hash part has been through enough churn (some
** inserts followed by some deletes and replacements) that current node
** positions no longer match what a fresh insert-in-saved-order replay
** would produce. */
static void populate(lua_State *L) {
  int i;
  lua_newtable(L);
  for (i = 1; i <= 8; i++) {
    char k[8]; snprintf(k, sizeof k, "k_%d", i);
    lua_pushinteger(L, i);
    lua_setfield(L, -2, k);
  }
  for (i = 1; i <= 4; i++) {
    char k[8]; snprintf(k, sizeof k, "k_%d", i);
    lua_pushnil(L);
    lua_setfield(L, -2, k);
  }
  for (i = 1; i <= 4; i++) {
    char k[8]; snprintf(k, sizeof k, "a_%d", i);
    lua_pushinteger(L, i);
    lua_setfield(L, -2, k);
  }
  lua_setglobal(L, "t");
}

static size_t first_diff(const unsigned char *a, size_t na,
                         const unsigned char *b, size_t nb) {
  size_t lim = na < nb ? na : nb;
  size_t i;
  for (i = 0; i < lim; i++) if (a[i] != b[i]) return i;
  return lim;
}

int main(void) {
  lua_State *L;
  lua_State *L2;
  unsigned char *s1 = NULL;
  unsigned char *s2 = NULL;
  size_t n1 = 0;
  size_t n2 = 0;
  int ok;
  char err[256] = "";

  printf("=== LuaStasis roundtrip layout test ===\n");

  L = luaL_newstate();
  if (!L) { fprintf(stderr, "newstate failed\n"); return 2; }
  populate(L);

  if (lstasis_save(L, NULL, &s1, &n1, err, sizeof err) != 0) {
    fprintf(stderr, "first save failed: %s\n", err);
    lua_close(L); return 2;
  }

  L2 = lstasis_load(s1, n1, NULL, err, sizeof err);
  if (!L2) { fprintf(stderr, "load failed: %s\n", err); free(s1); lua_close(L); return 2; }

  if (lstasis_save(L2, NULL, &s2, &n2, err, sizeof err) != 0) {
    fprintf(stderr, "second save failed: %s\n", err);
    free(s1); lua_close(L2); lua_close(L); return 2;
  }

  ok = (n1 == n2) && (memcmp(s1, s2, n1) == 0);
  printf("save 1: %zu bytes\nsave 2: %zu bytes\n", n1, n2);
  if (ok) {
    printf("PASS: snapshots match byte-for-byte\n");
  } else {
    size_t off = first_diff(s1, n1, s2, n2);
    printf("FAIL: snapshots differ\n");
    printf("  first divergence at byte %zu / %zu\n", off, n1 < n2 ? n1 : n2);
    printf("  size delta: %lld bytes\n", (long long)n2 - (long long)n1);
  }

  free(s1); free(s2);
  lua_close(L2); lua_close(L);
  return ok ? 0 : 1;
}

#endif  /* LUASTASIS_DETERMINISTIC */
