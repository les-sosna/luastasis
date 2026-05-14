/*
** test_serial.c  –  Tests for the Lua state serializer.
**
** Compile & run:
**   make test_serial && ./test_serial
*/

#define LUA_CORE
#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lstate.h"
#include "lobject.h"
#include "ltable.h"
#include "lstate_serial.h"

/* -----------------------------------------------------------------------
** Helpers
** --------------------------------------------------------------------- */

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  return L;
}

static int run(lua_State *L, const char *code) {
  int r = luaL_dostring(L, code);
  if (r != LUA_OK) {
    fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  return r;
}

/* Execute code, save, reload, return the reloaded state.
   Caller owns the returned state and must lua_close() it. */
static lua_State *save_reload(lua_State *L, const char *setup_code) {
  if (setup_code) {
    int r = run(L, setup_code);
    if (r != LUA_OK) return NULL;
  }
  unsigned char *buf = NULL;
  size_t sz = 0;
  if (luaser_save(L, &buf, &sz) != 0) {
    fprintf(stderr, "save failed\n");
    return NULL;
  }
  lua_State *L2 = luaser_load(buf, sz);
  free(buf);
  return L2;
}

/* Push _G[name] onto the stack of L2; return lua_type() */
static int get_global(lua_State *L2, const char *name) {
  lua_getglobal(L2, name);
  return lua_type(L2, -1);
}

#define PASS(name) fprintf(stdout, "  PASS: %s\n", name)
#define FAIL(name, ...) do { fprintf(stdout, "  FAIL: %s  — " , name); \
  fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)

#define CHECK(cond, name, ...) do { if (cond) PASS(name); else FAIL(name, __VA_ARGS__); } while(0)

/* -----------------------------------------------------------------------
** Test 1 – basic scalar values
** --------------------------------------------------------------------- */
static void test_basic(void) {
  printf("== test_basic ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "n_int   = 42\n"
    "n_float = 3.14\n"
    "b_true  = true\n"
    "b_false = false\n"
    "s_hello = 'hello'\n"
    "n_nil   = nil\n"
  );
  lua_close(L);

  if (!L2) { FAIL("reload", "load returned NULL"); return; }

  get_global(L2, "n_int");
  CHECK(lua_type(L2,-1)==LUA_TNUMBER && lua_isinteger(L2,-1) && lua_tointeger(L2,-1)==42,
        "n_int=42", "got %s=%s", lua_typename(L2,lua_type(L2,-1)), lua_tostring(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "n_float");
  CHECK(lua_type(L2,-1)==LUA_TNUMBER && !lua_isinteger(L2,-1),
        "n_float is float", "type=%d", lua_type(L2,-1));
  double fv = lua_tonumber(L2,-1);
  CHECK(fv > 3.13 && fv < 3.15, "n_float≈3.14", "got %g", fv);
  lua_pop(L2, 1);

  get_global(L2, "b_true");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && lua_toboolean(L2,-1), "b_true", "");
  lua_pop(L2, 1);

  get_global(L2, "b_false");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && !lua_toboolean(L2,-1), "b_false", "");
  lua_pop(L2, 1);

  get_global(L2, "s_hello");
  CHECK(lua_type(L2,-1)==LUA_TSTRING && strcmp(lua_tostring(L2,-1),"hello")==0,
        "s_hello", "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "n_nil");
  CHECK(lua_type(L2,-1)==LUA_TNIL, "n_nil is nil", "type=%d", lua_type(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 2 – tables (array + hash + nested)
** --------------------------------------------------------------------- */
static void test_table(void) {
  printf("== test_table ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "arr = {10, 20, 30}\n"
    "t   = { x=1, y=2, z='three' }\n"
    "nested = { inner = { val = 99 } }\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* array */
  get_global(L2, "arr");
  lua_rawgeti(L2, -1, 1);
  CHECK(lua_tointeger(L2,-1)==10, "arr[1]=10", "got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);
  lua_rawgeti(L2, -1, 3);
  CHECK(lua_tointeger(L2,-1)==30, "arr[3]=30", "got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 2);

  /* hash */
  get_global(L2, "t");
  lua_getfield(L2, -1, "z");
  CHECK(strcmp(lua_tostring(L2,-1),"three")==0, "t.z='three'", "got '%s'",lua_tostring(L2,-1));
  lua_pop(L2, 2);

  /* nested */
  get_global(L2, "nested");
  lua_getfield(L2, -1, "inner");
  lua_getfield(L2, -1, "val");
  CHECK(lua_tointeger(L2,-1)==99, "nested.inner.val=99","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 3);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 3 – cyclic table reference
** --------------------------------------------------------------------- */
static void test_cycle(void) {
  printf("== test_cycle ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "t = {}\n"
    "t.self = t\n"
    "t.val  = 777\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "t");
  lua_getfield(L2, -1, "val");
  CHECK(lua_tointeger(L2,-1)==777, "t.val=777","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);
  /* t.self should be the same table */
  lua_getfield(L2, -1, "self");
  CHECK(lua_rawequal(L2,-1,-2), "t.self==t", "not same table");
  lua_pop(L2, 2);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 4 – closure with upvalue
** --------------------------------------------------------------------- */
static void test_closure_upvalue(void) {
  printf("== test_closure_upvalue ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "function make_counter(init)\n"
    "  local c = init\n"
    "  return function() c = c + 1; return c end\n"
    "end\n"
    "counter = make_counter(10)\n"
    "counter()  -- c=11\n"
    "counter()  -- c=12\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "counter");
  CHECK(lua_type(L2,-1)==LUA_TFUNCTION, "counter is function","type=%d",lua_type(L2,-1));
  lua_call(L2, 0, 1); /* should return 13 */
  CHECK(lua_tointeger(L2,-1)==13, "counter()=13 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 5 – shared upvalue between two closures
** --------------------------------------------------------------------- */
static void test_shared_upvalue(void) {
  printf("== test_shared_upvalue ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "do\n"
    "  local shared = 0\n"
    "  inc = function() shared = shared + 1 end\n"
    "  get = function() return shared end\n"
    "end\n"
    "inc(); inc(); inc()\n"   /* shared == 3 */
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "get");
  lua_call(L2, 0, 1);
  CHECK(lua_tointeger(L2,-1)==3, "shared=3 before inc","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  /* call inc, then get again – should update the SAME upvalue */
  get_global(L2, "inc"); lua_call(L2, 0, 0);
  get_global(L2, "get"); lua_call(L2, 0, 1);
  CHECK(lua_tointeger(L2,-1)==4, "shared=4 after inc","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 6 – long string (> LUAI_MAXSHORTLEN = 40 chars)
** --------------------------------------------------------------------- */
static void test_long_string(void) {
  printf("== test_long_string ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "long_s = 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEF_END'\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "long_s");
  const char *s = lua_tostring(L2, -1);
  CHECK(s && strstr(s,"_END") && strlen(s)==46, "long_string content",
        "got len=%zu val='%s'",(s?strlen(s):0),(s?s:"NULL"));
  lua_pop(L2, 1);
  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7 – yielded coroutine
** --------------------------------------------------------------------- */
static void test_coroutine(void) {
  printf("== test_coroutine ==\n");
  lua_State *L = new_state();
  /* create a generator coroutine, advance it twice */
  int r = run(L,
    "function gen(n)\n"
    "  for i = 1, n do\n"
    "    coroutine.yield(i)\n"
    "  end\n"
    "  return 'done'\n"
    "end\n"
    "co = coroutine.create(gen)\n"
    "ok1, v1 = coroutine.resume(co, 5)  -- yield(1)\n"
    "ok2, v2 = coroutine.resume(co)     -- yield(2)\n"
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  /* verify state before save */
  lua_getglobal(L, "v2");
  long long v2_before = lua_tointeger(L, -1);
  lua_pop(L, 1);

  lua_State *L2 = save_reload(L, NULL);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* C closures aren't serialized; re-register coroutine lib so yield works */
  lua_pushcfunction(L2, luaopen_coroutine);
  lua_call(L2, 0, 1);
  lua_setglobal(L2, "coroutine");

  /* verify v1, v2 survived */
  get_global(L2, "v1");
  CHECK(lua_tointeger(L2,-1)==1, "v1=1 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "v2");
  CHECK(lua_tointeger(L2,-1)==v2_before && v2_before==2,
        "v2=2 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  /* resume the coroutine – should yield 3 */
  get_global(L2, "co");
  CHECK(lua_type(L2,-1)==LUA_TTHREAD, "co is thread","type=%d",lua_type(L2,-1));
  lua_State *co = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  int nres = 0;
  int status = lua_resume(co, L2, 0, &nres);
  CHECK(status == LUA_YIELD || status == LUA_OK, "resume ok","status=%d",status);
  if (status == LUA_YIELD && nres >= 1) {
    long long yielded = lua_tointeger(co, -1);
    CHECK(yielded == 3, "coroutine yielded 3","got %lld",yielded);
    lua_pop(co, nres);
  }

  /* run to completion */
  while ((status = lua_resume(co, L2, 0, &nres)) == LUA_YIELD)
    lua_pop(co, nres);
  CHECK(status == LUA_OK, "coroutine finished","status=%d",status);
  if (nres >= 1) {
    CHECK(strcmp(lua_tostring(co,-1),"done")==0, "coroutine returned 'done'",
          "got '%s'",lua_tostring(co,-1));
    lua_pop(co, nres);
  }

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7b – coroutine stack restored byte-for-byte (tags + values)
** --------------------------------------------------------------------- */
#define MAX_SNAP 64

static void test_stack_restored(void) {
  printf("== test_stack_restored ==\n");
  lua_State *L = new_state();

  int r = run(L,
    "function gen(n)\n"
    "  local f = 1.5\n"
    "  local s = 'hi'\n"
    "  local b = true\n"
    "  for i = 1, n do\n"
    "    coroutine.yield(i)\n"
    "  end\n"
    "  return 'done'\n"
    "end\n"
    "co = coroutine.create(gen)\n"
    "coroutine.resume(co, 5)\n"   /* yields at i=1 */
    "coroutine.resume(co)\n"      /* yields at i=2 */
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  /* get internal lua_State of the coroutine */
  lua_getglobal(L, "co");
  lua_State *co = lua_tothread(L, -1);
  lua_pop(L, 1);

  /* snapshot before save */
  int ns = (int)(co->top.p - co->stack.p);
  int snap_n = ns < MAX_SNAP ? ns : MAX_SNAP;
  int     tags[MAX_SNAP];
  lua_Integer ivals[MAX_SNAP];
  lua_Number  nvals[MAX_SNAP];
  for (int i = 0; i < snap_n; i++) {
    TValue *v   = s2v(co->stack.p + i);
    tags[i]     = (int)rawtt(v);
    ivals[i]    = ttisinteger(v) ? ivalue(v) : 0;
    nvals[i]    = ttisfloat(v)   ? fltvalue(v) : 0;
  }

  lua_State *L2 = save_reload(L, NULL);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* re-register coroutine lib (C closures not serialized) */
  lua_pushcfunction(L2, luaopen_coroutine);
  lua_call(L2, 0, 1);
  lua_setglobal(L2, "coroutine");

  lua_getglobal(L2, "co");
  lua_State *co2 = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  int ns2 = (int)(co2->top.p - co2->stack.p);
  CHECK(ns == ns2, "stack depth preserved", "before=%d after=%d", ns, ns2);

  int check_n = ns < ns2 ? ns : ns2;
  check_n = check_n < snap_n ? check_n : snap_n;

  /* C functions (LCF, CCL) can't be serialized; they become nil.
  ** All other types (int, float, bool, nil, Lua closures, strings, tables)
  ** must be preserved exactly. */
  int tags_ok = 1, ints_ok = 1, floats_ok = 1;
  for (int i = 0; i < check_n; i++) {
    TValue *v    = s2v(co2->stack.p + i);
    int tag2     = (int)rawtt(v);
    int is_cfn   = (novariant(tags[i]) == LUA_TFUNCTION && tags[i] != ctb(LUA_VLCL));
    int exp_tag  = is_cfn ? LUA_VNIL : tags[i];
    if (tag2 != exp_tag) {
      fprintf(stdout, "  FAIL: slot[%d] type tag — before=%d expected=%d after=%d%s\n",
              i, tags[i], exp_tag, tag2,
              is_cfn ? " (C fn→nil expected)" : "");
      tags_ok = 0;
    }
    if (ttisinteger(v) && ivalue(v) != ivals[i]) {
      fprintf(stdout, "  FAIL: slot[%d] integer — before=%lld after=%lld\n",
              i, (long long)ivals[i], (long long)ivalue(v));
      ints_ok = 0;
    }
    if (ttisfloat(v) && fltvalue(v) != nvals[i]) {
      fprintf(stdout, "  FAIL: slot[%d] float — before=%g after=%g\n",
              i, (double)nvals[i], (double)fltvalue(v));
      floats_ok = 0;
    }
  }
  if (tags_ok)   PASS("all slot type tags preserved (C fns become nil)");
  if (ints_ok)   PASS("all integer slot values preserved");
  if (floats_ok) PASS("all float slot values preserved");

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 8 – multiple closures, one recursive via table
** --------------------------------------------------------------------- */
static void test_recursive_closure(void) {
  printf("== test_recursive_closure ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "function fib(n)\n"
    "  if n <= 1 then return n end\n"
    "  return fib(n-1) + fib(n-2)\n"
    "end\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "fib");
  lua_pushinteger(L2, 10);
  lua_call(L2, 1, 1);
  CHECK(lua_tointeger(L2,-1)==55, "fib(10)=55","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);
  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 9 – global environment integrity (_G basics)
** --------------------------------------------------------------------- */
static void test_globals_env(void) {
  printf("== test_globals_env ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L, "magic = 12345");
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* _G should be a table */
  lua_getglobal(L2, "_G");
  CHECK(lua_type(L2,-1)==LUA_TTABLE, "_G is table","type=%d",lua_type(L2,-1));
  lua_pop(L2, 1);

  /* can still set and get globals */
  lua_pushinteger(L2, 99);
  lua_setglobal(L2, "new_var");
  get_global(L2, "new_var");
  CHECK(lua_tointeger(L2,-1)==99, "new_var=99","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "magic");
  CHECK(lua_tointeger(L2,-1)==12345, "magic=12345","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 10 – table with mixed integer/string/float keys
** --------------------------------------------------------------------- */
static void test_mixed_keys(void) {
  printf("== test_mixed_keys ==\n");
  lua_State *L = new_state();
  lua_State *L2 = save_reload(L,
    "m = {}\n"
    "m[1]    = 'one'\n"
    "m[2]    = 'two'\n"
    "m['k']  = 3.5\n"
    "m[true] = 'yes'\n"
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "m");
  lua_rawgeti(L2, -1, 1);
  CHECK(strcmp(lua_tostring(L2,-1),"one")==0, "m[1]='one'","got '%s'",lua_tostring(L2,-1));
  lua_pop(L2, 1);

  lua_getfield(L2, -1, "k");
  CHECK(lua_tonumber(L2,-1)==3.5, "m['k']=3.5","got %g",lua_tonumber(L2,-1));
  lua_pop(L2, 1);

  lua_pushboolean(L2, 1);
  lua_rawget(L2, -2);
  CHECK(strcmp(lua_tostring(L2,-1),"yes")==0, "m[true]='yes'","got '%s'",lua_tostring(L2,-1));
  lua_pop(L2, 2);
  lua_close(L2);
}

/* -----------------------------------------------------------------------
** main
** --------------------------------------------------------------------- */
int main(void) {
  printf("=== Lua State Serializer Tests ===\n");
  test_basic();
  test_table();
  test_cycle();
  test_closure_upvalue();
  test_shared_upvalue();
  test_long_string();
  test_coroutine();
  test_stack_restored();
  test_recursive_closure();
  test_globals_env();
  test_mixed_keys();
  printf("=== Done ===\n");
  return 0;
}
