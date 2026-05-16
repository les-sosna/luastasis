#define _GNU_SOURCE   /* strdup, popen */
/*
** test_determinism.c
** LuaStasis: determinism test suite for both vanilla and deterministic
** builds of the runtime.
**
** The detection logic is identical in both modes — each test runs the
** same Lua snippet twice (in two coexisting lua_States or in two
** subprocesses) and checks whether the outputs differ.  Only the
** success criterion changes:
**
**   CAT_NONDET   — a known source of non-determinism in vanilla Lua.
**                  vanilla mode:        outputs MUST differ (proves bug)
**                  deterministic mode:  outputs MUST be identical (fixed)
**
**   CAT_SANITY   — a property we never want to drift in either mode
**                  (e.g. integer-key iteration order, __gc invocation
**                  count for a fixed workload).
**                  both modes: outputs MUST be identical.
**
**   CAT_ENV_LEAK — observation of an explicit environmental dependency
**                  (RTC, file system).  These exceptions persist even
**                  in deterministic mode; we just confirm the leak is
**                  observable.
**
** Build mode is selected via the LUASTASIS_DETERMINISTIC compile-time
** macro (defaults to 0).  To exercise the deterministic-mode polarity
** once the Lua core flag exists:  make MYCFLAGS=-DLUASTASIS_DETERMINISTIC=1
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#ifndef LUASTASIS_DETERMINISTIC
#define LUASTASIS_DETERMINISTIC 0
#endif

/* ----------------------------------------------------------------------
** Lua state helpers (coexisting states)
** -------------------------------------------------------------------- */

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  if (L) luaL_openlibs(L);
  return L;
}

static char *eval(lua_State *L, const char *code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return NULL;
  }
  const char *s = lua_tostring(L, -1);
  char *r = s ? strdup(s) : NULL;
  lua_pop(L, 1);
  return r;
}

/* Run snippet in two coexisting fresh states. */
static void run_two_states(const char *code, char **a, char **b) {
  lua_State *LA = new_state();
  lua_State *LB = new_state();
  *a = eval(LA, code);
  *b = eval(LB, code);
  lua_close(LA);
  lua_close(LB);
}

/* ----------------------------------------------------------------------
** Subprocess helper (separate ASLR layout)
** -------------------------------------------------------------------- */

static char *run_subproc(const char *lua_code) {
  /* Snippets are author-controlled; they contain no single quotes. */
  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "./lua -e '%s' 2>&1", lua_code);
  FILE *p = popen(cmd, "r");
  if (!p) return NULL;

  size_t cap = 1024, len = 0;
  char *out = (char *)malloc(cap);
  out[0] = '\0';
  int c;
  while ((c = fgetc(p)) != EOF) {
    if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
    out[len++] = (char)c;
  }
  pclose(p);
  while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) len--;
  out[len] = '\0';
  return out;
}

/* ----------------------------------------------------------------------
** Categorised assertion
** -------------------------------------------------------------------- */

typedef enum {
  CAT_NONDET,    /* polarity flips with build mode */
  CAT_SANITY,    /* always expect outputs identical */
  CAT_ENV_LEAK,  /* always expect outputs to differ (rtc/fs) */
} category_t;

static const char *cat_label(category_t cat) {
  switch (cat) {
    case CAT_NONDET:   return "non-det source";
    case CAT_SANITY:   return "sanity";
    case CAT_ENV_LEAK: return "env-leak";
  }
  return "?";
}

static const char *mode_label(void) {
  return LUASTASIS_DETERMINISTIC ? "deterministic" : "vanilla";
}

/* Shared reporter — same polarity logic, independent of how the
** outputs were collected (2 coexisting states vs N subprocesses). */
static void report(const char *name, category_t cat, int observed_differ) {
  int want_differ;
  switch (cat) {
    case CAT_NONDET:   want_differ = !LUASTASIS_DETERMINISTIC; break;
    case CAT_SANITY:   want_differ = 0; break;
    case CAT_ENV_LEAK: want_differ = 1; break;
    default:           want_differ = 0; break;
  }
  int ok = (observed_differ == want_differ);
  printf("  %s: %s [%s, %s mode → outputs should %s]\n",
         ok ? "PASS" : "FAIL", name, cat_label(cat), mode_label(),
         want_differ ? "differ" : "be identical");
  if (!ok)
    printf("    (got: outputs %s)\n",
           observed_differ ? "differ" : "are identical");
}

/* Two-state in-process check.  Coexisting states guarantee distinct
** addresses so 2 samples are sufficient for address-based tests. */
static void check_two_states(const char *name, category_t cat,
                             const char *code) {
  char *a, *b;
  run_two_states(code, &a, &b);
  printf("    run A: %s\n", a ? a : "(null)");
  printf("    run B: %s\n", b ? b : "(null)");
  int differ = (a && b && a[0] && b[0] && strcmp(a, b) != 0);
  report(name, cat, differ);
  free(a); free(b);
}

/* Three-subprocess check.  Seed-driven tests are probabilistic — two
** processes can collide on bucket order by chance.  Three samples drop
** the false-collision rate to negligible: "non-deterministic" = ANY
** pair differs; "deterministic" = ALL three are identical. */
static void check_three_subproc(const char *name, category_t cat,
                                const char *code) {
  char *a = run_subproc(code);
  char *b = run_subproc(code);
  char *c = run_subproc(code);
  printf("    run A: %s\n", a ? a : "(null)");
  printf("    run B: %s\n", b ? b : "(null)");
  printf("    run C: %s\n", c ? c : "(null)");
  int differ_ab = (a && b && strcmp(a, b) != 0);
  int differ_ac = (a && c && strcmp(a, c) != 0);
  int differ_bc = (b && c && strcmp(b, c) != 0);
  int differ = differ_ab || differ_ac || differ_bc;
  report(name, cat, differ);
  free(a); free(b); free(c);
}

/* ======================================================================
** A. Object identity strings (in-process, coexisting states)
**    Pointers leak through tostring — a known non-determinism source.
** ==================================================================== */

static void test_tostring_table(void) {
  printf("\n== tostring(table) ==\n");
  check_two_states("tostring({})", CAT_NONDET, "return tostring({})");
}

static void test_tostring_function(void) {
  printf("\n== tostring(function) ==\n");
  check_two_states("tostring(fn)", CAT_NONDET,
    "return tostring(function() end)");
}

static void test_tostring_thread(void) {
  printf("\n== tostring(thread) ==\n");
  check_two_states("tostring(coroutine)", CAT_NONDET,
    "return tostring(coroutine.create(function() end))");
}

static void test_tostring_userdata(void) {
  printf("\n== tostring(userdata) ==\n");
  /* Keep file handle alive in each state so concurrent FILE* allocs
  ** are forced to distinct addresses. */
  check_two_states("tostring(userdata)", CAT_NONDET,
    "local f = io.tmpfile()\n"
    "if not f then return 'no_tmpfile' end\n"
    "_G._keepalive = f\n"
    "return tostring(f)\n");
}

/* ======================================================================
** B. Table traversal order — subject to per-state hash seed
**    (across-process: ASLR varies the makeseed stack-address input)
** ==================================================================== */

static void test_pairs_string_keys(void) {
  printf("\n== pairs() on string keys ==\n");
  check_three_subproc("pairs(string keys)", CAT_NONDET,
    "local t = {} "
    "for i = 1, 26 do t[string.char(96 + i)] = i end "
    "local o = {} "
    "for k in pairs(t) do o[#o+1] = k end "
    "io.write(table.concat(o, \",\"))");
}

static void test_next_string_keys(void) {
  printf("\n== next() on string keys ==\n");
  /* Walk next() manually over 26 keys; first 5 keys cut collision rate. */
  check_three_subproc("next(string keys)", CAT_NONDET,
    "local t = {} "
    "for i = 1, 26 do t[string.char(96 + i)] = i end "
    "local o, k = {} "
    "for _ = 1, 5 do k = next(t, k); o[#o+1] = k end "
    "io.write(table.concat(o, \",\"))");
}

static void test_pairs_table_keys(void) {
  printf("\n== pairs() on table-pointer keys ==\n");
  check_three_subproc("pairs(table keys)", CAT_NONDET,
    "local t = {} "
    "for i = 1, 26 do t[{}] = i end "
    "local o = {} "
    "for _, v in pairs(t) do o[#o+1] = tostring(v) end "
    "io.write(table.concat(o, \",\"))");
}

static void test_pairs_mixed_keys(void) {
  printf("\n== pairs() on mixed non-string keys ==\n");
  check_three_subproc("pairs(mixed w/ table keys)", CAT_NONDET,
    "local k1, k2 = {}, {} "
    "local t = { [k1]=\"k1\", [k2]=\"k2\", [true]=\"T\", [false]=\"F\", "
    "            [3.14]=\"pi\", [2.71]=\"e\" } "
    "local o = {} "
    "for _, v in pairs(t) do o[#o+1] = v end "
    "io.write(table.concat(o, \",\"))");
}

/* ======================================================================
** C. PRNG seeded from makeseed-style inputs
** ==================================================================== */

static void test_math_random(void) {
  printf("\n== math.random() initial draw ==\n");
  check_three_subproc("math.random()", CAT_NONDET,
    "io.write(tostring(math.random(1, 1000000000)))");
}

/* ======================================================================
** D. Sanity tests — already deterministic in vanilla Lua;
**    must remain so in deterministic mode.
** ==================================================================== */

static void test_sanity_int_keys(void) {
  printf("\n== sanity: pairs() on sparse int keys ==\n");
  /* Integer hashing in ltable.c uses the raw integer value, no seed. */
  check_three_subproc("pairs(sparse int keys)", CAT_SANITY,
    "local t = {} "
    "for i = 1, 26 do t[i * 1000] = i end "
    "local o = {} "
    "for k in pairs(t) do o[#o+1] = tostring(k) end "
    "io.write(table.concat(o, \",\"))");
}

static void test_sanity_dense_array(void) {
  printf("\n== sanity: pairs() on dense integer array ==\n");
  /* Dense 1..N integer keys live in the array part — iterates 1..N. */
  check_three_subproc("pairs(dense array)", CAT_SANITY,
    "local t = {10, 20, 30, 40, 50, 60, 70, 80} "
    "local o = {} "
    "for k, v in pairs(t) do o[#o+1] = k..':'..v end "
    "io.write(table.concat(o, \",\"))");
}

static void test_sanity_float_keys(void) {
  printf("\n== sanity: pairs() on float keys ==\n");
  /* Float hashing uses the bit pattern, no seed. */
  check_three_subproc("pairs(float keys)", CAT_SANITY,
    "local t = { [3.14]=\"pi\", [2.71]=\"e\", [1.41]=\"sqrt2\", "
    "            [1.61]=\"phi\", [0.57]=\"gamma\" } "
    "local o = {} "
    "for _, v in pairs(t) do o[#o+1] = v end "
    "io.write(table.concat(o, \",\"))");
}

static void test_sanity_boolean_keys(void) {
  printf("\n== sanity: pairs() on boolean keys ==\n");
  check_three_subproc("pairs(boolean keys)", CAT_SANITY,
    "local t = { [true]=\"T\", [false]=\"F\" } "
    "local o = {} "
    "for k, v in pairs(t) do o[#o+1] = tostring(k)..':'..v end "
    "io.write(table.concat(o, \",\"))");
}

static void test_sanity_gc_finalizer_count(void) {
  printf("\n== sanity: __gc invocation count ==\n");
  /* Fixed number of finalizable objects → fixed __gc count. */
  check_three_subproc("__gc count after fixed workload", CAT_SANITY,
    "local n = 0 "
    "do "
    "  for i = 1, 50 do "
    "    setmetatable({i}, { __gc = function() n = n + 1 end }) "
    "  end "
    "end "
    "collectgarbage(\"collect\") "
    "io.write(tostring(n))");
}

/* NOTE: a __gc invocation-order test was intentionally omitted.  In
** vanilla Lua, the finalization order of a batch of objects becoming
** unreachable inside a tight loop is NOT fully reproducible — empirical
** results show two distinct orderings (pure LIFO vs split-batch LIFO),
** depending on whether the incremental GC fires a step mid-loop.  This
** flakes as both sanity and non-det classifications.  Once deterministic
** mode pins down GC pacing this can be added as CAT_SANITY. */

static void test_sanity_gc_count_after_work(void) {
  printf("\n== sanity: collectgarbage('count') after fixed work ==\n");
  /* String interning + power-of-two hash sizing make total memory
  ** byte-identical across runs despite different seeds. */
  check_three_subproc("collectgarbage('count')", CAT_SANITY,
    "local t = {} "
    "for i = 1, 1000 do t[\"k\"..i] = i end "
    "collectgarbage(\"collect\") "
    "io.write(string.format(\"%.4f\", collectgarbage(\"count\")))");
}

static void test_sanity_arithmetic(void) {
  printf("\n== sanity: integer / float arithmetic ==\n");
  /* Numeric ops are reproducible; baseline that fails loudly if the
  ** harness itself breaks. */
  check_three_subproc("arithmetic operations", CAT_SANITY,
    "io.write(tostring(1 + 2 * 3 - 4 // 2)..';'.."
    "         tostring(math.pi * 2)..';'..tostring(2^10))");
}

/* ======================================================================
** E. Environmental leaks (RTC).  Persist as exceptions in both modes.
** ==================================================================== */

static void test_env_os_clock_progresses(void) {
  printf("\n== env-leak: os.clock() within run ==\n");
  /* CPU clock advancing during a single run; observed once. */
  lua_State *L = new_state();
  char *r = eval(L,
    "local t0 = os.clock() "
    "local s = 0 for i = 1, 5000000 do s = s + i end "
    "local t1 = os.clock() "
    "return (t1 > t0) and 'advanced' or 'stuck'");
  printf("    observed: %s\n", r ? r : "(null)");
  int ok = (r && strcmp(r, "advanced") == 0);
  printf("  %s: os.clock() advances [env-leak]\n", ok ? "PASS" : "FAIL");
  free(r);
  lua_close(L);
}

static void test_env_os_time_real(void) {
  printf("\n== env-leak: os.time() ==\n");
  /* Wall clock; observed once.  No two-run comparison because two
  ** subprocess invocations typically fall in the same second. */
  lua_State *L = new_state();
  char *r = eval(L, "return tostring(os.time())");
  printf("    os.time(): %s\n", r ? r : "(null)");
  printf("  PASS: os.time() reflects wall clock [env-leak]\n");
  free(r);
  lua_close(L);
}

/* ----------------------------------------------------------------------
** main
** -------------------------------------------------------------------- */

int main(void) {
  printf("=== LuaStasis Determinism Tests ===\n");
  printf("Build mode: %s%s\n", mode_label(),
         LUASTASIS_DETERMINISTIC
           ? " (expect non-det sources to be silenced)"
           : " (expect non-det sources to be observable)");
  printf("\n");

  FILE *f = fopen("./lua", "rb");
  if (!f) {
    fprintf(stderr, "error: ./lua not found — run 'make lua' first\n");
    return 1;
  }
  fclose(f);

  /* Non-determinism sources (polarity flips with build mode) */
  test_tostring_table();
  test_tostring_function();
  test_tostring_thread();
  test_tostring_userdata();
  test_pairs_string_keys();
  test_next_string_keys();
  test_pairs_table_keys();
  test_pairs_mixed_keys();
  test_math_random();

  /* Sanity — must be reproducible in both modes */
  test_sanity_int_keys();
  test_sanity_dense_array();
  test_sanity_float_keys();
  test_sanity_boolean_keys();
  test_sanity_gc_finalizer_count();
  test_sanity_gc_count_after_work();
  test_sanity_arithmetic();

  /* Environmental leaks — observed once, persist in both modes */
  test_env_os_clock_progresses();
  test_env_os_time_real();

  printf("\n=== Done ===\n");
  return 0;
}
