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
** Test 7a – coroutine yielded from NESTED Lua frames survives save/reload
**
** Suspends a coroutine mid-yield with several Lua CallInfos live (outer ->
** middle -> inner), so the thread record round-trips multiple call frames
** (each carrying the transient u2 placeholder). After reload the coroutine
** must resume through every frame and return the correct final value.
** --------------------------------------------------------------------- */
static void test_coroutine_nested_yield(void) {
  int r, status, nres;
  lua_State *L, *L2, *co;
  long long y1;
  printf("== test_coroutine_nested_yield ==\n");
  L = new_state();
  r = run(L,
    "function inner(x)\n"
    "  coroutine.yield(x + 1)\n"     /* suspend deep in a Lua call chain */
    "  return x + 100\n"
    "end\n"
    "function middle(x) return inner(x) + 10 end\n"
    "function outer()  return middle(5) + 1000 end\n"
    "co = coroutine.create(outer)\n"
    "ok1, y1 = coroutine.resume(co)\n"   /* runs outer->middle->inner; yields 6 */
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  get_global(L, "y1");
  y1 = lua_tointeger(L, -1);
  lua_pop(L, 1);
  CHECK(y1 == 6, "yielded 6 before save", "got %lld", y1);

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL — nested yield should be serializable"); return; }

  get_global(L2, "co");
  CHECK(lua_type(L2,-1)==LUA_TTHREAD, "co is thread after reload", "type=%d", lua_type(L2,-1));
  co = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  /* Resume: inner returns 105, middle adds 10 -> 115, outer adds 1000 -> 1115. */
  nres = 0;
  status = lua_resume(co, L2, 0, &nres);
  CHECK(status == LUA_OK, "restored nested coroutine finished", "status=%d", status);
  if (status == LUA_OK && nres >= 1) {
    CHECK(lua_tointeger(co,-1) == 1115, "coroutine returned 1115 through all frames",
          "got %lld", (long long)lua_tointeger(co,-1));
  }
  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7b – coroutine yielded INSIDE pcall round-trips and recovers
**
** When a coroutine yields across pcall, the pcall C frame is suspended with
** the built-in 'finishpcall' continuation (CIST_YPCALL) and ci->u2.funcidx /
** ci->u.c.{ctx,old_errfunc} live. lstasis recognizes that continuation by
** name and restores those fields, so the reloaded coroutine resumes — and on
** the error raised after the yield, pcall recovers via finishpcallk (which
** reads exactly those restored fields).
** --------------------------------------------------------------------- */
static void test_coroutine_pcall_yield(void) {
  int r, status, nres;
  lua_State *L, *L2, *co;
  printf("== test_coroutine_pcall_yield ==\n");
  L = new_state();
  r = run(L,
    "function inner()\n"
    "  coroutine.yield('paused')\n"   /* suspend INSIDE pcall: CIST_YPCALL, funcidx live */
    "  error('boom')\n"               /* on resume: pcall recovers via finishpcallk */
    "end\n"
    "function body()\n"
    "  local ok, msg = pcall(inner)\n"
    "  pcall_ok = ok\n"
    "  pcall_msg = msg\n"
    "  return 'finished'\n"
    "end\n"
    "co = coroutine.create(body)\n"
    "ok0, y0 = coroutine.resume(co)\n" /* runs to yield('paused'); co suspended inside pcall */
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }

  get_global(L, "y0");
  CHECK(lua_type(L,-1)==LUA_TSTRING && strcmp(lua_tostring(L,-1),"paused")==0,
        "yielded 'paused' before save", "got '%s'", lua_tostring(L,-1));
  lua_pop(L, 1);

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL — pcall-yield should now be serializable"); return; }

  get_global(L2, "co");
  CHECK(lua_type(L2,-1)==LUA_TTHREAD, "co is thread after reload", "type=%d", lua_type(L2,-1));
  co = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  nres = 0;
  status = lua_resume(co, L2, 0, &nres);
  CHECK(status == LUA_OK, "restored coroutine finished cleanly", "status=%d", status);
  if (status == LUA_OK && nres >= 1) {
    CHECK(lua_type(co,-1)==LUA_TSTRING && strcmp(lua_tostring(co,-1),"finished")==0,
          "coroutine returned 'finished'", "got '%s'", lua_tostring(co,-1));
  }

  get_global(L2, "pcall_ok");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && lua_toboolean(L2,-1)==0,
        "pcall recovered the error after reload (ok=false)", "type=%d", lua_type(L2,-1));
  lua_pop(L2, 1);

  get_global(L2, "pcall_msg");
  CHECK(lua_type(L2,-1)==LUA_TSTRING && strstr(lua_tostring(L2,-1),"boom")!=NULL,
        "pcall error message preserved", "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7b2 – coroutine suspended in a USER (non-builtin) continuation is
** rejected. user_callk() calls its argument through lua_callk with its own
** continuation; when that argument yields, the user_callk frame is left with
** a continuation lstasis does not recognize, so the save must refuse cleanly.
** --------------------------------------------------------------------- */
static int usercont_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  return lua_gettop(L);
}
static int user_callk (lua_State *L) {
  lua_callk(L, 0, LUA_MULTRET, 0, usercont_k);  /* yieldable, user continuation */
  return usercont_k(L, LUA_OK, 0);
}
static const luaL_Reg usertest_funcs[] = {
  {"user_callk", user_callk}, {NULL, NULL}
};
static int luaopen_usertest (lua_State *L) { luaL_newlib(L, usertest_funcs); return 1; }

static void test_coroutine_usercont_rejected(void) {
  static const lstasis_Lib user_libs[] = {
    {"base", luaopen_base}, {"package", luaopen_package}, {"coroutine", luaopen_coroutine},
    {"table", luaopen_table}, {"string", luaopen_string}, {"usertest", luaopen_usertest},
    {NULL, NULL}
  };
  int r, rc;
  size_t sz = 0;
  unsigned char *buf = NULL;
  lua_State *L;
  printf("== test_coroutine_usercont_rejected ==\n");
  L = new_state();
  luaL_requiref(L, "usertest", luaopen_usertest, 1);
  lua_pop(L, 1);
  r = run(L,
    "co = coroutine.create(function()\n"
    "  usertest.user_callk(function() coroutine.yield('x') end)\n"
    "end)\n"
    "coroutine.resume(co)\n"   /* suspended in user_callk's lua_callk: user continuation */
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); lua_close(L); return; }

  rc = lstasis_save(L, user_libs, &buf, &sz);
  CHECK(rc != 0, "save rejects coroutine suspended in a user C continuation", "rc=%d", rc);
  free(buf);
  lua_close(L);
}

/* -----------------------------------------------------------------------
** Test 7b3 – several stacked builtin continuations survive save/reload
**
** A single yield deep inside pcall(level1) -> level1 -> pcall(level2) leaves
** TWO C frames suspended at once, each with its own finishpcall continuation
** (and its own ctx/old_errfunc/funcidx). Every CallInfo is serialized
** independently, so both must restore and unwind correctly on resume.
** --------------------------------------------------------------------- */
static void test_coroutine_stacked_pcall_yield(void) {
  int r, status, nres;
  lua_State *L, *L2, *co;
  printf("== test_coroutine_stacked_pcall_yield ==\n");
  L = new_state();
  r = run(L,
    "function level2()\n"
    "  coroutine.yield('deep')\n"   /* one yield, two pcalls live below it */
    "  error('boom')\n"
    "end\n"
    "function level1()\n"
    "  inner_ok, inner_msg = pcall(level2)\n"  /* inner pcall */
    "  return 'L1DONE'\n"
    "end\n"
    "function body()\n"
    "  outer_ok, outer_r = pcall(level1)\n"    /* outer pcall */
    "  return 'finished'\n"
    "end\n"
    "co = coroutine.create(body)\n"
    "ok0, y0 = coroutine.resume(co)\n"
  );
  if (r != LUA_OK) { FAIL("setup", "lua error"); return; }
  get_global(L, "y0");
  CHECK(lua_type(L,-1)==LUA_TSTRING && strcmp(lua_tostring(L,-1),"deep")==0,
        "yielded 'deep' before save", "got '%s'", lua_tostring(L,-1));
  lua_pop(L, 1);

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "NULL — stacked pcalls should be serializable"); return; }

  get_global(L2, "co");
  co = lua_tothread(L2, -1);
  lua_pop(L2, 1);

  nres = 0;
  status = lua_resume(co, L2, 0, &nres);
  CHECK(status == LUA_OK, "restored coroutine finished cleanly", "status=%d", status);
  if (status == LUA_OK && nres >= 1)
    CHECK(lua_type(co,-1)==LUA_TSTRING && strcmp(lua_tostring(co,-1),"finished")==0,
          "coroutine returned 'finished'", "got '%s'", lua_tostring(co,-1));

  /* inner pcall caught the error; outer pcall saw level1 return normally. */
  get_global(L2, "inner_ok");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && lua_toboolean(L2,-1)==0,
        "inner pcall caught error (inner_ok=false)", "type=%d", lua_type(L2,-1));
  lua_pop(L2, 1);
  get_global(L2, "inner_msg");
  CHECK(lua_type(L2,-1)==LUA_TSTRING && strstr(lua_tostring(L2,-1),"boom")!=NULL,
        "inner error message preserved", "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);
  get_global(L2, "outer_ok");
  CHECK(lua_type(L2,-1)==LUA_TBOOLEAN && lua_toboolean(L2,-1)==1,
        "outer pcall succeeded (outer_ok=true)", "type=%d", lua_type(L2,-1));
  lua_pop(L2, 1);
  get_global(L2, "outer_r");
  CHECK(lua_type(L2,-1)==LUA_TSTRING && strcmp(lua_tostring(L2,-1),"L1DONE")==0,
        "outer pcall got level1's return", "got '%s'", lua_tostring(L2,-1));
  lua_pop(L2, 1);

  lua_close(L2);
}

/* -----------------------------------------------------------------------
** Test 7c – coroutine locals survive save/reload
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
** Test 14 – full userdata serialization via a __persist metatable
**
** A userdata opts into persistence by a truthy __persist field on its
** metatable. lstasis serializes it self-contained — raw payload bytes plus its
** metatable (an ordinary serialized object) — and reconstructs it from the
** buffer alone. This mirrors how a host application persists an opaque POD
** handle whose metatable carries __persist.
** --------------------------------------------------------------------- */

#define TEST_UD_TNAME "test.ud"

/* Build the persistable metatable once per state, then create a userdata
** holding a single int payload and leave it on the stack at -1. luaL_newmetatable
** auto-sets the metatable's __name to "test.ud"; we add __persist = true. */
static void make_persistable_ud(lua_State *L, int payload) {
  int *p;
  if (luaL_newmetatable(L, TEST_UD_TNAME)) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__persist");
  }
  lua_pop(L, 1);  /* drop the metatable; it stays anchored in the registry */
  p = (int *)lua_newuserdatauv(L, sizeof(int), 0);
  *p = payload;
  luaL_setmetatable(L, TEST_UD_TNAME);
}

/* Helper: does _G[name] hold our persistable userdata with the given int?
** Reads the raw payload directly so it does not depend on metatable identity. */
static int global_ud_int(lua_State *L, const char *name, int *out) {
  int ok = 0;
  lua_getglobal(L, name);
  if (lua_type(L, -1) == LUA_TUSERDATA) {
    *out = *(int *)lua_touserdata(L, -1);
    ok = 1;
  }
  lua_pop(L, 1);
  return ok;
}

static void test_userdata(void) {
  lua_State *L;
  lua_State *L2;
  int val = 0;
  printf("== test_userdata ==\n");
  L = new_state();

  make_persistable_ud(L, 0xBEEF);
  /* Stash the SAME userdata in two globals and a table entry to test that
  ** identity (pointer equality) is preserved across save/load. */
  lua_pushvalue(L, -1); lua_setglobal(L, "ud_a");
  lua_pushvalue(L, -1); lua_setglobal(L, "ud_b");
  lua_newtable(L);
  lua_pushvalue(L, -2); lua_rawseti(L, -2, 1);
  lua_setglobal(L, "ud_t");
  lua_pop(L, 1);  /* drop the original userdata */

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "load returned NULL"); return; }

  /* Payload survived. */
  CHECK(global_ud_int(L2, "ud_a", &val) && val == 0xBEEF,
        "userdata payload round-trips", "got 0x%X", val);

  /* Metatable reattached → resolves as test.ud (verifies the mt object and its
  ** __name round-tripped and the udata is relinked to it). */
  lua_getglobal(L2, "ud_a");
  CHECK(luaL_testudata(L2, -1, TEST_UD_TNAME) != NULL,
        "userdata metatable round-trips", "no/foreign metatable");

  /* Identity: ud_a, ud_b and ud_t[1] must be the SAME object. */
  lua_getglobal(L2, "ud_b");
  CHECK(lua_rawequal(L2, -1, -2), "shared references keep identity (ud_a==ud_b)",
        "distinct objects");
  lua_getglobal(L2, "ud_t");
  lua_rawgeti(L2, -1, 1);
  /* stack: ud_a(-4), ud_b(-3), ud_t(-2), ud_t[1](-1) — compare against ud_a. */
  CHECK(lua_rawequal(L2, -1, -4), "table entry shares identity (ud_a==ud_t[1])",
        "distinct objects");
  lua_pop(L2, 4);

  lua_close(L2);
}

/* A full userdata WITHOUT __persist must still serialize as nil (historical
** behavior). */
static void test_userdata_non_persistable(void) {
  lua_State *L;
  lua_State *L2;
  printf("== test_userdata_non_persistable ==\n");
  L = new_state();

  /* Plain userdata, no metatable / no __persist. */
  lua_newuserdatauv(L, sizeof(int), 0);
  lua_setglobal(L, "plain_ud");

  L2 = save_reload(L, NULL, std_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "load returned NULL"); return; }

  lua_getglobal(L2, "plain_ud");
  CHECK(lua_type(L2, -1) == LUA_TNIL,
        "non-persistable userdata becomes nil", "type=%d", lua_type(L2, -1));
  lua_pop(L2, 1);
  lua_close(L2);
}

/* A persistable userdata carrying Lua user values (nuvalue > 0) is rejected
** at save time rather than silently dropping the user values. */
static void test_userdata_uservalue_rejected(void) {
  size_t sz;
  unsigned char *buf;
  int rc;
  lua_State *L;
  int *p;
  printf("== test_userdata_uservalue_rejected ==\n");
  L = new_state();
  if (luaL_newmetatable(L, TEST_UD_TNAME)) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__persist");
  }
  lua_pop(L, 1);
  /* userdata with one user value */
  p = (int *)lua_newuserdatauv(L, sizeof(int), 1);
  *p = 5;
  lua_pushstring(L, "uv");
  lua_setiuservalue(L, -2, 1);
  luaL_setmetatable(L, TEST_UD_TNAME);
  lua_setglobal(L, "ud_uv");

  buf = NULL; sz = 0;
  rc = lstasis_save(L, std_libs, &buf, &sz);
  CHECK(rc != 0, "save rejects persistable userdata with user values",
        "rc=%d", rc);
  free(buf);
  lua_close(L);
}

/* save -> load -> save must be byte-identical with a persistable userdata in
** the state (the determinism requirement, exercised with userdata).
**
** This builds a base-library-only state on purpose: the io library's standard
** file handles (io.stdin/stdout/stderr) are NON-persistable userdata that
** serialize to nil, and on reload fill_table drops those nil-valued slots,
** unanchoring their key strings so the post-load GC sweeps them — making a
** second save a few strings shorter. That pre-existing nil-drop effect is
** unrelated to __persist userdata; excluding io isolates this test to the
** feature under test. */
static const lstasis_Lib base_libs[] = {
  {"base", luaopen_base},
  {NULL, NULL}
};

/* Gated to deterministic mode: byte-identical re-serialization is only a
** LUASTASIS_DETERMINISTIC guarantee. In vanilla mode objects hash by address,
** so two distinct states may lay their tables out differently and re-save to
** different bytes (observable e.g. under ASAN, which shifts allocations). */
#if LUASTASIS_DETERMINISTIC
static void test_userdata_byte_stable(void) {
  size_t n1, n2;
  unsigned char *s1, *s2;
  lua_State *L;
  lua_State *L2;
  int ok;
  printf("== test_userdata_byte_stable ==\n");
  L = luaL_newstate();
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  make_persistable_ud(L, 0x1234);
  lua_pushvalue(L, -1); lua_setglobal(L, "ud_a");
  lua_setglobal(L, "ud_b");  /* same object in two globals */

  s1 = NULL; n1 = 0;
  if (lstasis_save(L, base_libs, &s1, &n1) != 0) {
    FAIL("save1", "first save failed"); lua_close(L); return;
  }
  L2 = lstasis_load(s1, n1, base_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "load returned NULL"); free(s1); return; }

  s2 = NULL; n2 = 0;
  if (lstasis_save(L2, base_libs, &s2, &n2) != 0) {
    FAIL("save2", "second save failed"); free(s1); lua_close(L2); return;
  }
  ok = (n1 == n2) && (memcmp(s1, s2, n1) == 0);
  CHECK(ok, "save->load->save byte-identical with userdata",
        "n1=%zu n2=%zu", n1, n2);
  free(s1); free(s2);
  lua_close(L2);
}
#endif /* LUASTASIS_DETERMINISTIC */

/* A persistable userdata whose metatable defines __gc must run that finalizer
** after load, exactly as a freshly-created one would. The loader relinks
** u->metatable with a raw assignment, bypassing lua_setmetatable's
** luaC_checkfinalizer; Pass 2h re-registers the finalizer so it still fires.
** The finalizer is a plain Lua function (a serializable closure) that bumps a
** global counter; we drop the only reference post-load and force a collection. */
static void test_userdata_gc_finalizer(void) {
  lua_State *L;
  lua_State *L2;
  int ran;
  printf("== test_userdata_gc_finalizer ==\n");
  L = luaL_newstate();
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);

  /* metatable: __persist=true, __gc = function() gc_ran = (gc_ran or 0) + 1 end */
  luaL_newmetatable(L, "gc.ud");
  lua_pushboolean(L, 1); lua_setfield(L, -2, "__persist");
  if (luaL_loadstring(L, "gc_ran = (gc_ran or 0) + 1") != LUA_OK) {
    FAIL("loadstring", "could not compile __gc body"); lua_close(L); return;
  }
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);  /* drop metatable; stays anchored in the registry */

  lua_newuserdatauv(L, sizeof(int), 0);
  luaL_setmetatable(L, "gc.ud");
  lua_setglobal(L, "u");

  L2 = save_reload(L, NULL, base_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "load returned NULL"); return; }

  /* gc_ran was nil in the saved state; finalizing the loaded userdata sets it. */
  lua_pushnil(L2); lua_setglobal(L2, "u");
  lua_gc(L2, LUA_GCCOLLECT, 0);

  lua_getglobal(L2, "gc_ran");
  ran = (int)lua_tointeger(L2, -1);
  CHECK(lua_isinteger(L2, -1) && ran == 1,
        "restored userdata runs its __gc finalizer once", "gc_ran=%d", ran);
  lua_pop(L2, 1);
  lua_close(L2);
}

/* The loader rebuilds tables by writing hash nodes directly, which leaves each
** table's fast tag-method cache (Table.flags) stale (luaH_new marks every fast
** metamethod absent). Pass 2h invalidates it so metamethod dispatch keeps
** working on loaded metatables — without it, t.missing skips __index. */
static void test_loaded_metatable_index(void) {
  lua_State *L;
  lua_State *L2;
  const char *res;
  printf("== test_loaded_metatable_index ==\n");
  L = luaL_newstate();
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  if (run(L, "t = setmetatable({}, "
              "{ __index = function(_, k) return 'IDX:' .. k end })") != LUA_OK) {
    FAIL("setup", "%s", lua_tostring(L, -1)); lua_close(L); return;
  }

  L2 = save_reload(L, NULL, base_libs);
  lua_close(L);
  if (!L2) { FAIL("reload", "load returned NULL"); return; }

  if (run(L2, "probe = t.missing") != LUA_OK) {
    FAIL("index", "%s", lua_tostring(L2, -1)); lua_close(L2); return;
  }
  lua_getglobal(L2, "probe");
  res = lua_tostring(L2, -1);
  CHECK(res != NULL && strcmp(res, "IDX:missing") == 0,
        "loaded metatable still dispatches __index", "got %s", res ? res : "(nil)");
  lua_pop(L2, 1);
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
  test_coroutine_nested_yield();
  test_coroutine_pcall_yield();
  test_coroutine_stacked_pcall_yield();
  test_coroutine_usercont_rejected();
  test_stack_restored();
  test_recursive_closure();
  test_globals_env();
  test_mixed_keys();
  test_cfunc();
  test_vararg();
  test_save_preserves_state();
  test_userdata();
  test_userdata_non_persistable();
  test_userdata_uservalue_rejected();
  test_userdata_gc_finalizer();
  test_loaded_metatable_index();
#if LUASTASIS_DETERMINISTIC
  test_userdata_byte_stable();
#endif
  printf("=== Done ===\n");
  return 0;
}
