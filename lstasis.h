/*
** lstasis.h
** LuaStasis: full lua_State serialization into/from a raw byte buffer.
** Invoke only when no Lua code is running.
**
** Supports: nil, bool, integer, float, string, table, Lua closures,
**           upvalues (open + closed), prototypes, coroutines,
**           C functions (LCF and C closures) via a name registry,
**           full userdata that opts in via a truthy __persist metatable field
**           (serialized self-contained: raw payload bytes + its metatable).
** Not supported: light userdata; full userdata without a __persist metatable
**                field (serialized as nil, exactly as before); persistable
**                userdata carrying Lua user values (nuvalue > 0) — hard error.
*/

#ifndef lstasis_h
#define lstasis_h

#include <stddef.h>
#include "lua.h"

/*
** Entry for the C function registry.  Terminate the array with {NULL, NULL}.
** The opener is called with no arguments and must return the library table.
** Every C function in that table is registered as "libname.funcname".
** Example:  {"coroutine", luaopen_coroutine}, {"table", luaopen_table}
*/
typedef struct {
  const char    *libname;
  lua_CFunction  opener;
} lstasis_Lib;

/*
** A built-in continuation (lua_KFunction) that lstasis recognizes. A C call
** frame suspended in one of these — e.g. a coroutine that yielded across
** pcall — can be serialized and restored, because the continuation pointer is
** identified by 'name' rather than by its (non-portable) address. A
** continuation that is not built in (e.g. one passed to lua_pcallk by user C
** code) cannot be reconstructed and is rejected at save time.
** lstasis_builtin_konts() returns a {NULL,NULL}-terminated table; it lives in
** lbaselib.c, where the continuation functions are visible.
*/
typedef struct {
  const char    *name;
  lua_KFunction  k;
} lstasis_Kont;

const lstasis_Kont *lstasis_builtin_konts(void);

/*
** Serialize the full state of L into a freshly malloc'd byte buffer.
** libs: NULL-or-{NULL,NULL}-terminated list of library openers used to
**       resolve C functions.  Unknown C functions are a hard error.
**
** Full userdata is serialized when (and only when) its metatable has a truthy
** '__persist' field; such a value is self-contained — its raw payload bytes and
** its metatable (an ordinary serialized object) round-trip together, and the
** metatable is reattached by object id on load. The metatable round-trips in
** full, so its metamethods work after load: dispatch (__index, __eq, ...) is
** restored and a __gc finalizer is re-registered, so it runs when the loaded
** object is collected. Full userdata without '__persist' is serialized as nil
** (the historical behavior). A persistable userdata that carries Lua user
** values (nuvalue > 0) is a hard save error for now (typical persistable
** handles have nuvalue 0); so is a payload of 2^32 bytes or more.
**
** On success, *out_buf points to the buffer (caller must free) and
** *out_size holds its length.  Returns 0 on success, -1 on error.
*/
int lstasis_save(lua_State *L, const lstasis_Lib *libs,
                 unsigned char **out_buf, size_t *out_size);

/*
** Deserialize a buffer produced by lstasis_save.
** libs must match (or be a superset of) the libs used at save time.
** Returns a new, fully independent lua_State, or NULL on error.
*/
lua_State *lstasis_load(const unsigned char *buf, size_t size,
                        const lstasis_Lib *libs);

#endif
