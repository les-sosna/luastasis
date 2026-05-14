/*
** lstate_serial.c
** Full lua_State serialization / deserialization via a memory buffer.
*/

#define lstate_serial_c
#define LUA_CORE

#include "lprefix.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lua.h"
#include "lstate.h"
#include "lobject.h"
#include "ltable.h"
#include "lfunc.h"
#include "lstring.h"
#include "lgc.h"
#include "ldo.h"
#include "lmem.h"
#include "lauxlib.h"

#include "lstate_serial.h"

/* -----------------------------------------------------------------------
** Object type codes written to the buffer
** --------------------------------------------------------------------- */
#define OBJ_STRING       1
#define OBJ_TABLE        2
#define OBJ_PROTO        3
#define OBJ_LCLOSURE     4
#define OBJ_UPVAL_CLOSED 5
#define OBJ_UPVAL_OPEN   6   /* placeholder; data lives in owning thread */
#define OBJ_THREAD       7

#define ID_NULL 0u

/* -----------------------------------------------------------------------
** ObjMap  –  pointer-to-uint32 open-addressing hash table
** --------------------------------------------------------------------- */
typedef struct { const void *ptr; uint32_t id; } ObjMapEntry;
typedef struct { ObjMapEntry *ents; uint32_t cap; uint32_t count; } ObjMap;

static void objmap_init(ObjMap *m) {
  m->cap   = 2048;
  m->count = 0;
  m->ents  = (ObjMapEntry *)calloc(m->cap, sizeof(ObjMapEntry));
}
static void objmap_free(ObjMap *m) { free(m->ents); }

static uint32_t om_hash(const void *p, uint32_t cap) {
  return (uint32_t)((uintptr_t)p * 2654435761u) & (cap - 1);
}

static uint32_t objmap_find(const ObjMap *m, const void *ptr) {
  uint32_t h = om_hash(ptr, m->cap);
  for (;;) {
    if (m->ents[h].ptr == ptr) return m->ents[h].id;
    if (m->ents[h].ptr == NULL) return 0;
    h = (h + 1) & (m->cap - 1);
  }
}

static void objmap_insert_raw(ObjMapEntry *ents, uint32_t cap,
                               const void *ptr, uint32_t id) {
  uint32_t h = om_hash(ptr, cap);
  while (ents[h].ptr != NULL && ents[h].ptr != ptr)
    h = (h + 1) & (cap - 1);
  ents[h].ptr = ptr;
  ents[h].id  = id;
}

static void objmap_insert(ObjMap *m, const void *ptr, uint32_t id) {
  if (m->count * 2 >= m->cap) {
    uint32_t nc = m->cap * 2;
    ObjMapEntry *ne = (ObjMapEntry *)calloc(nc, sizeof(ObjMapEntry));
    for (uint32_t i = 0; i < m->cap; i++)
      if (m->ents[i].ptr) objmap_insert_raw(ne, nc, m->ents[i].ptr, m->ents[i].id);
    free(m->ents);
    m->ents = ne;
    m->cap  = nc;
  }
  objmap_insert_raw(m->ents, m->cap, ptr, id);
  m->count++;
}

/* -----------------------------------------------------------------------
** Growable write buffer
** --------------------------------------------------------------------- */
typedef struct { uint8_t *data; size_t size; size_t cap; } WBuf;

static void wb_init(WBuf *b) {
  b->cap  = 4096;
  b->size = 0;
  b->data = (uint8_t *)malloc(b->cap);
}

static void wb_ensure(WBuf *b, size_t need) {
  if (b->size + need <= b->cap) return;
  while (b->cap < b->size + need) b->cap *= 2;
  b->data = (uint8_t *)realloc(b->data, b->cap);
}

static void wb_write(WBuf *b, const void *src, size_t n) {
  wb_ensure(b, n);
  memcpy(b->data + b->size, src, n);
  b->size += n;
}

static void wb_u8 (WBuf *b, uint8_t  v) { wb_write(b, &v, 1); }
static void wb_u32(WBuf *b, uint32_t v) { wb_write(b, &v, 4); }
static void wb_i32(WBuf *b, int32_t  v) { wb_write(b, &v, 4); }
static void wb_u64(WBuf *b, uint64_t v) { wb_write(b, &v, 8); }
static void wb_dbl(WBuf *b, double   v) { wb_write(b, &v, 8); }

/* patch a uint32 at a previously recorded position */
static void wb_patch_u32(WBuf *b, size_t pos, uint32_t v) {
  memcpy(b->data + pos, &v, 4);
}

/* -----------------------------------------------------------------------
** Read-only cursor over a byte buffer
** --------------------------------------------------------------------- */
typedef struct { const uint8_t *data; size_t pos; size_t size; } RBuf;

static int rb_ok(const RBuf *b, size_t need) { return b->pos + need <= b->size; }
#define RB_READ(b,T) ({ T _v; memcpy(&_v,(b)->data+(b)->pos,sizeof(T)); (b)->pos+=sizeof(T); _v; })
static uint8_t  rb_u8 (RBuf *b) { return RB_READ(b, uint8_t ); }
static uint32_t rb_u32(RBuf *b) { return RB_READ(b, uint32_t); }
static int32_t  rb_i32(RBuf *b) { return RB_READ(b, int32_t ); }
static uint64_t rb_u64(RBuf *b) { return RB_READ(b, uint64_t); }
static double   rb_dbl(RBuf *b) { return RB_READ(b, double  ); }

/* -----------------------------------------------------------------------
** Serialization state
** --------------------------------------------------------------------- */
typedef struct {
  ObjMap     map;        /* ptr -> id */
  uint32_t   next_id;   /* starts at 1 */
  GCObject **ordered;   /* all discovered objects, ordered[id-1] */
  uint32_t   num_objs;
  uint32_t   cap_objs;
} SerState;

static void ser_init(SerState *s) {
  objmap_init(&s->map);
  s->next_id  = 1;
  s->cap_objs = 1024;
  s->num_objs = 0;
  s->ordered  = (GCObject **)malloc(s->cap_objs * sizeof(GCObject *));
}
static void ser_free(SerState *s) {
  objmap_free(&s->map);
  free(s->ordered);
}

static uint32_t ser_get_id(const SerState *s, const GCObject *o) {
  if (!o) return ID_NULL;
  return objmap_find(&s->map, o);
}

/* Assign an ID to obj if not yet seen; push it to the work-list via queue. */
typedef struct { GCObject **items; uint32_t head, tail, cap; } ObjQ;

static void oq_init(ObjQ *q) {
  q->cap   = 2048;
  q->head  = q->tail = 0;
  q->items = (GCObject **)malloc(q->cap * sizeof(GCObject *));
}
static void oq_free(ObjQ *q) { free(q->items); }
static void oq_push(ObjQ *q, GCObject *o) {
  if (q->tail - q->head >= q->cap) {
    uint32_t n  = q->tail - q->head;
    uint32_t nc = q->cap * 2;
    GCObject **ni = (GCObject **)malloc(nc * sizeof(GCObject *));
    for (uint32_t i = 0; i < n; i++)
      ni[i] = q->items[(q->head + i) & (q->cap - 1)];
    free(q->items);
    q->items = ni; q->cap = nc; q->tail = n; q->head = 0;
  }
  q->items[q->tail++ & (q->cap - 1)] = o;
}
static GCObject *oq_pop(ObjQ *q) {
  if (q->head == q->tail) return NULL;
  return q->items[q->head++ & (q->cap - 1)];
}

static void discover_obj(SerState *s, ObjQ *q, GCObject *o);

static void discover_tv(SerState *s, ObjQ *q, const TValue *v) {
  if (iscollectable(v)) discover_obj(s, q, gcvalue(v));
}

static int is_serializable(GCObject *o) {
  switch (novariant(o->tt)) {
  case LUA_TSTRING:
  case LUA_TTABLE:
  case LUA_TPROTO:
  case LUA_TTHREAD:
  case LUA_TUPVAL:
    return 1;
  case LUA_TFUNCTION:
    return o->tt == LUA_VLCL;   /* Lua closures only */
  default:
    return 0;
  }
}

static void discover_obj(SerState *s, ObjQ *q, GCObject *o) {
  if (!o || objmap_find(&s->map, o)) return;
  if (!is_serializable(o)) return;   /* skip C closures, userdata, etc. */
  uint32_t id = s->next_id++;
  objmap_insert(&s->map, o, id);
  if (s->num_objs >= s->cap_objs) {
    s->cap_objs *= 2;
    s->ordered = (GCObject **)realloc(s->ordered, s->cap_objs * sizeof(GCObject *));
  }
  s->ordered[s->num_objs++] = o;
  oq_push(q, o);
}

static void process_obj(SerState *s, ObjQ *q, GCObject *o) {
  switch (novariant(o->tt)) {
  case LUA_TSTRING: break; /* no outgoing refs */
  case LUA_TTABLE: {
    Table *t = gco2t(o);
    for (unsigned i = 0; i < t->asize; i++) {
      lu_byte tag = *getArrTag(t, i);
      if (!tagisempty(tag)) {
        TValue v; arr2obj(t, i, &v); discover_tv(s, q, &v);
      }
    }
    for (unsigned i = 0; i < sizenode(t); i++) {
      Node *n = &t->node[i];
      if (!keyisnil(n)) {
        TValue k; getnodekey(NULL, &k, n);
        discover_tv(s, q, &k);
        discover_tv(s, q, &n->i_val);
      }
    }
    if (t->metatable) discover_obj(s, q, obj2gco(t->metatable));
    break;
  }
  case LUA_TPROTO: {
    Proto *p = gco2p(o);
    for (int i = 0; i < p->sizek; i++)        discover_tv(s, q, &p->k[i]);
    for (int i = 0; i < p->sizep; i++)        if (p->p[i]) discover_obj(s, q, obj2gco(p->p[i]));
    for (int i = 0; i < p->sizeupvalues; i++) if (p->upvalues[i].name) discover_obj(s, q, obj2gco(p->upvalues[i].name));
    for (int i = 0; i < p->sizelocvars; i++)  if (p->locvars[i].varname) discover_obj(s, q, obj2gco(p->locvars[i].varname));
    if (p->source) discover_obj(s, q, obj2gco(p->source));
    break;
  }
  case LUA_TFUNCTION:
    if (o->tt == LUA_VLCL) {
      LClosure *cl = gco2lcl(o);
      if (cl->p) discover_obj(s, q, obj2gco(cl->p));
      for (int i = 0; i < cl->nupvalues; i++)
        if (cl->upvals[i]) discover_obj(s, q, obj2gco(cl->upvals[i]));
    }
    break;
  case LUA_TUPVAL: {
    UpVal *uv = gco2upv(o);
    if (!upisopen(uv)) discover_tv(s, q, &uv->u.value);
    break;
  }
  case LUA_TTHREAD: {
    lua_State *th = gco2th(o);
    for (StkId p = th->stack.p; p < th->top.p; p++)
      discover_tv(s, q, s2v(p));
    for (UpVal *uv = th->openupval; uv; uv = uv->u.open.next)
      discover_obj(s, q, obj2gco(uv));
    break;
  }
  default: break;
  }
}

static void discover_all(SerState *s, lua_State *L) {
  ObjQ q; oq_init(&q);
  discover_tv(s, &q, &G(L)->l_registry);
  discover_obj(s, &q, obj2gco(mainthread(G(L))));
  for (int i = 0; i < LUA_NUMTYPES; i++)
    if (G(L)->mt[i]) discover_obj(s, &q, obj2gco(G(L)->mt[i]));
  GCObject *o;
  while ((o = oq_pop(&q))) process_obj(s, &q, o);
  oq_free(&q);
}

/* -----------------------------------------------------------------------
** TValue  write
** --------------------------------------------------------------------- */
static void write_tv(WBuf *b, const SerState *s, const TValue *v) {
  if (ttisinteger(v)) {
    wb_u8(b, rawtt(v)); wb_u64(b, (uint64_t)(lua_Integer)ivalue(v));
  } else if (ttisfloat(v)) {
    wb_u8(b, rawtt(v)); wb_dbl(b, fltvalue(v));
  } else if (iscollectable(v)) {
    wb_u8(b, rawtt(v)); wb_u32(b, ser_get_id(s, gcvalue(v)));
  } else if (ttisnil(v) || ttisboolean(v)) {
    wb_u8(b, rawtt(v));
  } else {
    /* light C functions (LUA_VLCF) and any other unsupported type → nil */
    wb_u8(b, LUA_VNIL);
  }
}

/* -----------------------------------------------------------------------
** Per-type write helpers (each writes their data section only)
** Each function is wrapped so we can measure the size: caller writes
**   type(u8) + size_placeholder(u32) + actual data, then patches size.
** --------------------------------------------------------------------- */

static void wobj_string(WBuf *b, TString *ts) {
  size_t len; const char *str = getlstr(ts, len);
  wb_u32(b, (uint32_t)len);
  wb_write(b, str, len);
}

static void wobj_table(WBuf *b, const SerState *s, Table *t) {
  /* count non-nil entries */
  uint32_t cnt = 0;
  for (unsigned i = 0; i < t->asize; i++)
    if (!tagisempty(*getArrTag(t, i))) cnt++;
  for (unsigned i = 0; i < sizenode(t); i++) {
    Node *n = &t->node[i];
    if (!keyisnil(n) && !tagisempty(rawtt(&n->i_val))) cnt++;
  }
  wb_u32(b, cnt);
  /* array entries */
  for (unsigned i = 0; i < t->asize; i++) {
    lu_byte tag = *getArrTag(t, i);
    if (!tagisempty(tag)) {
      TValue key, val;
      setivalue(&key, (lua_Integer)(i + 1));
      arr2obj(t, i, &val);
      write_tv(b, s, &key);
      write_tv(b, s, &val);
    }
  }
  /* hash entries */
  for (unsigned i = 0; i < sizenode(t); i++) {
    Node *n = &t->node[i];
    if (!keyisnil(n) && !tagisempty(rawtt(&n->i_val))) {
      TValue key; getnodekey(NULL, &key, n);
      write_tv(b, s, &key);
      write_tv(b, s, &n->i_val);
    }
  }
  wb_u32(b, t->metatable ? ser_get_id(s, obj2gco(t->metatable)) : ID_NULL);
  wb_u32(b, t->asize);
}

static void wobj_proto(WBuf *b, const SerState *s, Proto *p) {
  wb_u8(b, p->numparams);
  wb_u8(b, (uint8_t)(p->flag & (uint8_t)~PF_FIXED));
  wb_u8(b, p->maxstacksize);
  wb_u32(b, (uint32_t)p->sizecode);
  wb_write(b, p->code, (size_t)p->sizecode * sizeof(Instruction));
  wb_u32(b, (uint32_t)p->sizek);
  for (int i = 0; i < p->sizek; i++) write_tv(b, s, &p->k[i]);
  wb_u32(b, (uint32_t)p->sizep);
  for (int i = 0; i < p->sizep; i++)
    wb_u32(b, p->p[i] ? ser_get_id(s, obj2gco(p->p[i])) : ID_NULL);
  wb_u32(b, (uint32_t)p->sizeupvalues);
  for (int i = 0; i < p->sizeupvalues; i++) {
    wb_u32(b, p->upvalues[i].name ? ser_get_id(s, obj2gco(p->upvalues[i].name)) : ID_NULL);
    wb_u8(b, p->upvalues[i].instack);
    wb_u8(b, p->upvalues[i].idx);
    wb_u8(b, p->upvalues[i].kind);
  }
  wb_u32(b, (uint32_t)p->sizelineinfo);
  if (p->sizelineinfo > 0)
    wb_write(b, p->lineinfo, (size_t)p->sizelineinfo * sizeof(ls_byte));
  wb_u32(b, (uint32_t)p->sizeabslineinfo);
  for (int i = 0; i < p->sizeabslineinfo; i++) {
    wb_i32(b, p->abslineinfo[i].pc);
    wb_i32(b, p->abslineinfo[i].line);
  }
  wb_u32(b, (uint32_t)p->sizelocvars);
  for (int i = 0; i < p->sizelocvars; i++) {
    wb_u32(b, p->locvars[i].varname ? ser_get_id(s, obj2gco(p->locvars[i].varname)) : ID_NULL);
    wb_i32(b, p->locvars[i].startpc);
    wb_i32(b, p->locvars[i].endpc);
  }
  wb_u32(b, p->source ? ser_get_id(s, obj2gco(p->source)) : ID_NULL);
  wb_i32(b, p->linedefined);
  wb_i32(b, p->lastlinedefined);
}

static void wobj_lclosure(WBuf *b, const SerState *s, LClosure *cl) {
  wb_u32(b, cl->p ? ser_get_id(s, obj2gco(cl->p)) : ID_NULL);
  wb_u8(b, cl->nupvalues);
  for (int i = 0; i < cl->nupvalues; i++)
    wb_u32(b, cl->upvals[i] ? ser_get_id(s, obj2gco(cl->upvals[i])) : ID_NULL);
}

static void wobj_upval(WBuf *b, const SerState *s, UpVal *uv) {
  if (upisopen(uv)) {
    /* OBJ_UPVAL_OPEN: no data here; thread writes the stack offset */
  } else {
    write_tv(b, s, &uv->u.value);
  }
}

static void wobj_thread(WBuf *b, const SerState *s, lua_State *th) {
  wb_u8(b, (uint8_t)th->status);
  int32_t nstack = (int32_t)(th->top.p - th->stack.p);
  wb_i32(b, nstack);
  for (int32_t i = 0; i < nstack; i++)
    write_tv(b, s, s2v(th->stack.p + i));
  /* count CI frames */
  int32_t nci = 0;
  for (CallInfo *ci = &th->base_ci; ; ci = ci->next) {
    nci++;
    if (ci == th->ci) break;
  }
  wb_i32(b, nci);
  for (CallInfo *ci = &th->base_ci; ; ci = ci->next) {
    int32_t foff = (int32_t)(ci->func.p - th->stack.p);
    int32_t toff = (int32_t)(ci->top.p  - th->stack.p);
    wb_u8(b, (uint8_t)isLua(ci));
    wb_i32(b, foff);
    wb_i32(b, toff);
    wb_u32(b, ci->callstatus);
    wb_i32(b, ci->u2.funcidx);
    if (isLua(ci)) {
      TValue *fv = s2v(ci->func.p);
      uint32_t proto_id = ID_NULL;
      int32_t pc_off = 0, nextra = ci->u.l.nextraargs;
      if (ttisLclosure(fv)) {
        LClosure *cl = clLvalue(fv);
        proto_id = ser_get_id(s, obj2gco(cl->p));
        pc_off   = (int32_t)(ci->u.l.savedpc - cl->p->code);
      }
      wb_u32(b, proto_id);
      wb_i32(b, pc_off);
      wb_i32(b, nextra);
    }
    if (ci == th->ci) break;
  }
  /* open upvalues */
  uint32_t nopen = 0;
  for (UpVal *uv = th->openupval; uv; uv = uv->u.open.next) nopen++;
  wb_u32(b, nopen);
  for (UpVal *uv = th->openupval; uv; uv = uv->u.open.next) {
    wb_u32(b, ser_get_id(s, obj2gco(uv)));
    /* uv->v.p is TValue*; stack slots are StackValue*; cast to align types */
    wb_i32(b, (int32_t)(cast(StkId, uv->v.p) - th->stack.p));
  }
}

static void write_obj(WBuf *b, const SerState *s, GCObject *o) {
  uint8_t type_code;
  switch (novariant(o->tt)) {
  case LUA_TSTRING:   type_code = OBJ_STRING;   break;
  case LUA_TTABLE:    type_code = OBJ_TABLE;     break;
  case LUA_TPROTO:    type_code = OBJ_PROTO;     break;
  case LUA_TFUNCTION: type_code = (o->tt == LUA_VLCL) ? OBJ_LCLOSURE : 0; break;
  case LUA_TUPVAL:    type_code = upisopen(gco2upv(o)) ? OBJ_UPVAL_OPEN : OBJ_UPVAL_CLOSED; break;
  case LUA_TTHREAD:   type_code = OBJ_THREAD;   break;
  default:            type_code = 0; break;
  }
  if (type_code == 0) return; /* unsupported – skip */

  wb_u8(b, type_code);
  size_t size_pos = b->size;
  wb_u32(b, 0);           /* placeholder for data size */
  size_t data_start = b->size;

  switch (type_code) {
  case OBJ_STRING:       wobj_string  (b, gco2ts(o)); break;
  case OBJ_TABLE:        wobj_table   (b, s, gco2t(o)); break;
  case OBJ_PROTO:        wobj_proto   (b, s, gco2p(o)); break;
  case OBJ_LCLOSURE:     wobj_lclosure(b, s, gco2lcl(o)); break;
  case OBJ_UPVAL_CLOSED: wobj_upval   (b, s, gco2upv(o)); break;
  case OBJ_UPVAL_OPEN:   /* no data */ break;
  case OBJ_THREAD:       wobj_thread  (b, s, gco2th(o)); break;
  }

  uint32_t data_sz = (uint32_t)(b->size - data_start);
  wb_patch_u32(b, size_pos, data_sz);
}

/* -----------------------------------------------------------------------
** Public save
** --------------------------------------------------------------------- */
int luaser_save(lua_State *L, unsigned char **out_buf, size_t *out_size) {
  SerState s; ser_init(&s);
  discover_all(&s, L);

  WBuf b; wb_init(&b);
  wb_u32(&b, s.num_objs);
  for (uint32_t i = 0; i < s.num_objs; i++)
    write_obj(&b, &s, s.ordered[i]);

  /* roots */
  wb_u32(&b, ser_get_id(&s, gcvalue(&G(L)->l_registry)));
  wb_u32(&b, ser_get_id(&s, obj2gco(mainthread(G(L)))));
  for (int i = 0; i < LUA_NUMTYPES; i++)
    wb_u32(&b, G(L)->mt[i] ? ser_get_id(&s, obj2gco(G(L)->mt[i])) : ID_NULL);

  ser_free(&s);
  *out_buf  = b.data;
  *out_size = b.size;
  return 0;
}


/* =======================================================================
** DESERIALIZATION
** ===================================================================== */

typedef struct {
  RBuf        rb;
  uint32_t    num_objects;
  void      **id_to_ptr;   /* id_to_ptr[id] for id in 1..num_objects */
  uint8_t    *obj_types;   /* obj_types[id-1] */
  size_t     *obj_offsets; /* start of each object's data in rb */
  lua_State  *L;           /* the new state being built */
  uint32_t    main_thread_id;
} DeserState;

/* read a TValue from the read buffer; GC pointers resolved from id_to_ptr */
static TValue ds_read_tv(DeserState *d) {
  TValue v;
  uint8_t tag = rb_u8(&d->rb);
  v.tt_ = tag;
  if (ttisinteger(&v))        { v.value_.i = (lua_Integer)rb_u64(&d->rb); }
  else if (ttisfloat(&v))     { v.value_.n = rb_dbl(&d->rb); }
  else if (iscollectable(&v)) {
    uint32_t id = rb_u32(&d->rb);
    v.value_.gc = id ? (GCObject *)d->id_to_ptr[id] : NULL;
    if (!v.value_.gc) setnilvalue(&v);
  }
  return v;
}

/* -----------------------------------------------------------------------
** Pass-1 helpers: create blank GC objects of correct size
** --------------------------------------------------------------------- */

/* create a blank proto with arrays allocated but not filled */
static Proto *create_proto(DeserState *d) {
  RBuf rb = d->rb; /* local copy for reading just the sizes */
  (void)rb_u8(&rb); (void)rb_u8(&rb); (void)rb_u8(&rb); /* numparams, flag, maxstack */
  int sizecode       = (int)rb_u32(&rb);
  /* skip code */
  rb.pos += (size_t)sizecode * sizeof(Instruction);
  int sizek          = (int)rb_u32(&rb);
  /* skip constants: each is tag(1) + optional 8 bytes */
  for (int i = 0; i < sizek; i++) {
    uint8_t t = rb_u8(&rb);
    TValue tmp; tmp.tt_ = t;
    if (ttisinteger(&tmp) || ttisfloat(&tmp)) rb.pos += 8;
    else if (iscollectable(&tmp))             rb.pos += 4;
  }
  int sizep          = (int)rb_u32(&rb);
  rb.pos += (size_t)sizep * 4;
  int sizeupvalues   = (int)rb_u32(&rb);
  rb.pos += (size_t)sizeupvalues * (4+1+1+1);
  int sizelineinfo   = (int)rb_u32(&rb);
  rb.pos += (size_t)sizelineinfo;
  int sizeabslineinfo = (int)rb_u32(&rb);
  rb.pos += (size_t)sizeabslineinfo * 8;
  int sizelocvars    = (int)rb_u32(&rb);
  (void)sizelocvars;

  lua_State *L = d->L;
  Proto *p = luaF_newproto(L);
  p->sizecode = sizecode;
  if (sizecode > 0)
    p->code = luaM_newvector(L, sizecode, Instruction);
  p->sizek = sizek;
  if (sizek > 0) {
    p->k = luaM_newvector(L, sizek, TValue);
    for (int i = 0; i < sizek; i++) setnilvalue(&p->k[i]);
  }
  p->sizep = sizep;
  if (sizep > 0) {
    p->p = luaM_newvector(L, sizep, Proto *);
    for (int i = 0; i < sizep; i++) p->p[i] = NULL;
  }
  p->sizeupvalues = sizeupvalues;
  if (sizeupvalues > 0) {
    p->upvalues = luaM_newvector(L, sizeupvalues, Upvaldesc);
    for (int i = 0; i < sizeupvalues; i++) {
      p->upvalues[i].name    = NULL;
      p->upvalues[i].instack = 0;
      p->upvalues[i].idx     = 0;
      p->upvalues[i].kind    = 0;
    }
  }
  p->sizelineinfo = sizelineinfo;
  if (sizelineinfo > 0)
    p->lineinfo = luaM_newvector(L, sizelineinfo, ls_byte);
  p->sizeabslineinfo = sizeabslineinfo;
  if (sizeabslineinfo > 0)
    p->abslineinfo = luaM_newvector(L, sizeabslineinfo, AbsLineInfo);
  /* locvars allocated lazily during fill */
  return p;
}

/* -----------------------------------------------------------------------
** Pass-2 fill helpers (rb is positioned at start of this object's data)
** --------------------------------------------------------------------- */

static void fill_proto(DeserState *d, Proto *p) {
  RBuf *rb       = &d->rb;
  p->numparams   = rb_u8(rb);
  p->flag        = rb_u8(rb);
  p->maxstacksize = rb_u8(rb);
  uint32_t sizecode = rb_u32(rb);
  (void)sizecode;
  if (p->sizecode > 0)
    memcpy(p->code, rb->data + rb->pos, (size_t)p->sizecode * sizeof(Instruction));
  rb->pos += (size_t)p->sizecode * sizeof(Instruction);
  /* constants */
  rb_u32(rb); /* sizek already stored */
  for (int i = 0; i < p->sizek; i++)
    p->k[i] = ds_read_tv(d);
  /* nested protos */
  rb_u32(rb);
  for (int i = 0; i < p->sizep; i++) {
    uint32_t id = rb_u32(rb);
    p->p[i] = id ? (Proto *)d->id_to_ptr[id] : NULL;
  }
  /* upvalue descriptors */
  rb_u32(rb);
  for (int i = 0; i < p->sizeupvalues; i++) {
    uint32_t nid = rb_u32(rb);
    p->upvalues[i].name    = nid ? (TString *)d->id_to_ptr[nid] : NULL;
    p->upvalues[i].instack = rb_u8(rb);
    p->upvalues[i].idx     = rb_u8(rb);
    p->upvalues[i].kind    = rb_u8(rb);
  }
  /* line info */
  rb_u32(rb);
  if (p->sizelineinfo > 0)
    memcpy(p->lineinfo, rb->data + rb->pos, (size_t)p->sizelineinfo);
  rb->pos += (size_t)p->sizelineinfo;
  /* abs line info */
  rb_u32(rb);
  for (int i = 0; i < p->sizeabslineinfo; i++) {
    p->abslineinfo[i].pc   = rb_i32(rb);
    p->abslineinfo[i].line = rb_i32(rb);
  }
  /* local vars */
  uint32_t nloc = rb_u32(rb);
  p->sizelocvars = (int)nloc;
  if (nloc > 0) {
    p->locvars = luaM_newvector(d->L, (int)nloc, LocVar);
    for (uint32_t i = 0; i < nloc; i++) {
      uint32_t vid    = rb_u32(rb);
      p->locvars[i].varname = vid ? (TString *)d->id_to_ptr[vid] : NULL;
      p->locvars[i].startpc = rb_i32(rb);
      p->locvars[i].endpc   = rb_i32(rb);
    }
  }
  uint32_t srcid   = rb_u32(rb);
  p->source        = srcid ? (TString *)d->id_to_ptr[srcid] : NULL;
  p->linedefined   = rb_i32(rb);
  p->lastlinedefined = rb_i32(rb);
}

static void fill_lclosure(DeserState *d, LClosure *cl) {
  RBuf *rb = &d->rb;
  uint32_t pid = rb_u32(rb);
  cl->p = pid ? (Proto *)d->id_to_ptr[pid] : NULL;
  uint8_t nuv = rb_u8(rb);
  for (int i = 0; i < nuv; i++) {
    uint32_t uid = rb_u32(rb);
    cl->upvals[i] = uid ? (UpVal *)d->id_to_ptr[uid] : NULL;
  }
}

static void fill_upval_closed(DeserState *d, UpVal *uv) {
  uv->u.value = ds_read_tv(d);
  uv->v.p     = &uv->u.value;
}

static void fill_table(DeserState *d, Table *t) {
  lua_State *L = d->L;
  RBuf *rb     = &d->rb;
  uint32_t cnt = rb_u32(rb);
  for (uint32_t i = 0; i < cnt; i++) {
    TValue key = ds_read_tv(d);
    TValue val = ds_read_tv(d);
    if (ttisinteger(&key))
      luaH_setint(L, t, ivalue(&key), &val);
    else
      luaH_set(L, t, &key, &val);
  }
  uint32_t mt_id  = rb_u32(rb);
  t->metatable    = mt_id ? (Table *)d->id_to_ptr[mt_id] : NULL;
  /* asize_hint: already handled via luaH_resize at creation time */
  (void)rb_u32(rb);
}

static void fill_thread(DeserState *d, lua_State *th, int is_main) {
  (void)is_main;
  lua_State *L = d->L;
  RBuf *rb     = &d->rb;

  TStatus saved_status = (TStatus)rb_u8(rb);  /* read early, re-apply at end */
  int32_t nstack = rb_i32(rb);

  /* Tear down the CI chain without touching stack contents.
  ** luaE_resetthread is not used because it zeroes slot 0 (via setnilvalue2s)
  ** and may shrink the stack below nstack. */
  {
    CallInfo *ci;
    CallInfo *next = th->base_ci.next;
    th->base_ci.next = NULL;
    while ((ci = next) != NULL) {
      next = ci->next;
      luaM_free(th, ci);
      th->nci--;
    }
  }
  th->ci              = &th->base_ci;
  th->base_ci.callstatus = CIST_C;
  th->base_ci.u.c.k  = NULL;
  th->errfunc         = 0;
  th->tbclist.p       = th->stack.p;

  /* Ensure stack is large enough */
  if (nstack > stacksize(th))
    luaD_reallocstack(th, nstack + 4, 1);

  for (int32_t i = 0; i < nstack; i++) {
    TValue v = ds_read_tv(d);
    *s2v(th->stack.p + i) = v;
  }
  th->top.p = th->stack.p + nstack;

  /* CallInfo chain */
  int32_t nci_total = rb_i32(rb);
  CallInfo *cur = &th->base_ci;
  for (int32_t j = 0; j < nci_total; j++) {
    uint8_t is_lua  = rb_u8(rb);
    int32_t foff    = rb_i32(rb);
    int32_t toff    = rb_i32(rb);
    uint32_t cstat  = rb_u32(rb);
    int32_t u2v     = rb_i32(rb);
    uint32_t pid    = ID_NULL;
    int32_t pc_off  = 0;
    int32_t nextra  = 0;
    if (is_lua) {
      pid    = rb_u32(rb);
      pc_off = rb_i32(rb);
      nextra = rb_i32(rb);
    }

    cur->func.p     = th->stack.p + foff;
    cur->top.p      = th->stack.p + toff;
    cur->callstatus = (l_uint32)cstat;
    cur->u2.funcidx = u2v;

    if (is_lua) {
      Proto *proto = pid ? (Proto *)d->id_to_ptr[pid] : NULL;
      cur->u.l.savedpc    = proto ? proto->code + pc_off : NULL;
      cur->u.l.nextraargs = nextra;
      cur->u.l.trap       = 0;
    } else {
      cur->u.c.k           = NULL;
      cur->u.c.old_errfunc = 0;
      cur->u.c.ctx         = 0;
    }

    th->ci = cur;

    /* allocate next CI if more to come (luaE_extendCI increments th->nci) */
    if (j + 1 < nci_total) {
      cur = luaE_extendCI(th, 1);
    }
  }

  /* open upvalues */
  uint32_t nopen = rb_u32(rb);
  for (uint32_t i = 0; i < nopen; i++) {
    uint32_t uid   = rb_u32(rb);
    int32_t  soff  = rb_i32(rb);
    UpVal *uv      = (UpVal *)d->id_to_ptr[uid];
    if (!uv) continue;
    uv->v.p = s2v(th->stack.p + soff);  /* TValue* into the stack slot */
    /* link into openupval list (sorted by stack pos, higher = closer to head) */
    UpVal **pp = &th->openupval;
    while (*pp && (*pp)->v.p > uv->v.p)
      pp = &(*pp)->u.open.next;
    uv->u.open.next     = *pp;
    uv->u.open.previous = pp;
    if (*pp) (*pp)->u.open.previous = &uv->u.open.next;
    *pp = uv;
  }
  /* add to G->twups if has open upvalues */
  if (th->openupval && !isintwups(th)) {
    th->twups   = G(L)->twups;
    G(L)->twups = th;
  }

  /* restore the original thread status */
  th->status = saved_status;
}

/* -----------------------------------------------------------------------
** luaser_load
** --------------------------------------------------------------------- */
lua_State *luaser_load(const unsigned char *buf, size_t size) {
  DeserState d;
  memset(&d, 0, sizeof(d));
  d.rb.data = buf;
  d.rb.pos  = 0;
  d.rb.size = size;

  if (!rb_ok(&d.rb, 4)) return NULL;
  d.num_objects = rb_u32(&d.rb);

  /* Read object type + size + record offsets, don't parse data yet */
  d.obj_types   = (uint8_t *)malloc(d.num_objects);
  d.obj_offsets = (size_t   *)malloc(d.num_objects * sizeof(size_t));
  if (!d.obj_types || !d.obj_offsets) goto fail_pre;

  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (!rb_ok(&d.rb, 5)) goto fail_pre;
    d.obj_types[i]   = rb_u8(&d.rb);
    uint32_t dsz     = rb_u32(&d.rb);
    d.obj_offsets[i] = d.rb.pos;
    d.rb.pos        += dsz;
  }

  /* Read roots section */
  if (!rb_ok(&d.rb, (4 + 4 + LUA_NUMTYPES * 4))) goto fail_pre;
  uint32_t registry_id     = rb_u32(&d.rb);
  uint32_t main_thread_id  = rb_u32(&d.rb);
  uint32_t mt_ids[LUA_NUMTYPES];
  for (int i = 0; i < LUA_NUMTYPES; i++) mt_ids[i] = rb_u32(&d.rb);

  /* Create new lua_State */
  lua_State *L = luaL_newstate();
  if (!L) goto fail_pre;
  d.L              = L;
  d.main_thread_id = main_thread_id;

  /* Stop GC during reconstruction */
  lua_gc(L, LUA_GCSTOP, 0);

  /* Allocate id-to-pointer table (ids are 1-based) */
  d.id_to_ptr = (void **)calloc(d.num_objects + 1, sizeof(void *));
  if (!d.id_to_ptr) goto fail_L;

  /* Map main thread id to the new state's main thread */
  if (main_thread_id && main_thread_id <= d.num_objects)
    d.id_to_ptr[main_thread_id] = mainthread(G(L));

  /* ----------------------------------------------------------------
  ** Pass 1: create blank objects
  ** -------------------------------------------------------------- */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    uint32_t id = i + 1;
    if (id == main_thread_id) continue; /* already handled */

    uint8_t  tc  = d.obj_types[i];
    d.rb.pos     = d.obj_offsets[i]; /* position at data start */

    switch (tc) {
    case OBJ_STRING: {
      uint32_t len = rb_u32(&d.rb);
      TString *ts  = luaS_newlstr(L, (const char *)(d.rb.data + d.rb.pos), len);
      d.id_to_ptr[id] = ts;
      break;
    }
    case OBJ_TABLE: {
      /* read num_entries to compute asize_hint position */
      uint32_t cnt = rb_u32(&d.rb);
      /* skip entries: tag + optional 8/4 bytes, twice per entry */
      for (uint32_t e = 0; e < cnt * 2; e++) {
        uint8_t t = rb_u8(&d.rb);
        TValue tmp; tmp.tt_ = t;
        if (ttisinteger(&tmp) || ttisfloat(&tmp)) d.rb.pos += 8;
        else if (iscollectable(&tmp))             d.rb.pos += 4;
      }
      (void)rb_u32(&d.rb); /* mt_id */
      uint32_t asize = rb_u32(&d.rb);
      Table *t = luaH_new(L);
      if (asize > 0) luaH_resize(L, t, asize, 0);
      d.id_to_ptr[id] = t;
      break;
    }
    case OBJ_PROTO: {
      Proto *p        = create_proto(&d);
      d.id_to_ptr[id] = p;
      break;
    }
    case OBJ_LCLOSURE: {
      (void)rb_u32(&d.rb); /* proto_id */
      uint8_t nuv  = rb_u8(&d.rb);
      LClosure *cl = luaF_newLclosure(L, nuv);
      cl->p = NULL;
      for (int j = 0; j < nuv; j++) cl->upvals[j] = NULL;
      d.id_to_ptr[id] = cl;
      break;
    }
    case OBJ_UPVAL_CLOSED: {
      UpVal *uv        = (UpVal *)luaC_newobj(L, LUA_VUPVAL, sizeof(UpVal));
      uv->v.p          = &uv->u.value;
      setnilvalue(&uv->u.value);
      d.id_to_ptr[id]  = uv;
      break;
    }
    case OBJ_UPVAL_OPEN: {
      UpVal *uv        = (UpVal *)luaC_newobj(L, LUA_VUPVAL, sizeof(UpVal));
      uv->v.p          = &uv->u.value; /* placeholder; fixed in fill_thread */
      setnilvalue(&uv->u.value);
      uv->u.open.next     = NULL;
      uv->u.open.previous = NULL;
      d.id_to_ptr[id]  = uv;
      break;
    }
    case OBJ_THREAD: {
      /* create a new coroutine; push onto L's stack temporarily */
      lua_State *th    = lua_newthread(L);
      lua_pop(L, 1);
      d.id_to_ptr[id]  = th;
      break;
    }
    default: break;
    }
  }

  /* ----------------------------------------------------------------
  ** Pass 2a: fill protos (needed before threads restore savedpc)
  ** -------------------------------------------------------------- */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (d.obj_types[i] != OBJ_PROTO) continue;
    d.rb.pos = d.obj_offsets[i];
    fill_proto(&d, (Proto *)d.id_to_ptr[i + 1]);
  }

  /* ----------------------------------------------------------------
  ** Pass 2b: fill closed upvalues
  ** -------------------------------------------------------------- */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (d.obj_types[i] != OBJ_UPVAL_CLOSED) continue;
    d.rb.pos = d.obj_offsets[i];
    fill_upval_closed(&d, (UpVal *)d.id_to_ptr[i + 1]);
  }

  /* ----------------------------------------------------------------
  ** Pass 2c: fill closures
  ** -------------------------------------------------------------- */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (d.obj_types[i] != OBJ_LCLOSURE) continue;
    d.rb.pos = d.obj_offsets[i];
    fill_lclosure(&d, (LClosure *)d.id_to_ptr[i + 1]);
  }

  /* ----------------------------------------------------------------
  ** Pass 2d: fill tables
  ** -------------------------------------------------------------- */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (d.obj_types[i] != OBJ_TABLE) continue;
    d.rb.pos = d.obj_offsets[i];
    fill_table(&d, (Table *)d.id_to_ptr[i + 1]);
  }

  /* ----------------------------------------------------------------
  ** Pass 2e: fill threads (main thread first, then coroutines)
  ** -------------------------------------------------------------- */
  /* main thread */
  if (main_thread_id && main_thread_id <= d.num_objects) {
    uint32_t i = main_thread_id - 1;
    d.rb.pos   = d.obj_offsets[i];
    /* status byte was first; reset it after resetthread */
    uint8_t saved_status = d.obj_types[i]; /* just access offset */
    (void)saved_status;
    fill_thread(&d, mainthread(G(L)), 1);
  }
  /* other threads */
  for (uint32_t i = 0; i < d.num_objects; i++) {
    if (d.obj_types[i] != OBJ_THREAD) continue;
    uint32_t id = i + 1;
    if (id == main_thread_id) continue;
    d.rb.pos = d.obj_offsets[i];
    fill_thread(&d, (lua_State *)d.id_to_ptr[id], 0);
  }

  /* ----------------------------------------------------------------
  ** Swap in the restored registry and type metatables
  ** -------------------------------------------------------------- */
  if (registry_id)
    sethvalue(L, &G(L)->l_registry, (Table *)d.id_to_ptr[registry_id]);
  for (int i = 0; i < LUA_NUMTYPES; i++)
    G(L)->mt[i] = mt_ids[i] ? (Table *)d.id_to_ptr[mt_ids[i]] : NULL;

  /* restart GC and do a full cycle to collect transitional garbage */
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  free(d.id_to_ptr);
  free(d.obj_types);
  free(d.obj_offsets);
  return L;

fail_L:
  lua_close(L);
fail_pre:
  free(d.obj_types);
  free(d.obj_offsets);
  free(d.id_to_ptr);
  return NULL;
}
