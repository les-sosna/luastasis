/*
** test_serial.c  –  Tests for the Lua state serializer.
**
** Compile & run:
**   make test_serial && ./test_serial
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lstasis.h"

/* -----------------------------------------------------------------------
** Standard library registry (used for C function serialization)
** --------------------------------------------------------------------- */
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
static lua_State *save_reload(lua_State *L, const char *setup_code,
                              const lstasis_Lib *libs) {
  size_t sz;
  unsigned char *buf;
  lua_State *L2;
  if (setup_code) {
    int r = run(L, setup_code);
    if (r != LUA_OK) return NULL;
  }
  buf = NULL;
  sz = 0;
  if (lstasis_save(L, libs, &buf, &sz) != 0) {
    fprintf(stderr, "save failed\n");
    return NULL;
  }
  L2 = lstasis_load(buf, sz, libs);
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
  lua_State *L2;
  lua_State *L;
  double fv;
  printf("== test_basic ==\n");
  L = new_state();
  L2 = save_reload(L,
    "n_int   = 42\n"
    "n_float = 3.14\n"
    "b_true  = true\n"
    "b_false = false\n"
    "s_hello = 'hello'\n"
    "n_nil   = nil\n",
    std_libs
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
  fv = lua_tonumber(L2,-1);
  CHECK(fv > 3.13 && fv < 3.15, "n_float≈3.14", "got %g", fv);
  lua_pop(L2, 1);

  get_global(L2, "b_true");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && lua_toboolean(L2,-1),
        "b_true", "type=%d val=%d",
        lua_type(L2,-1), lua_toboolean(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "b_false");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && !lua_toboolean(L2,-1),
        "b_false", "type=%d val=%d",
        lua_type(L2,-1), lua_toboolean(L2,-1));
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
  lua_State *L2;
  lua_State *L;
  printf("== test_table ==\n");
  L = new_state();
  L2 = save_reload(L,
    "arr = {10, 20, 30}\n"
    "t   = { x=1, y=2, z='three' }\n"
    "nested = { inner = { val = 99 } }\n",
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "arr");
  lua_rawgeti(L2, -1, 1);
  CHECK(lua_tointeger(L2,-1)==10, "arr[1]=10", "got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);
  lua_rawgeti(L2, -1, 3);
  CHECK(lua_tointeger(L2,-1)==30, "arr[3]=30", "got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 2);

  get_global(L2, "t");
  lua_getfield(L2, -1, "z");
  CHECK(strcmp(lua_tostring(L2,-1),"three")==0, "t.z='three'", "got '%s'",lua_tostring(L2,-1));
  lua_pop(L2, 2);

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
  lua_State *L2;
  lua_State *L;
  printf("== test_cycle ==\n");
  L = new_state();
  L2 = save_reload(L,
    "t = {}\n"
    "t.self = t\n"
    "t.val  = 777\n",
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "t");
  lua_getfield(L2, -1, "val");
  CHECK(lua_tointeger(L2,-1)==777, "t.val=777","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);
  lua_getfield(L2, -1, "self");
  CHECK(lua_rawequal(L2,-1,-2), "t.self==t", "not same table");
  lua_pop(L2, 2);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 4 – closure with upvalue
** --------------------------------------------------------------------- */
static void test_closure_upvalue(void) {
  lua_State *L2;
  lua_State *L;
  printf("== test_closure_upvalue ==\n");
  L = new_state();
  L2 = save_reload(L,
    "function make_counter(init)\n"
    "  local c = init\n"
    "  return function() c = c + 1; return c end\n"
    "end\n"
    "counter = make_counter(10)\n"
    "counter()  -- c=11\n"
    "counter()  -- c=12\n",
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "counter");
  CHECK(lua_type(L2,-1)==LUA_TFUNCTION, "counter is function","type=%d",lua_type(L2,-1));
  lua_call(L2, 0, 1);
  CHECK(lua_tointeger(L2,-1)==13, "counter()=13 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 5 – shared upvalue between two closures
** --------------------------------------------------------------------- */
static void test_shared_upvalue(void) {
  lua_State *L2;
  lua_State *L;
  printf("== test_shared_upvalue ==\n");
  L = new_state();
  L2 = save_reload(L,
    "do\n"
    "  local shared = 0\n"
    "  inc = function() shared = shared + 1 end\n"
    "  get = function() return shared end\n"
    "end\n"
    "inc(); inc(); inc()\n",
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "get");
  lua_call(L2, 0, 1);
  CHECK(lua_tointeger(L2,-1)==3, "shared=3 before inc","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

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
  lua_State *L2;
  lua_State *L;
  const char *s;
  printf("== test_long_string ==\n");
  L = new_state();
  L2 = save_reload(L,
    "long_s = 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEF_END'\n",
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "long_s");
  s = lua_tostring(L2, -1);
  CHECK(s && strstr(s,"_END") && strlen(s)==46, "long_string content",
        "got len=%zu val='%s'",(s?strlen(s):0),(s?s:"NULL"));
  lua_pop(L2, 1);
  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7 – yielded coroutine
** --------------------------------------------------------------------- */
static void test_coroutine(void) {
  int r;
  int status;
  lua_State *L;
  long long v2_before;
  lua_State *L2;
  lua_State *co;
  int nres;
  printf("== test_coroutine ==\n");
  L = new_state();
  r = run(L,
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

  lua_getglobal(L, "v2");
  v2_before = lua_tointeger(L, -1);
  lua_pop(L, 1);

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  get_global(L2, "v1");
  CHECK(lua_tointeger(L2,-1)==1, "v1=1 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "v2");
  CHECK(lua_tointeger(L2,-1)==v2_before && v2_before==2,
        "v2=2 after reload","got %lld",(long long)lua_tointeger(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "co");
  CHECK(lua_type(L2,-1)==LUA_TTHREAD, "co is thread","type=%d",lua_type(L2,-1));
  co = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  nres = 0;
  status = lua_resume(co, L2, 0, &nres);
  CHECK(status == LUA_YIELD || status == LUA_OK, "resume ok","status=%d",status);
  if (status == LUA_YIELD && nres >= 1) {
    long long yielded = lua_tointeger(co, -1);
    CHECK(yielded == 3, "coroutine yielded 3","got %lld",yielded);
    lua_pop(co, nres);
  }

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
** Test 7b – coroutine locals survive save/reload
** The coroutine yields its local variables so we can verify them via the
** public API without touching internal lua_State fields.
** --------------------------------------------------------------------- */
static void test_stack_restored(void) {
  int r;
  int status;
  lua_State *L;
  lua_State *L2;
  lua_State *co2;
  int nres;
  printf("== test_stack_restored ==\n");
  L = new_state();

  /* gen yields (i, f, s, b) each iteration and returns them at the end. */
  r = run(L,
    "function gen(n)\n"
    "  local f = 1.5\n"
    "  local s = 'hi'\n"
    "  local b = true\n"
    "  for i = 1, n do\n"
    "    coroutine.yield(i, f, s, b)\n"
    "  end\n"
    "  return 'done', f, s, b\n"
    "end\n"
    "co = coroutine.create(gen)\n"
    "coroutine.resume(co, 3)\n"   /* suspends after yield(1,...) */
    "coroutine.resume(co)\n"      /* suspends after yield(2,...) */
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  lua_getglobal(L2, "co");
  co2 = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  /* Resume: should yield (3, 1.5, 'hi', true) — verifies locals intact. */
  nres = 0;
  status = lua_resume(co2, L2, 0, &nres);
  CHECK(status == LUA_YIELD, "stack depth preserved", "status=%d", status);
  if (status == LUA_YIELD && nres == 4) {
    long long counter = lua_tointeger(co2, -4);
    double    fval    = lua_tonumber(co2, -3);
    int       isfl    = !lua_isinteger(co2, -3);
    const char *sval  = lua_tostring(co2, -2);
    int       bval    = lua_toboolean(co2, -1);
    CHECK(counter == 3 && isfl && fval == 1.5, "all float slot values preserved",
          "counter=%lld f=%g isfl=%d", counter, fval, isfl);
    CHECK(sval && strcmp(sval, "hi") == 0,    "all slot type tags preserved",
          "s='%s'", sval ? sval : "NULL");
    CHECK(bval,                                "all integer slot values preserved",
          "b=%d", bval);
  }
  lua_pop(co2, nres);

  /* Drain remaining iterations and verify final return. */
  while ((status = lua_resume(co2, L2, 0, &nres)) == LUA_YIELD)
    lua_pop(co2, nres);
  CHECK(status == LUA_OK, "coroutine finished", "status=%d", status);
  if (status == LUA_OK && nres >= 4) {
    CHECK(strcmp(lua_tostring(co2, -nres), "done") == 0,
          "coroutine returned done", "got '%s'", lua_tostring(co2, -nres));
  }
  lua_pop(co2, nres);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 8 – multiple closures, one recursive via table
** --------------------------------------------------------------------- */
static void test_recursive_closure(void) {
  lua_State *L2;
  lua_State *L;
  printf("== test_recursive_closure ==\n");
  L = new_state();
  L2 = save_reload(L,
    "function fib(n)\n"
    "  if n <= 1 then return n end\n"
    "  return fib(n-1) + fib(n-2)\n"
    "end\n",
    std_libs
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
  lua_State *L2;
  lua_State *L;
  printf("== test_globals_env ==\n");
  L = new_state();
  L2 = save_reload(L, "magic = 12345", std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  lua_getglobal(L2, "_G");
  CHECK(lua_type(L2,-1)==LUA_TTABLE, "_G is table","type=%d",lua_type(L2,-1));
  lua_pop(L2, 1);

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
  lua_State *L2;
  lua_State *L;
  printf("== test_mixed_keys ==\n");
  L = new_state();
  L2 = save_reload(L,
    "m = {}\n"
    "m[1]    = 'one'\n"
    "m[2]    = 'two'\n"
    "m['k']  = 3.5\n"
    "m[true] = 'yes'\n",
    std_libs
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
** Test 11 – C function serialization
** C functions stored as globals / table values survive round-trip and
** remain callable.
** --------------------------------------------------------------------- */
static void test_cfunc(void) {
  int r;
  lua_State *L;
  lua_State *L2;
  printf("== test_cfunc ==\n");
  L = new_state();

  /* Save table.sort as a global; use table.concat to build a string. */
  r = run(L,
    "local t = {3, 1, 4, 1, 5, 9, 2, 6}\n"
    "sort_fn = table.sort\n"
    "sort_fn(t)\n"
    "sorted_str = table.concat(t, ',')\n"
    "concat_fn = table.concat\n"
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* sorted_str should have been computed before save */
  get_global(L2, "sorted_str");
  CHECK(lua_type(L2,-1) == LUA_TSTRING, "sorted_str is string",
        "type=%d", lua_type(L2,-1));
  CHECK(strcmp(lua_tostring(L2,-1), "1,1,2,3,4,5,6,9") == 0,
        "sorted_str='1,1,2,3,4,5,6,9'", "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);

  /* sort_fn should be restored as a callable C function */
  get_global(L2, "sort_fn");
  CHECK(lua_type(L2,-1) == LUA_TFUNCTION, "sort_fn is function",
        "type=%d", lua_type(L2,-1));
  lua_pop(L2, 1);

  /* concat_fn should also be restored and callable */
  get_global(L2, "concat_fn");
  CHECK(lua_type(L2,-1) == LUA_TFUNCTION, "concat_fn is function",
        "type=%d", lua_type(L2,-1));
  /* call concat_fn({10,20,30}, '-') */
  lua_newtable(L2);
  lua_pushinteger(L2, 10); lua_rawseti(L2, -2, 1);
  lua_pushinteger(L2, 20); lua_rawseti(L2, -2, 2);
  lua_pushinteger(L2, 30); lua_rawseti(L2, -2, 3);
  lua_pushstring(L2, "-");
  lua_call(L2, 2, 1);
  CHECK(strcmp(lua_tostring(L2,-1), "10-20-30") == 0,
        "concat_fn({10,20,30},'-')='10-20-30'",
        "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);

  /* Coroutine functions should work without manual re-registration */
  r = luaL_dostring(L2,
    "local co = coroutine.create(function()\n"
    "  coroutine.yield(42)\n"
    "  coroutine.yield(99)\n"
    "end)\n"
    "local ok, a = coroutine.resume(co)\n"
    "local ok2, b = coroutine.resume(co)\n"
    "cfunc_co_a = a\n"
    "cfunc_co_b = b\n"
  );
  CHECK(r == LUA_OK, "coroutine code runs after reload",
        "error: %s", lua_tostring(L2, -1));
  if (r == LUA_OK) {
    get_global(L2, "cfunc_co_a");
    CHECK(lua_tointeger(L2,-1)==42, "first yield=42",
          "got %lld",(long long)lua_tointeger(L2,-1));
    lua_pop(L2, 1);
    get_global(L2, "cfunc_co_b");
    CHECK(lua_tointeger(L2,-1)==99, "second yield=99",
          "got %lld",(long long)lua_tointeger(L2,-1));
    lua_pop(L2, 1);
  }

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 12 – vararg functions survive round-trip
** Exercises both PF_VAHID (hidden vararg, any use of ...) and PF_VATAB
** (vararg table, triggered by {...} construction) which are separate bits
** in Lua 5.5's proto flag byte.
** --------------------------------------------------------------------- */
static void test_vararg(void) {
  lua_State *L2;
  int st;
  lua_State *L;
  lua_State *vco;
  int nres;
  printf("== test_vararg ==\n");
  L = new_state();
  L2 = save_reload(L,
    /* PF_VAHID: plain ... passthrough */
    "function identity(...) return ... end\n"
    /* PF_VAHID: select on ... */
    "function count(...) return select('#', ...) end\n"
    /* PF_VAHID | PF_VATAB: {...} forces a vararg table */
    "function sum(...)\n"
    "  local t = {...}\n"
    "  local s = 0\n"
    "  for i = 1, #t do s = s + t[i] end\n"
    "  return s\n"
    "end\n"
    /* vararg coroutine: tests vararg + yield interaction */
    "function vararg_gen(...)\n"
    "  local t = {...}\n"
    "  for i = 1, #t do coroutine.yield(t[i]) end\n"
    "end\n"
    "vco = coroutine.create(vararg_gen)\n"
    "coroutine.resume(vco, 10, 20, 30)\n",  /* suspends after yield(10) */
    std_libs
  );
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL"); return; }

  /* identity('a','b') → 'a','b' */
  get_global(L2, "identity");
  lua_pushstring(L2, "a"); lua_pushstring(L2, "b");
  lua_call(L2, 2, 2);
  CHECK(strcmp(lua_tostring(L2, -2), "a") == 0, "identity ret[1]='a'",
        "got '%s'", lua_tostring(L2, -2));
  CHECK(strcmp(lua_tostring(L2, -1), "b") == 0, "identity ret[2]='b'",
        "got '%s'", lua_tostring(L2, -1));
  lua_pop(L2, 2);

  /* count(10,20,30) → 3 */
  get_global(L2, "count");
  lua_pushinteger(L2, 10); lua_pushinteger(L2, 20); lua_pushinteger(L2, 30);
  lua_call(L2, 3, 1);
  CHECK(lua_tointeger(L2, -1) == 3, "count(10,20,30)=3",
        "got %lld", (long long)lua_tointeger(L2, -1));
  lua_pop(L2, 1);

  /* sum(1,2,3,4,5) → 15  (uses {...}, exercises PF_VATAB) */
  get_global(L2, "sum");
  lua_pushinteger(L2, 1); lua_pushinteger(L2, 2); lua_pushinteger(L2, 3);
  lua_pushinteger(L2, 4); lua_pushinteger(L2, 5);
  lua_call(L2, 5, 1);
  CHECK(lua_tointeger(L2, -1) == 15, "sum(1..5)=15",
        "got %lld", (long long)lua_tointeger(L2, -1));
  lua_pop(L2, 1);

  /* vco was suspended after yielding 10; resume → 20, then 30, then done */
  get_global(L2, "vco");
  vco = lua_tothread(L2, -1);
  lua_pop(L2, 1);
  nres = 0;
  st = lua_resume(vco, L2, 0, &nres);
  CHECK(st == LUA_YIELD && nres == 1 && lua_tointeger(vco, -1) == 20,
        "vararg coroutine yields 20", "st=%d v=%lld",
        st, (long long)(nres ? lua_tointeger(vco, -1) : -1));
  lua_pop(vco, nres);
  st = lua_resume(vco, L2, 0, &nres);
  CHECK(st == LUA_YIELD && nres == 1 && lua_tointeger(vco, -1) == 30,
        "vararg coroutine yields 30", "st=%d v=%lld",
        st, (long long)(nres ? lua_tointeger(vco, -1) : -1));
  lua_pop(vco, nres);
  st = lua_resume(vco, L2, 0, &nres);
  CHECK(st == LUA_OK, "vararg coroutine finishes", "st=%d", st);
  lua_pop(vco, nres);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 13 – lstasis_save must not modify the caller's lua_State
** If openers were called on L instead of a side state, they would overwrite
** standard globals (_G.package, require, etc.) and clobber custom ones.
** --------------------------------------------------------------------- */
static void test_save_preserves_state(void) {
  int rc;
  size_t sz;
  lua_State *L;
  unsigned char *buf;
  printf("== test_save_preserves_state ==\n");
  L = new_state();

  /* Install a sentinel global and override require with a custom function. */
  run(L,
    "my_sentinel = 99999\n"
    "my_table    = { key = 'preserved' }\n"
    "require = function(m) return 'intercepted:' .. m end\n"
  );

  /* Verify our custom require works before the save. */
  lua_getglobal(L, "require");
  lua_pushstring(L, "test");
  lua_call(L, 1, 1);
  CHECK(strcmp(lua_tostring(L, -1), "intercepted:test") == 0,
        "custom require works before save",
        "got '%s'", lua_tostring(L, -1));
  lua_pop(L, 1);

  /* Perform the save. */
  buf = NULL;
  sz = 0;
  rc = lstasis_save(L, std_libs, &buf, &sz);
  free(buf);

  CHECK(rc == 0, "save succeeds", "rc=%d", rc);

  /* Sentinel must still be intact. */
  lua_getglobal(L, "my_sentinel");
  CHECK(lua_tointeger(L, -1) == 99999,
        "my_sentinel unchanged after save",
        "got %lld", (long long)lua_tointeger(L, -1));
  lua_pop(L, 1);

  /* Custom table must still be intact. */
  lua_getglobal(L, "my_table");
  lua_getfield(L, -1, "key");
  CHECK(lua_type(L, -1) == LUA_TSTRING &&
        strcmp(lua_tostring(L, -1), "preserved") == 0,
        "my_table.key unchanged after save",
        "got '%s'", lua_tostring(L, -1));
  lua_pop(L, 2);

  /* Custom require must still be our override, not the original. */
  lua_getglobal(L, "require");
  lua_pushstring(L, "foo");
  lua_call(L, 1, 1);
  CHECK(strcmp(lua_tostring(L, -1), "intercepted:foo") == 0,
        "custom require not overwritten by save",
        "got '%s'", lua_tostring(L, -1));
  lua_pop(L, 1);

  lua_close(L);
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
  test_cfunc();
  test_vararg();
  test_save_preserves_state();
  printf("=== Done ===\n");
  return 0;
}
