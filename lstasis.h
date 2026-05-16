/*
** lstasis.h
** LuaStasis: full lua_State serialization into/from a raw byte buffer.
** Invoke only when no Lua code is running.
**
** Supports: nil, bool, integer, float, string, table, Lua closures,
**           upvalues (open + closed), prototypes, coroutines,
**           C functions (LCF and C closures) via a name registry.
** Not supported: full userdata, light userdata.
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
** Serialize the full state of L into a freshly malloc'd byte buffer.
** libs: NULL-or-{NULL,NULL}-terminated list of library openers used to
**       resolve C functions.  Unknown C functions are a hard error.
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
