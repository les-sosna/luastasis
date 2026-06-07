/*
** lstasis_tojson.c
** Convert a lstasis snapshot buffer (from lstasis_save) to a readable JSON dump.
**
** Standalone — no Lua headers or linkage required; all format constants are
** embedded from the specification in lstate_serial.c.
**
** Usage:  lstasis_tojson <file.bin>
**         lstasis_tojson < file.bin   (reads from stdin)
**
** Public API:
**   int lstasis_tojson(const unsigned char *buf, size_t size, FILE *out);
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <float.h>
#include <errno.h>

#include "lstasis_format.h"
#include "lstasis_tojson.h"

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
static void jsonnl(void) {
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
  jsonnl();
  fprintf(g_out, "\"%s\": ", k);
}

/* -------------------------------------------------------------------------
** TValue printer and skipper
** ----------------------------------------------------------------------- */

/* Emit one TValue from the stream as a JSON value. */
static void jtv(RBuf *rb) {
  uint8_t tag;
  if (rb->err) { fputs("null", g_out); return; }
  tag = rb_u8(rb);
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
  uint32_t mt;
  uint32_t acount = rb_u32(rb);
  uint32_t hsize;
  uint32_t i;
  fprintf(g_out, ", \"array\":[");
  g_depth++;
  for (i = 0; i < acount; i++) {
    uint32_t idx = rb_u32(rb);
    if (i > 0) fputc(',', g_out);
    jsonnl(); fprintf(g_out, "{\"idx\":%u, \"val\":", idx);
    jtv(rb);
    fputc('}', g_out);
    if (rb->err) break;
  }
  g_depth--;
  if (acount > 0) jsonnl();
  fputc(']', g_out);

  hsize = rb_u32(rb);
  fprintf(g_out, ", \"hsize\":%u, \"hash\":[", hsize);
  g_depth++;
  for (i = 0; i < hsize; i++) {
    size_t before;
    int32_t nxt;
    if (i > 0) fputc(',', g_out);
    jsonnl();
    before = rb->pos;
    fprintf(g_out, "{\"idx\":%u", i);
    if (rb->pos < rb->size && rb->data[before] == TV_NIL) {
      rb_u8(rb);  /* consume the empty-slot marker */
      fputs(", \"empty\":true}", g_out);
      continue;
    }
    fputs(", \"key\":", g_out);
    jtv(rb);
    fputs(", \"val\":", g_out);
    jtv(rb);
    nxt = rb_i32(rb);
    fprintf(g_out, ", \"next\":%d}", nxt);
    if (rb->err) break;
  }
  g_depth--;
  if (hsize > 0) jsonnl();
  fputc(']', g_out);

  mt = rb_u32(rb);
  if (mt) fprintf(g_out, ", \"metatable\":%u", mt);
  else fputs(", \"metatable\":null", g_out);

  fprintf(g_out, ", \"asize\":%u", rb_u32(rb));
}

static void dump_proto(RBuf *rb) {
  int32_t lld;
  int32_t ld;
  uint32_t ncode;
  uint32_t nk;
  uint32_t np;
  uint32_t nuv;
  uint32_t nline;
  uint32_t nabsl;
  uint32_t nloc;
  uint32_t src;
  uint8_t params  = rb_u8(rb);
  uint8_t flag    = rb_u8(rb);
  uint8_t maxstk  = rb_u8(rb);
  fprintf(g_out, ", \"params\":%u, \"is_vararg\":%s, \"max_stack\":%u",
         params, (flag & LPF_ISVARARG) ? "true" : "false", maxstk);

  ncode = rb_u32(rb);
  fprintf(g_out, ", \"num_instructions\":%u", ncode);
  rb_skip(rb, (size_t)ncode * INSTR_SZ);

  nk = rb_u32(rb);
  fputs(", \"constants\":[", g_out);
  for (uint32_t i = 0; i < nk; i++) {
    if (i > 0) fputc(',', g_out);
    jtv(rb);
    if (rb->err) break;
  }
  fputc(']', g_out);

  np = rb_u32(rb);
  fputs(", \"sub_protos\":[", g_out);
  for (uint32_t i = 0; i < np; i++) {
    uint32_t id;
    if (i > 0) fputc(',', g_out);
    id = rb_u32(rb);
    if (id) fprintf(g_out, "%u", id); else fputs("null", g_out);
  }
  fputc(']', g_out);

  nuv = rb_u32(rb);
  fputs(", \"upvalues\":[", g_out);
  g_depth++;
  for (uint32_t i = 0; i < nuv; i++) {
    uint32_t nid;
    uint8_t ins, idx, knd;
    if (i > 0) fputc(',', g_out);
    jsonnl();
    nid = rb_u32(rb);
    ins = rb_u8(rb);
    idx = rb_u8(rb);
    knd = rb_u8(rb);
    fputs("{\"name\":", g_out);
    if (nid) fprintf(g_out, "{\"ref\":%u}", nid); else fputs("null", g_out);
    fprintf(g_out, ", \"instack\":%u, \"idx\":%u, \"kind\":%u}", ins, idx, knd);
  }
  g_depth--;
  if (nuv > 0) jsonnl();
  fputc(']', g_out);

  nline = rb_u32(rb);
  rb_skip(rb, nline);                               /* lineinfo: 1 byte each */
  nabsl = rb_u32(rb);
  rb_skip(rb, (size_t)nabsl * ABSLINE_SZ);          /* abslineinfo */

  nloc = rb_u32(rb);
  fputs(", \"locvars\":[", g_out);
  g_depth++;
  for (uint32_t i = 0; i < nloc; i++) {
    int32_t epc, spc;
    uint32_t vid;
    if (i > 0) fputc(',', g_out);
    jsonnl();
    vid = rb_u32(rb);
    spc = rb_i32(rb);
    epc = rb_i32(rb);
    fputs("{\"name\":", g_out);
    if (vid) fprintf(g_out, "{\"ref\":%u}", vid); else fputs("null", g_out);
    fprintf(g_out, ", \"startpc\":%d, \"endpc\":%d}", spc, epc);
  }
  g_depth--;
  if (nloc > 0) jsonnl();
  fputc(']', g_out);

  src = rb_u32(rb);
  ld = rb_i32(rb);
  lld = rb_i32(rb);
  fputs(", \"source\":", g_out);
  if (src) fprintf(g_out, "{\"ref\":%u}", src); else fputs("null", g_out);
  fprintf(g_out, ", \"line_defined\":%d, \"last_line_defined\":%d", ld, lld);
}

static void dump_lclosure(RBuf *rb) {
  uint8_t nuv;
  uint32_t pid = rb_u32(rb);
  fputs(", \"proto\":", g_out);
  if (pid) fprintf(g_out, "%u", pid); else fputs("null", g_out);

  nuv = rb_u8(rb);
  fputs(", \"upvalues\":[", g_out);
  for (int i = 0; i < (int)nuv; i++) {
    uint32_t uid;
    if (i > 0) fputc(',', g_out);
    uid = rb_u32(rb);
    if (uid) fprintf(g_out, "%u", uid); else fputs("null", g_out);
  }
  fputc(']', g_out);
}

static void dump_cclosure(RBuf *rb) {
  uint8_t nuv;
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

  nuv = rb_u8(rb);
  fputs(", \"upvalues\":[", g_out);
  g_depth++;
  for (int i = 0; i < (int)nuv; i++) {
    if (i > 0) fputc(',', g_out);
    jsonnl();
    jtv(rb);
    if (rb->err) break;
  }
  g_depth--;
  if (nuv > 0) jsonnl();
  fputc(']', g_out);
}

static void dump_upval_closed(RBuf *rb) {
  fputs(", \"value\":", g_out);
  jtv(rb);
}

/* OBJ_USERDATA record: payload_len:u32, payload, mt_id:u32. The payload is an
** opaque blob, so emit its length, a short hex preview, and the metatable id. */
static void dump_userdata(RBuf *rb) {
  uint32_t plen = rb_u32(rb);
  uint32_t mt_id;
  uint32_t preview;
  if (rb->err) return;
  /* Avoid the (pos + plen + 4) overflow that wraps on 32-bit size_t; compare
  ** against remaining room instead (rb->pos <= rb->size is maintained). A
  ** truncated record sets rb->err so the object is tagged "parse_error", as the
  ** other dump_* helpers do. */
  if ((size_t)plen > rb->size - rb->pos ||
      rb->size - rb->pos - (size_t)plen < 4) { rb->err = 1; return; }
  fprintf(g_out, ", \"payload_len\":%u, \"payload\":\"", plen);
  preview = plen < 32 ? plen : 32;  /* cap the inline hex preview */
  for (uint32_t i = 0; i < preview; i++)
    fprintf(g_out, "%02x", rb->data[rb->pos + i]);
  if (plen > preview) fputs("...", g_out);
  fputc('"', g_out);
  rb->pos += plen;
  mt_id = rb_u32(rb);
  fprintf(g_out, ", \"metatable\":%u", mt_id);
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
  int32_t nci;
  uint32_t nopen;
  uint8_t  status = rb_u8(rb);
  int32_t  nstack = rb_i32(rb);
  fprintf(g_out, ", \"status\":%u, \"status_name\":\"%s\", \"stack_size\":%d",
         status, thread_status_name(status), nstack);

  fputs(", \"stack\":[", g_out);
  g_depth++;
  for (int32_t i = 0; i < nstack; i++) {
    if (i > 0) fputc(',', g_out);
    jsonnl(); jtv(rb);
    if (rb->err) break;
  }
  g_depth--;
  if (nstack > 0) jsonnl();
  fputc(']', g_out);

  nci = rb_i32(rb);
  fprintf(g_out, ", \"num_callinfos\":%d, \"callinfos\":[", nci);
  g_depth++;
  for (int32_t j = 0; j < nci; j++) {
    uint32_t cstat;
    int32_t toff;
    int32_t foff;
    uint8_t is_lua;
    int32_t u2v;
    if (j > 0) fputc(',', g_out);
    jsonnl();
    is_lua = rb_u8(rb);
    foff = rb_i32(rb);
    toff = rb_i32(rb);
    cstat = rb_u32(rb);
    u2v = rb_i32(rb);
    fprintf(g_out, "{\"is_lua\":%s, \"func_slot\":%d, \"top_slot\":%d, \"callstatus\":%u, \"u2\":%d",
           is_lua ? "true" : "false", foff, toff, cstat, u2v);
    if (is_lua) {
      uint32_t pid    = rb_u32(rb);
      int32_t  pcoff  = rb_i32(rb);
      int32_t  nextra = rb_i32(rb);
      fputs(", \"proto\":", g_out);
      if (pid) fprintf(g_out, "%u", pid); else fputs("null", g_out);
      fprintf(g_out, ", \"pc_offset\":%d, \"nextra\":%d", pcoff, nextra);
    } else {
      uint8_t has_kont = rb_u8(rb);
      if (has_kont) {
        uint16_t knlen = rb_u16(rb);
        const char *kname = (const char *)(rb->data + rb->pos);
        int64_t ctx, errf;
        if (rb->pos + knlen > rb->size) { rb->err = 1; break; }
        rb->pos += knlen;
        ctx  = (int64_t)rb_u64(rb);
        errf = (int64_t)rb_u64(rb);
        fputs(", \"continuation\":", g_out);
        jstr_bytes((const uint8_t *)kname, knlen);
        fprintf(g_out, ", \"ctx\":%" PRId64 ", \"old_errfunc\":%" PRId64, ctx, errf);
      } else {
        fputs(", \"continuation\":null", g_out);
      }
    }
    fputc('}', g_out);
    if (rb->err) break;
  }
  g_depth--;
  if (nci > 0) jsonnl();
  fputc(']', g_out);

  nopen = rb_u32(rb);
  fputs(", \"open_upvalues\":[", g_out);
  for (uint32_t i = 0; i < nopen; i++) {
    int32_t soff;
    uint32_t uid;
    if (i > 0) fputc(',', g_out);
    uid = rb_u32(rb);
    soff = rb_i32(rb);
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
  case OBJ_USERDATA:     return "userdata";
  default:               return "unknown";
  }
}

/* Types whose dump functions emit no internal newlines. */
static int obj_is_single_line(uint8_t t) {
  return t == OBJ_STRING || t == OBJ_LCLOSURE ||
         t == OBJ_UPVAL_CLOSED || t == OBJ_UPVAL_OPEN ||
         t == OBJ_USERDATA;
}

/* -------------------------------------------------------------------------
** Public API
** ----------------------------------------------------------------------- */
int lstasis_tojson(const unsigned char *buf, size_t sz, FILE *out) {
  size_t header_size;
  size_t footer_size;
  size_t objects_end;
  size_t max_objects;
  uint8_t *types;
  uint64_t *objids;
  size_t *offsets;
  uint64_t next_seq;
  uint32_t seed;
  uint32_t num_objects;
  uint32_t registry_id;
  uint32_t main_thread_id;
  uint32_t mt_ids[LUA_NUMTYPES];
  RBuf rb = { (const uint8_t *)buf, 0, sz, 0 };
  g_out = out;

  /* Format: [next_seq:u64][seed:u32] {objects...} [registry:u32
  ** main:u32 mt:u32×LUA_NUMTYPES]. */
  header_size = 8 + 4;
  footer_size = (size_t)(4 + 4 + LUA_NUMTYPES * 4);
  if (rb.size < header_size + footer_size) {
    fprintf(stderr, "lstasis_tojson: buffer too small for header+footer\n");
    return 1;
  }
  objects_end = rb.size - footer_size;

  next_seq = rb_u64(&rb);
  seed     = rb_u32(&rb);
  if (rb.err) { fprintf(stderr, "lstasis_tojson: truncated header\n"); return 1; }

  /* Index pass: record type code, objid, and data offset for each object.
  ** Conservative pre-allocation by min header size (13 bytes). */
  max_objects = (objects_end - rb.pos) / 13;
  if (max_objects == 0) max_objects = 1;
  types   = (uint8_t  *)calloc(max_objects, sizeof(uint8_t));
  objids  = (uint64_t *)calloc(max_objects, sizeof(uint64_t));
  offsets = (size_t   *)calloc(max_objects, sizeof(size_t));
  if (!types || !objids || !offsets) {
    fprintf(stderr, "lstasis_tojson: out of memory\n");
    free(types); free(objids); free(offsets); return 1;
  }

  num_objects = 0;
  while (rb.pos < objects_end) {
    uint32_t dsz;
    if (rb.err) break;
    types[num_objects]    = rb_u8(&rb);
    objids[num_objects]   = rb_u64(&rb);
    dsz = rb_u32(&rb);
    offsets[num_objects]  = rb.pos;
    rb.pos               += dsz;
    if (rb.pos > objects_end) { rb.err = 1; break; }
    num_objects++;
  }
  if (rb.err) {
    fprintf(stderr, "lstasis_tojson: truncated object table\n");
    free(types); free(objids); free(offsets); return 1;
  }
  if (rb.pos + 4 + 4 + (size_t)LUA_NUMTYPES * 4 > rb.size) {
    fprintf(stderr, "lstasis_tojson: truncated roots section\n");
    free(types); free(objids); free(offsets); return 1;
  }

  registry_id = rb_u32(&rb);
  main_thread_id = rb_u32(&rb);
  for (int i = 0; i < LUA_NUMTYPES; i++) mt_ids[i] = rb_u32(&rb);

  /* Emit JSON. */
  g_depth = 0;
  fputc('{', g_out);
  g_depth = 1;

  jkey("format"); fputs("\"lstasis\",", g_out);
  jkey("num_objects"); fprintf(g_out, "%u,", num_objects);
  jkey("next_seq");
  fprintf(g_out, "\"0x%016" PRIx64 "\",", next_seq);
  jkey("seed"); fprintf(g_out, "\"0x%08" PRIx32 "\",", seed);

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
  jsonnl(); fputs("},", g_out);

  jkey("objects"); fputc('{', g_out);
  g_depth++;
  for (uint32_t i = 0; i < num_objects; i++) {
    if (i > 0) fputc(',', g_out);
    jsonnl(); fprintf(g_out, "\"%u\": {", i + 1);
    g_depth++;

    fprintf(g_out, "\"type\":\"%s\", \"objid\":\"0x%016" PRIx64 "\"",
            obj_type_name(types[i]), objids[i]);

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
    case OBJ_USERDATA:     dump_userdata(&rb);     break;
    default: break;
    }

    if (rb.err) fputs(", \"parse_error\":true", g_out);

    g_depth--;
    if (obj_is_single_line(types[i]) && !rb.err) fputc('}', g_out);
    else { jsonnl(); fputc('}', g_out); }
  }
  g_depth--;
  jsonnl(); fputc('}', g_out);

  g_depth--;
  jsonnl(); fputc('}', g_out); fputc('\n', g_out);

  free(types);
  free(objids);
  free(offsets);
  return 0;
}

