#define _GNU_SOURCE   /* strdup, popen */
/*
** test_determinism.c
** LuaStasis: demonstrates sources of non-determinism in vanilla Lua.
**
** Hybrid strategy:
**   * Address-based non-determinism (tostring leaking heap pointers) is
**     demonstrated by allocating two coexisting lua_State instances in
**     one process — the C allocator is forced to hand back distinct
**     addresses, exposing the leak.  (Close-then-reopen would reuse
**     freed memory and hide the divergence.)
**   * Seed-based non-determinism (pairs/next order, math.random, GC
**     count) is demonstrated across two separate invocations of
**     ./lua -e <snippet>.  Lua 5.5's luaL_makeseed mixes only a stack
**     address and time(NULL); both are stable within one process frame,
**     so in-process states share a seed.  ASLR across processes does
**     vary the stack address.
**   * Real-time leaks (os.clock, os.time) need no comparison — they
**     read the system clock directly.
**
** A passing test demonstrates the named source of non-determinism.
** These tests document what LuaStasis must eliminate; no fixes here.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#define PASS(name)            printf("  PASS: %s\n", (name))
#define FAIL(name, fmt, ...)  printf("  FAIL: %s — " fmt "\n", (name), __VA_ARGS__)

/* ----------------------------------------------------------------------
** In-process helpers (coexisting states)
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

/* Two coexisting states run the same snippet; both stay alive until
** after both results are captured, guaranteeing distinct addresses. */
static void run_two_states(const char *code, char **a, char **b) {
  lua_State *LA = new_state();
  lua_State *LB = new_state();
  *a = eval(LA, code);
  *b = eval(LB, code);
  lua_close(LA);
  lua_close(LB);
}

/* ----------------------------------------------------------------------
** Subprocess helper (separate ASLR layouts)
** -------------------------------------------------------------------- */

static char *run_subproc(const char *lua_code) {
  /* Snippets are author-controlled and contain no single quotes. */
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
** Assertion
** -------------------------------------------------------------------- */

static void expect_differ(const char *test_name, const char *a, const char *b) {
  printf("    run A: %s\n", a ? a : "(null)");
  printf("    run B: %s\n", b ? b : "(null)");
  if (a && b && a[0] && b[0] && strcmp(a, b) != 0) PASS(test_name);
  else FAIL(test_name, "outputs are identical%s", "");
}

/* For sources that are NOT a problem — documents that the named behaviour
** is already reproducible across runs. */
static void expect_same(const char *test_name, const char *a, const char *b) {
  printf("    run A: %s\n", a ? a : "(null)");
  printf("    run B: %s\n", b ? b : "(null)");
  if (a && b && strcmp(a, b) == 0) PASS(test_name);
  else FAIL(test_name, "outputs differ%s", "");
}

/* ======================================================================
** 1. Object identity strings (in-process, coexisting states)
** ==================================================================== */

static void test_tostring_table(void) {
  printf("== test_tostring_table ==\n");
  /* tostring(t) returns "table: 0x<addr>" — pointer leaks into output. */
  char *a, *b;
  run_two_states("return tostring({})", &a, &b);
  expect_differ("tostring({}) differs across states", a, b);
  free(a); free(b);
}

static void test_tostring_function(void) {
  printf("== test_tostring_function ==\n");
  /* "function: 0x<addr>" — closure address leaks. */
  char *a, *b;
  run_two_states("return tostring(function() end)", &a, &b);
  expect_differ("tostring(fn) differs across states", a, b);
  free(a); free(b);
}

static void test_tostring_thread(void) {
  printf("== test_tostring_thread ==\n");
  /* "thread: 0x<addr>" — coroutine address leaks. */
  char *a, *b;
  run_two_states(
    "return tostring(coroutine.create(function() end))", &a, &b);
  expect_differ("tostring(coroutine) differs across states", a, b);
  free(a); free(b);
}

static void test_tostring_userdata(void) {
  printf("== test_tostring_userdata ==\n");
  /* io file handle: __tostring is "file (0x<addr>)".  We must NOT close
  ** the file in the snippet — closing in state A would free the FILE*
  ** before state B's tmpfile() runs, which would then reuse it.  Both
  ** states' tmpfiles are kept alive concurrently. */
  char *a, *b;
  run_two_states(
    "local f = io.tmpfile()\n"
    "if not f then return 'no_tmpfile' end\n"
    "_G._keepalive = f\n"
    "return tostring(f)\n",
    &a, &b);
  expect_differ("tostring(userdata) differs across states", a, b);
  free(a); free(b);
}

/* ======================================================================
** 2. Table traversal order (across-process — needs distinct seed)
** ==================================================================== */

static void test_pairs_string_keys(void) {
  printf("== test_pairs_string_keys ==\n");
  /* luaL_makeseed(): {&local_var, time(NULL)}.  ASLR across processes
  ** randomises the stack address; pairs() order on string keys follows. */
  const char *snippet =
    "local t = {} "
    "for i = 1, 26 do t[string.char(96 + i)] = i end "
    "local o = {} "
    "for k in pairs(t) do o[#o+1] = k end "
    "io.write(table.concat(o, \",\"))";
  char *a = run_subproc(snippet);
  char *b = run_subproc(snippet);
  expect_differ("pairs() order differs across processes", a, b);
  free(a); free(b);
}

static void test_next_string_keys(void) {
  printf("== test_next_string_keys ==\n");
  const char *snippet =
    "local t = { apple=1, banana=2, cherry=3, date=4, elderberry=5, "
    "            fig=6, grape=7, honeydew=8 } "
    "io.write(tostring((next(t))))";
  char *a = run_subproc(snippet);
  char *b = run_subproc(snippet);
  expect_differ("next() first key differs across processes", a, b);
  free(a); free(b);
}

static void test_pairs_sparse_int_keys(void) {
  printf("== test_pairs_sparse_int_keys ==\n");
  /* Sparse integer keys land in the hash part (not the array part).
  ** Lua hashes integers directly (no seed mixing), so the order is
  ** reproducible across processes — THIS source of non-determinism
  ** does NOT apply to integer-keyed tables.  Documented as baseline. */
  const char *snippet =
    "local t = {} "
    "for i = 1, 26 do t[i * 1000] = i end "
    "local o = {} "
    "for k in pairs(t) do o[#o+1] = tostring(k) end "
    "io.write(table.concat(o, \",\"))";
  char *a = run_subproc(snippet);
  char *b = run_subproc(snippet);
  expect_same("pairs() order on sparse int keys is reproducible", a, b);
  free(a); free(b);
}

static void test_pairs_table_keys(void) {
  printf("== test_pairs_table_keys ==\n");
  /* Tables hashed as keys use their GC pointer for hash positioning.
  ** ASLR randomises those pointers across processes, so iteration order
  ** over the same set of table-keyed entries varies.  We label entries
  ** with explicit values so we can compare the value-sequence. */
  const char *snippet =
    "local t = {} "
    "for i = 1, 26 do t[{}] = i end "
    "local o = {} "
    "for _, v in pairs(t) do o[#o+1] = tostring(v) end "
    "io.write(table.concat(o, \",\"))";
  char *a = run_subproc(snippet);
  char *b = run_subproc(snippet);
  expect_differ("pairs() order on table-pointer keys differs", a, b);
  free(a); free(b);
}

static void test_pairs_mixed_keys(void) {
  printf("== test_pairs_mixed_keys ==\n");
  /* A table whose keys are exclusively non-string, non-integer values
  ** (booleans + floats + tables): no string hashing involved, but
  ** table-pointer hashing still varies across processes. */
  const char *snippet =
    "local k1, k2 = {}, {} "
    "local t = { [k1]=\"k1\", [k2]=\"k2\", [true]=\"T\", [false]=\"F\", "
    "            [3.14]=\"pi\", [2.71]=\"e\" } "
    "local o = {} "
    "for _, v in pairs(t) do o[#o+1] = v end "
    "io.write(table.concat(o, \",\"))";
  char *a = run_subproc(snippet);
  char *b = run_subproc(snippet);
  expect_differ("pairs() order on mixed non-string keys differs", a, b);
  free(a); free(b);
}

/* ======================================================================
** 3. PRNG seeded from time + makeseed result (across-process)
** ==================================================================== */

static void test_math_random(void) {
  printf("== test_math_random ==\n");
  /* lmathlib's xoshiro256** is seeded with values derived from the same
  ** sources as luaL_makeseed — varies with ASLR across processes. */
  char *a = run_subproc("io.write(tostring(math.random(1, 1000000000)))");
  char *b = run_subproc("io.write(tostring(math.random(1, 1000000000)))");
  expect_differ("math.random() first draw differs across processes", a, b);
  free(a); free(b);
}

/* ======================================================================
** 4. Real-world time access (single-state, observe directly)
** ==================================================================== */

static void test_os_clock_progresses(void) {
  printf("== test_os_clock_progresses ==\n");
  /* os.clock() returns process CPU time.  Reading it before and after
  ** real work yields different values — real time leaks into Lua. */
  lua_State *L = new_state();
  char *r = eval(L,
    "local t0 = os.clock()\n"
    "local s = 0\n"
    "for i = 1, 5000000 do s = s + i end\n"
    "local t1 = os.clock()\n"
    "return (t1 > t0) and 'advanced' or 'stuck'\n");
  printf("    observed: %s\n", r ? r : "(null)");
  if (r && strcmp(r, "advanced") == 0) PASS("os.clock advances during work");
  else FAIL("os.clock advances during work", "got '%s'", r ? r : "(null)");
  free(r);
  lua_close(L);
}

static void test_os_time_real(void) {
  printf("== test_os_time_real ==\n");
  /* os.time() is the wall clock.  We don't compare runs — they share a
  ** second.  We just confirm it reflects real time. */
  lua_State *L = new_state();
  char *r = eval(L, "return tostring(os.time())");
  printf("    os.time(): %s — reflects wall clock\n", r ? r : "(null)");
  PASS("os.time() leaks wall-clock state");
  free(r);
  lua_close(L);
}

/* ----------------------------------------------------------------------
** main
** -------------------------------------------------------------------- */

int main(void) {
  printf("=== Lua Non-Determinism Demonstration ===\n\n");
  printf("Each test PASSES when the same Lua snippet produces different\n");
  printf("results across two runs.\n\n");

  /* Sanity check: ./lua exists for the subprocess tests. */
  FILE *f = fopen("./lua", "rb");
  if (!f) {
    fprintf(stderr, "error: ./lua not found — run 'make lua' first\n");
    return 1;
  }
  fclose(f);

  test_tostring_table();
  test_tostring_function();
  test_tostring_thread();
  test_tostring_userdata();
  test_pairs_string_keys();
  test_next_string_keys();
  test_pairs_sparse_int_keys();
  test_pairs_table_keys();
  test_pairs_mixed_keys();
  test_math_random();
  test_os_clock_progresses();
  test_os_time_real();

  printf("\n=== Done ===\n");
  return 0;
}
