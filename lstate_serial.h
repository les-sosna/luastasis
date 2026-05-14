/*
** lstate_serial.h
** Full lua_State serialization into/from a raw byte buffer.
** Invoke only when no Lua code is running.
**
** Supports: nil, bool, integer, float, string, table, Lua closures,
**           upvalues (open + closed), prototypes, coroutines.
** Not supported: C closures, full userdata, light userdata.
*/

#ifndef lstate_serial_h
#define lstate_serial_h

#include <stddef.h>
#include "lua.h"

/*
** Serialize the full state of L into a freshly malloc'd byte buffer.
** On success, *out_buf points to the buffer (caller must free) and
** *out_size holds its length.  Returns 0 on success, -1 on error.
*/
int luaser_save(lua_State *L, unsigned char **out_buf, size_t *out_size);

/*
** Deserialize a buffer produced by luaser_save.
** Returns a new, fully independent lua_State, or NULL on error.
*/
lua_State *luaser_load(const unsigned char *buf, size_t size);

#endif
