/*
** lstasis_format.h
** LuaStasis binary format constants shared between lstasis.c and any reader
** (e.g. lstasis_tojson.c).  All values are part of the on-wire format;
** changing them breaks compatibility with existing buffers.
*/

#ifndef lstasis_format_h
#define lstasis_format_h

/* -------------------------------------------------------------------------
** Object type codes written to the buffer header
** ----------------------------------------------------------------------- */
#define OBJ_STRING       1
#define OBJ_TABLE        2
#define OBJ_PROTO        3
#define OBJ_LCLOSURE     4
#define OBJ_UPVAL_CLOSED 5
#define OBJ_UPVAL_OPEN   6   /* data lives in the owning thread record */
#define OBJ_THREAD       7
#define OBJ_CCLOSURE     8   /* C closure: fn name + inline Lua upvalues */
#define OBJ_USERDATA     9   /* full userdata: payload_len + raw payload + metatable id */

/* -------------------------------------------------------------------------
** Inline tag for a light-C-function TValue in the stream.
** Layout: CFUNC_TAG  uint16_t:name_len  char[name_len]  ("libname.funcname")
**
** Must not have bit 6 (BIT_ISCOLLECTABLE = 0x40) set, or readers that check
** iscollectable first will misinterpret it as a GC-object reference and
** consume 4 bytes instead of the name.
** ----------------------------------------------------------------------- */
#define CFUNC_TAG 0x80

/* -------------------------------------------------------------------------
** TValue tag byte constants  (Lua 5.5, lobject.h)
**
**   tag = makevariant(base_type, variant)  |  BIT_ISCOLLECTABLE?
**   BIT_ISCOLLECTABLE = bit 6 = 0x40
**
**   If bit 6 is set  → GC object; a uint32_t object-ID follows.
**   TV_INT / TV_FLOAT → 8 bytes of value follow.
**   All others        → no extra bytes.
** ----------------------------------------------------------------------- */
#define TV_NIL    0x00   /* makevariant(LUA_TNIL,     0)              */
#define TV_FALSE  0x01   /* makevariant(LUA_TBOOLEAN, 0)              */
#define TV_TRUE   0x11   /* makevariant(LUA_TBOOLEAN, 1)              */
#define TV_INT    0x03   /* makevariant(LUA_TNUMBER,  0) — integer    */
#define TV_FLOAT  0x13   /* makevariant(LUA_TNUMBER,  1) — float      */
#define BIT_COLL  0x40   /* BIT_ISCOLLECTABLE — GC ref if set         */

/* -------------------------------------------------------------------------
** Proto flag bits as serialized (Lua 5.5, lobject.h).
** The serializer strips PF_FIXED before writing, so it is never set here.
** ----------------------------------------------------------------------- */
#define LPF_VAHID  1   /* function has hidden vararg args  (PF_VAHID) */
#define LPF_VATAB  2   /* function has a vararg table      (PF_VATAB) */
#define LPF_ISVARARG  (LPF_VAHID | LPF_VATAB)  /* either vararg form */

#endif /* lstasis_format_h */
