/*
** luaser_tojson.c
** Convert a luaser binary state file (from luaser_save) to a readable JSON dump.
**
** Standalone — no Lua headers or linkage required; all format constants are
** embedded from the specification in lstate_serial.c.
**
** Usage:  luaser_tojson <file.bin>
**         luaser_tojson < file.bin   (reads from stdin)
**
** Public API:
**   int luaser_tojson(const unsigned char *buf, size_t size, FILE *out);
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <float.h>
#include <errno.h>

#include "luaser_format.h"
#include "luaser_tojson.h"

/* Sizes of Lua 5.4 internal structures used in the proto record. */
#define INSTR_SZ    4   /* sizeof(Instruction) */
#define ABSLINE_SZ  8   /* sizeof(AbsLineInfo): int32 pc + int32 line */

/* Number of public Lua types; determines the type_metatables[] array size. */
#define LUA_NUMTYPES  9

/* -------------------------------------------------------------------------
** Byte-buffer reader
** ----------------------------------------------------------------------- */
typedef struct {
  const uint8_t *data;
  size_t         pos;
  size_t         size;
  int            err;   /* set on underflow */
} RBuf;

static uint8_t  rb_u8 (RBuf *b) {
  if (b->pos >= b->size) { b->err=1; return 0; }
  return b->data[b->pos++];
}
static uint16_t rb_u16(RBuf *b) {
  uint16_t v;
  if (b->pos+2 > b->size) { b->err=1; return 0; }
  memcpy(&v, b->data+b->pos, 2); b->pos += 2; return v;
}
static uint32_t rb_u32(RBuf *b) {
  uint32_t v;
  if (b->pos+4 > b->size) { b->err=1; return 0; }
  memcpy(&v, b->data+b->pos, 4); b->pos += 4; return v;
}
static int32_t  rb_i32(RBuf *b) {
  int32_t v;
  if (b->pos+4 > b->size) { b->err=1; return 0; }
  memcpy(&v, b->data+b->pos, 4); b->pos += 4; return v;
}
static uint64_t rb_u64(RBuf *b) {
  uint64_t v;
  if (b->pos+8 > b->size) { b->err=1; return 0; }
  memcpy(&v, b->data+b->pos, 8); b->pos += 8; return v;
}
static double   rb_dbl(RBuf *b) {
  double v;
  if (b->pos+8 > b->size) { b->err=1; return 0.0; }
  memcpy(&v, b->data+b->pos, 8); b->pos += 8; return v;
}
static void rb_skip(RBuf *b, size_t n) {
  if (b->pos + n > b->size) { b->err=1; return; }
  b->pos += n;
}

/* -------------------------------------------------------------------------
** JSON output helpers
** ----------------------------------------------------------------------- */
static int   g_depth = 0;
static FILE *g_out   = NULL;

/* Newline + indentation */
static void jnl(void) {
  fputc('\n', g_out);
  for (int i = 0; i < g_depth; i++) fputs("  ", g_out);
}

/* Print a raw byte span as a JSON string with full escape handling */
static void jstr_bytes(const uint8_t *data, size_t len) {
  fputc('"', g_out);
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if      (c == '"')  fputs("\\\"", g_out);
    else if (c == '\\') fputs("\\\\", g_out);
    else if (c == '\n') fputs("\\n",  g_out);
    else if (c == '\r') fputs("\\r",  g_out);
    else if (c == '\t') fputs("\\t",  g_out);
    else if (c < 0x20 || c > 0x7e) fprintf(g_out, "\\u%04x", (unsigned)c);
    else fputc((int)c, g_out);
  }
  fputc('"', g_out);
}

static void jkey(const char *k) {
  jnl();
  fprintf(g_out, "\"%s\": ", k);
}

/* -------------------------------------------------------------------------
** TValue printer and skipper
** ----------------------------------------------------------------------- */

/* Emit one TValue from the stream as a JSON value. */
static void jtv(RBuf *rb) {
  if (rb->err) { fputs("null", g_out); return; }
  uint8_t tag = rb_u8(rb);
  if (rb->err) { fputs("null", g_out); return; }

  if (tag == CFUNC_TAG) {
    uint16_t nlen = rb_u16(rb);
    if (rb->err || rb->pos + nlen > rb->size) { fputs("null", g_out); rb->err=1; return; }
    fputs("{\"cfunc\":", g_out);
    jstr_bytes(rb->data + rb->pos, nlen);
    fputc('}', g_out);
    rb->pos += nlen;

  } else if (tag == TV_NIL) {
    fputs("null", g_out);

  } else if (tag == TV_FALSE) {
    fputs("{\"bool\":false}", g_out);

  } else if (tag == TV_TRUE) {
    fputs("{\"bool\":true}", g_out);

  } else if (tag == TV_INT) {
    int64_t v = (int64_t)rb_u64(rb);
    fprintf(g_out, "{\"int\":%" PRId64 "}", v);

  } else if (tag == TV_FLOAT) {
    double v = rb_dbl(rb);
    if (isnan(v))       fputs("{\"float\":\"NaN\"}", g_out);
    else if (isinf(v))  fprintf(g_out, "{\"float\":\"%sInf\"}", v < 0 ? "-" : "+");
    else                fprintf(g_out, "{\"float\":%.*g}", DBL_DIG + 2, v);

  } else if (tag & BIT_COLL) {
    uint32_t id = rb_u32(rb);
    if (id == 0) fputs("null", g_out);
    else         fprintf(g_out, "{\"ref\":%u}", id);

  } else {
    fprintf(g_out, "{\"?tag\":%u}", (unsigned)tag);
  }
}

/* -------------------------------------------------------------------------
** Per-object-type printers
** Each is called with rb->pos already at the start of the object's data.
** Each appends key:"value" pairs to the current JSON object (no braces).
** ----------------------------------------------------------------------- */

static void dump_string(RBuf *rb) {
  uint32_t len = rb_u32(rb);
  if (rb->err || rb->pos + len > rb->size) return;
  fprintf(g_out, ", \"length\":%u, \"value\":", len);
  jstr_bytes(rb->data + rb->pos, len);
  rb->pos += len;
}

static void dump_table(RBuf *rb) {
  uint32_t cnt = rb_u32(rb);
  fprintf(g_out, ", \"entry_count\":%u", cnt);
  fputs(", \"entries\":[", g_out);
  g_depth++;
  for (uint32_t i = 0; i < cnt; i++) {
    if (i > 0) fputc(',', g_out);
    jnl(); fputs("{\"key\":", g_out);
    jtv(rb);
    fputs(", \"val\":", g_out);
    jtv(rb);
    fputc('}', g_out);
    if (rb->err) break;
  }
  g_depth--;
  if (cnt > 0) jnl();
  fputc(']', g_out);

  uint32_t mt = rb_u32(rb);
  if (mt) fprintf(g_out, ", \"metatable\":%u", mt);
  else fputs(", \"metatable\":null", g_out);

  fprintf(g_out, ", \"asize\":%u", rb_u32(rb));
}

static void dump_proto(RBuf *rb) {
  uint8_t params  = rb_u8(rb);
  uint8_t flag    = rb_u8(rb);
  uint8_t maxstk  = rb_u8(rb);
  fprintf(g_out, ", \"params\":%u, \"is_vararg\":%s, \"max_stack\":%u",
         params, (flag & LPF_ISVARARG) ? "true" : "false", maxstk);

  uint32_t ncode = rb_u32(rb);
  fprintf(g_out, ", \"num_instructions\":%u", ncode);
  rb_skip(rb, (size_t)ncode * INSTR_SZ);

  uint32_t nk = rb_u32(rb);
  fputs(", \"constants\":[", g_out);
  for (uint32_t i = 0; i < nk; i++) {
    if (i > 0) fputc(',', g_out);
    jtv(rb);
    if (rb->err) break;
  }
  fputc(']', g_out);

  uint32_t np = rb_u32(rb);
  fputs(", \"sub_protos\":[", g_out);
  for (uint32_t i = 0; i < np; i++) {
    if (i > 0) fputc(',', g_out);
    uint32_t id = rb_u32(rb);
    if (id) fprintf(g_out, "%u", id); else fputs("null", g_out);
  }
  fputc(']', g_out);

  uint32_t nuv = rb_u32(rb);
  fputs(", \"upvalues\":[", g_out);
  g_depth++;
  for (uint32_t i = 0; i < nuv; i++) {
    if (i > 0) fputc(',', g_out);
    jnl();
    uint32_t nid = rb_u32(rb);
    uint8_t  ins = rb_u8(rb);
    uint8_t  idx = rb_u8(rb);
    uint8_t  knd = rb_u8(rb);
    fputs("{\"name\":", g_out);
    if (nid) fprintf(g_out, "{\"ref\":%u}", nid); else fputs("null", g_out);
    fprintf(g_out, ", \"instack\":%u, \"idx\":%u, \"kind\":%u}", ins, idx, knd);
  }
  g_depth--;
  if (nuv > 0) jnl();
  fputc(']', g_out);

  uint32_t nline = rb_u32(rb);
  rb_skip(rb, nline);                               /* lineinfo: 1 byte each */
  uint32_t nabsl = rb_u32(rb);
  rb_skip(rb, (size_t)nabsl * ABSLINE_SZ);          /* abslineinfo */

  uint32_t nloc = rb_u32(rb);
  fputs(", \"locvars\":[", g_out);
  g_depth++;
  for (uint32_t i = 0; i < nloc; i++) {
    if (i > 0) fputc(',', g_out);
    jnl();
    uint32_t vid = rb_u32(rb);
    int32_t  spc = rb_i32(rb);
    int32_t  epc = rb_i32(rb);
    fputs("{\"name\":", g_out);
    if (vid) fprintf(g_out, "{\"ref\":%u}", vid); else fputs("null", g_out);
    fprintf(g_out, ", \"startpc\":%d, \"endpc\":%d}", spc, epc);
  }
  g_depth--;
  if (nloc > 0) jnl();
  fputc(']', g_out);

  uint32_t src = rb_u32(rb);
  int32_t  ld  = rb_i32(rb);
  int32_t  lld = rb_i32(rb);
  fputs(", \"source\":", g_out);
  if (src) fprintf(g_out, "{\"ref\":%u}", src); else fputs("null", g_out);
  fprintf(g_out, ", \"line_defined\":%d, \"last_line_defined\":%d", ld, lld);
}

static void dump_lclosure(RBuf *rb) {
  uint32_t pid = rb_u32(rb);
  fputs(", \"proto\":", g_out);
  if (pid) fprintf(g_out, "%u", pid); else fputs("null", g_out);

  uint8_t nuv = rb_u8(rb);
  fputs(", \"upvalues\":[", g_out);
  for (int i = 0; i < (int)nuv; i++) {
    if (i > 0) fputc(',', g_out);
    uint32_t uid = rb_u32(rb);
    if (uid) fprintf(g_out, "%u", uid); else fputs("null", g_out);
  }
  fputc(']', g_out);
}

static void dump_cclosure(RBuf *rb) {
  uint8_t tag = rb_u8(rb);
  fputs(", \"func\":", g_out);
  if (tag == CFUNC_TAG) {
    uint16_t nlen = rb_u16(rb);
    if (rb->pos + nlen > rb->size) { fputs("null", g_out); rb->err=1; return; }
    jstr_bytes(rb->data + rb->pos, nlen);
    rb->pos += nlen;
  } else {
    fputs("null", g_out);
  }

  uint8_t nuv = rb_u8(rb);
  fputs(", \"upvalues\":[", g_out);
  g_depth++;
  for (int i = 0; i < (int)nuv; i++) {
    if (i > 0) fputc(',', g_out);
    jnl();
    jtv(rb);
    if (rb->err) break;
  }
  g_depth--;
  if (nuv > 0) jnl();
  fputc(']', g_out);
}

static void dump_upval_closed(RBuf *rb) {
  fputs(", \"value\":", g_out);
  jtv(rb);
}

static const char *thread_status_name(uint8_t s) {
  switch (s) {
  case 0: return "ok";
  case 1: return "yield";
  case 2: return "errrun";
  case 3: return "errsyntax";
  case 4: return "errmem";
  case 6: return "errerr";
  default: return "?";
  }
}

static void dump_thread(RBuf *rb) {
  uint8_t  status = rb_u8(rb);
  int32_t  nstack = rb_i32(rb);
  fprintf(g_out, ", \"status\":%u, \"status_name\":\"%s\", \"stack_size\":%d",
         status, thread_status_name(status), nstack);

  fputs(", \"stack\":[", g_out);
  g_depth++;
  for (int32_t i = 0; i < nstack; i++) {
    if (i > 0) fputc(',', g_out);
    jnl(); jtv(rb);
    if (rb->err) break;
  }
  g_depth--;
  if (nstack > 0) jnl();
  fputc(']', g_out);

  int32_t nci = rb_i32(rb);
  fprintf(g_out, ", \"num_callinfos\":%d, \"callinfos\":[", nci);
  g_depth++;
  for (int32_t j = 0; j < nci; j++) {
    if (j > 0) fputc(',', g_out);
    jnl();
    uint8_t  is_lua = rb_u8(rb);
    int32_t  foff   = rb_i32(rb);
    int32_t  toff   = rb_i32(rb);
    uint32_t cstat  = rb_u32(rb);
    int32_t  u2v    = rb_i32(rb); (void)u2v;
    fprintf(g_out, "{\"is_lua\":%s, \"func_slot\":%d, \"top_slot\":%d, \"callstatus\":%u",
           is_lua ? "true" : "false", foff, toff, cstat);
    if (is_lua) {
      uint32_t pid    = rb_u32(rb);
      int32_t  pcoff  = rb_i32(rb);
      int32_t  nextra = rb_i32(rb);
      fputs(", \"proto\":", g_out);
      if (pid) fprintf(g_out, "%u", pid); else fputs("null", g_out);
      fprintf(g_out, ", \"pc_offset\":%d, \"nextra\":%d", pcoff, nextra);
    }
    fputc('}', g_out);
    if (rb->err) break;
  }
  g_depth--;
  if (nci > 0) jnl();
  fputc(']', g_out);

  uint32_t nopen = rb_u32(rb);
  fputs(", \"open_upvalues\":[", g_out);
  for (uint32_t i = 0; i < nopen; i++) {
    if (i > 0) fputc(',', g_out);
    uint32_t uid  = rb_u32(rb);
    int32_t  soff = rb_i32(rb);
    fprintf(g_out, "{\"id\":%u, \"stack_offset\":%d}", uid, soff);
    if (rb->err) break;
  }
  fputc(']', g_out);
}

/* -------------------------------------------------------------------------
** Top-level dump
** ----------------------------------------------------------------------- */
static const char *obj_type_name(uint8_t t) {
  switch (t) {
  case OBJ_STRING:       return "string";
  case OBJ_TABLE:        return "table";
  case OBJ_PROTO:        return "proto";
  case OBJ_LCLOSURE:     return "lclosure";
  case OBJ_UPVAL_CLOSED: return "upvalue_closed";
  case OBJ_UPVAL_OPEN:   return "upvalue_open";
  case OBJ_THREAD:       return "thread";
  case OBJ_CCLOSURE:     return "cclosure";
  default:               return "unknown";
  }
}

/* Types whose dump functions emit no internal newlines. */
static int obj_is_single_line(uint8_t t) {
  return t == OBJ_STRING || t == OBJ_LCLOSURE ||
         t == OBJ_UPVAL_CLOSED || t == OBJ_UPVAL_OPEN;
}

/* -------------------------------------------------------------------------
** Public API
** ----------------------------------------------------------------------- */
int luaser_tojson(const unsigned char *buf, size_t sz, FILE *out) {
  RBuf rb = { (const uint8_t *)buf, 0, sz, 0 };
  g_out = out;

  uint32_t num_objects = rb_u32(&rb);
  if (rb.err) { fprintf(stderr, "luaser_tojson: truncated header\n"); return 1; }

  /* Index pass: record type code and data offset for each object. */
  uint8_t *types   = (uint8_t *)calloc(num_objects, sizeof(uint8_t));
  size_t  *offsets = (size_t  *)calloc(num_objects, sizeof(size_t));
  if (!types || !offsets) {
    fprintf(stderr, "luaser_tojson: out of memory\n");
    free(types); free(offsets); return 1;
  }

  for (uint32_t i = 0; i < num_objects; i++) {
    if (rb.err) break;
    types[i]    = rb_u8(&rb);
    uint32_t dsz = rb_u32(&rb);
    offsets[i]  = rb.pos;
    rb.pos     += dsz;
  }
  if (rb.err) {
    fprintf(stderr, "luaser_tojson: truncated object table\n");
    free(types); free(offsets); return 1;
  }
  if (rb.pos + 4 + 4 + (size_t)LUA_NUMTYPES * 4 > rb.size) {
    fprintf(stderr, "luaser_tojson: truncated roots section\n");
    free(types); free(offsets); return 1;
  }

  uint32_t registry_id    = rb_u32(&rb);
  uint32_t main_thread_id = rb_u32(&rb);
  uint32_t mt_ids[LUA_NUMTYPES];
  for (int i = 0; i < LUA_NUMTYPES; i++) mt_ids[i] = rb_u32(&rb);

  /* Emit JSON. */
  g_depth = 0;
  fputc('{', g_out);
  g_depth = 1;

  jkey("format"); fputs("\"luaser\",", g_out);
  jkey("num_objects"); fprintf(g_out, "%u,", num_objects);

  jkey("roots"); fputs("{", g_out);
  g_depth++;
  jkey("registry");    fprintf(g_out, "%u,", registry_id);
  jkey("main_thread"); fprintf(g_out, "%u,", main_thread_id);
  jkey("type_metatables"); fputc('[', g_out);
  for (int i = 0; i < LUA_NUMTYPES; i++) {
    if (i > 0) fputc(',', g_out);
    if (mt_ids[i]) fprintf(g_out, "%u", mt_ids[i]); else fputs("null", g_out);
  }
  fputc(']', g_out);
  g_depth--;
  jnl(); fputs("},", g_out);

  jkey("objects"); fputc('{', g_out);
  g_depth++;
  for (uint32_t i = 0; i < num_objects; i++) {
    if (i > 0) fputc(',', g_out);
    jnl(); fprintf(g_out, "\"%u\": {", i + 1);
    g_depth++;

    fprintf(g_out, "\"type\":\"%s\"", obj_type_name(types[i]));

    /* Seek to this object's data and parse it. */
    rb.pos = offsets[i];
    rb.err = 0;

    switch (types[i]) {
    case OBJ_STRING:       dump_string(&rb);       break;
    case OBJ_TABLE:        dump_table(&rb);        break;
    case OBJ_PROTO:        dump_proto(&rb);        break;
    case OBJ_LCLOSURE:     dump_lclosure(&rb);     break;
    case OBJ_CCLOSURE:     dump_cclosure(&rb);     break;
    case OBJ_UPVAL_CLOSED: dump_upval_closed(&rb); break;
    case OBJ_UPVAL_OPEN:   break;
    case OBJ_THREAD:       dump_thread(&rb);       break;
    default: break;
    }

    if (rb.err) fputs(", \"parse_error\":true", g_out);

    g_depth--;
    if (obj_is_single_line(types[i]) && !rb.err) fputc('}', g_out);
    else { jnl(); fputc('}', g_out); }
  }
  g_depth--;
  jnl(); fputc('}', g_out);

  g_depth--;
  jnl(); fputc('}', g_out); fputc('\n', g_out);

  free(types);
  free(offsets);
  return 0;
}

