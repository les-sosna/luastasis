/*
** test_json.c  -  Golden-file tests for lstasis_tojson.
**
** Runs only in deterministic builds (LUASTASIS_DETERMINISTIC=1) where the
** snapshot byte layout - and therefore the JSON - is stable across runs.
** In vanilla mode the binary just prints a skip note and exits 0.
**
** Usage:
**   ./test_json            compare snapshot JSON to checked-in goldens
**   ./test_json --update   regenerate goldens from current output
**
** Goldens live at testdata/json/empty.json and testdata/json/with_libs.json,
** resolved relative to the current working directory.
*/

#define _GNU_SOURCE   /* open_memstream */
#include <stdio.h>
#include "luaconf.h"

#if !LUASTASIS_DETERMINISTIC

int main(void) {
  printf("=== LuaStasis JSON snapshot tests ===\n");
  printf("SKIP: JSON snapshot tests require a deterministic build "
         "(make DETERMINISTIC=1).\n");
  return 0;
}

#else  /* LUASTASIS_DETERMINISTIC */

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lstasis.h"
#include "lstasis_tojson.h"

/* The checked-in goldens were generated with LUA_COMPAT_MATHLIB enabled
** (which adds atan2/cosh/sinh/tanh/pow/log10 to the math library and is
** turned on by ltests.h via LUA_USER_H). Building this test without that
** flag produces snapshots with fewer objects and the comparison fails
** in confusing ways. Fail fast at compile time instead. The CI build passes
** TESTS='-DLUA_USER_H="\"ltests.h\"" -DLUA_USE_APICHECK -Og -g', which is
** the configuration the goldens are pinned to. */
#if !defined(LUA_COMPAT_MATHLIB)
#error "test_json must be built with LUA_COMPAT_MATHLIB defined " \
       "(typically via TESTS='-DLUA_USER_H=\"\\\"ltests.h\\\"\" ...'). " \
       "See the comment above this #error for details."
#endif

static const lstasis_Lib std_libs[] = {
  {"base",      luaopen_base},
  {"package",   luaopen_package},
  {"coroutine", luaopen_coroutine},
  {"table",     luaopen_table},
  {"string",    luaopen_string},
  {"math",      luaopen_math},
  {"io",        luaopen_io},
  {"os",        luaopen_os},
  {"utf8",      luaopen_utf8},
  {"debug",     luaopen_debug},
  {NULL, NULL}
};

#define EMPTY_GOLDEN  "testdata/json/empty.json"
#define LIBS_GOLDEN   "testdata/json/with_libs.json"
#define NESTED_GOLDEN "testdata/json/nested_pcalls.json"

/* Minimal lib set for the structure-inspection case: base provides pcall;
** coroutine is added because a suspended coroutine is needed to capture live
** call frames (coroutine is the lib that exposes yield). Keeping the set tiny
** keeps the golden small enough to read. */
static const lstasis_Lib base_co_libs[] = {
  {"base", luaopen_base}, {"coroutine", luaopen_coroutine}, {NULL, NULL}
};

static int g_update = 0;
static int g_failures = 0;

static char *slurp(const char *path, size_t *out_len) {
  FILE *fp;
  long sz;
  char *buf;
  size_t n;
  fp = fopen(path, "rb");
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
  sz = ftell(fp);
  if (sz < 0) { fclose(fp); return NULL; }
  rewind(fp);
  buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(fp); return NULL; }
  n = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  if (n != (size_t)sz) { free(buf); return NULL; }
  buf[sz] = '\0';
  if (out_len) *out_len = (size_t)sz;
  return buf;
}

static int write_file(const char *path, const char *data, size_t len) {
  FILE *fp;
  size_t n;
  fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "  cannot open '%s' for writing\n", path);
    return -1;
  }
  n = fwrite(data, 1, len, fp);
  fclose(fp);
  if (n != len) {
    fprintf(stderr, "  short write to '%s'\n", path);
    return -1;
  }
  return 0;
}

/* Print a unified-diff-like dump of the first divergence between two
** buffers.  Keeps the failure output digestible when the JSON is large. */
static void print_diff(const char *got, const char *want) {
  size_t i = 0;
  size_t line_start = 0;
  size_t line_no = 1;
  size_t j;
  while (got[i] && want[i] && got[i] == want[i]) {
    if (got[i] == '\n') { line_no++; line_start = i + 1; }
    i++;
  }
  fprintf(stderr, "  first diff at byte %zu (line %zu)\n", i, line_no);
  fprintf(stderr, "  got  : ");
  for (j = line_start; got[j] && got[j] != '\n' && j < line_start + 200; j++)
    fputc(got[j], stderr);
  fprintf(stderr, "\n");
  fprintf(stderr, "  want : ");
  for (j = line_start; want[j] && want[j] != '\n' && j < line_start + 200; j++)
    fputc(want[j], stderr);
  fprintf(stderr, "\n");
}

static void check_snapshot(lua_State *L, const lstasis_Lib *libs,
                           const char *test_name, const char *golden_path) {
  unsigned char *buf = NULL;
  size_t sz = 0;
  char *json = NULL;
  size_t json_len = 0;
  FILE *fp;
  int rc;
  char *want;
  size_t want_len;

  printf("== %s ==\n", test_name);

  if (lstasis_save(L, libs, &buf, &sz) != 0) {
    fprintf(stderr, "  FAIL: lstasis_save failed\n");
    g_failures++;
    return;
  }

  fp = open_memstream(&json, &json_len);
  if (!fp) {
    fprintf(stderr, "  FAIL: open_memstream failed\n");
    free(buf);
    g_failures++;
    return;
  }
  rc = lstasis_tojson(buf, sz, fp);
  fclose(fp);
  free(buf);
  if (rc != 0) {
    fprintf(stderr, "  FAIL: lstasis_tojson rc=%d\n", rc);
    free(json);
    g_failures++;
    return;
  }

  if (g_update) {
    if (write_file(golden_path, json, json_len) == 0)
      printf("  UPDATED: %s (%zu bytes)\n", golden_path, json_len);
    else
      g_failures++;
    free(json);
    return;
  }

  want = slurp(golden_path, &want_len);
  if (!want) {
    fprintf(stderr,
            "  FAIL: cannot read golden '%s' (run with --update to create)\n",
            golden_path);
    free(json);
    g_failures++;
    return;
  }

  if (want_len == json_len && memcmp(want, json, json_len) == 0) {
    printf("  PASS: matches %s (%zu bytes)\n", golden_path, json_len);
  } else {
    fprintf(stderr, "  FAIL: snapshot JSON differs from %s\n", golden_path);
    fprintf(stderr, "  got=%zu bytes  want=%zu bytes\n", json_len, want_len);
    print_diff(json, want);
    g_failures++;
  }

  free(want);
  free(json);
}

/* Test 1: empty state - lua_newstate with no openlibs and seed=0. */
static void test_empty_state(void) {
  lua_State *L = lua_newstate(luaL_alloc, NULL, 0);
  check_snapshot(L, NULL, "test_empty_state", EMPTY_GOLDEN);
  lua_close(L);
}

/* Test 2: state with the full standard library preloaded. */
static void test_with_libs(void) {
  lua_State *L = lua_newstate(luaL_alloc, NULL, 0);
  luaL_openlibs(L);
  check_snapshot(L, std_libs, "test_with_libs", LIBS_GOLDEN);
  lua_close(L);
}

static int dostr(lua_State *L, const char *s) {
  if (luaL_dostring(L, s) != LUA_OK) {
    fprintf(stderr, "  lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
  }
  return 1;
}

/* Test 3: a coroutine suspended inside two nested pcalls — to inspect the
** serialized call-frame / continuation structure (stacked finishpcall). */
static void test_nested_pcalls(void) {
  lua_State *L = lua_newstate(luaL_alloc, NULL, 0);
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);     lua_pop(L, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);
  dostr(L,
    "co = coroutine.create(function()\n"
    "  pcall(function()\n"
    "    pcall(function()\n"
    "      coroutine.yield('deep')\n"   /* suspend: two pcall frames live */
    "    end)\n"
    "  end)\n"
    "end)\n"
    "coroutine.resume(co)\n");
  check_snapshot(L, base_co_libs, "test_nested_pcalls", NESTED_GOLDEN);
  lua_close(L);
}

int main(int argc, char **argv) {
  int i;
  printf("=== LuaStasis JSON snapshot tests ===\n");

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--update") == 0) {
      g_update = 1;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }

  test_empty_state();
  test_with_libs();
  test_nested_pcalls();

  printf("=== %s ===\n", g_failures ? "FAILED" : "Done");
  return g_failures ? 1 : 0;
}

#endif  /* LUASTASIS_DETERMINISTIC */
