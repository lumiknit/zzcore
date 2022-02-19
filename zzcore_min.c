// ----------------------
// -- zzcore_min.c 0.0.1
// -- authour: lumiknit
 
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __L_ZZCORE_H__
#define __L_ZZCORE_H__
#include <stdint.h>
#include <limits.h>
typedef uintptr_t zu_t;
typedef uint8_t zb_t;
typedef intptr_t zi_t;
typedef void* zp_t;
#if UINTPTR_MAX >= 0xffffffffffffffff
#define ZZ_SZPTR 8
  typedef double zf_t;
#elif UINTPTR_MAX >= 0xffffffff
#define ZZ_SZPTR 4
  typedef float zf_t;
#else
  #error Unsupported pointer size
#endif
#define zBytesToWords(n) (((n) + ZZ_SZPTR - 1) / ZZ_SZPTR)
#define zWordsToBytes(n) ((n) * ZZ_SZPTR)
typedef union ztag {
  zu_t u; zi_t i; zf_t f; zp_t p;
  struct ztup *t; struct zstr *s;
  zb_t b[0];
} ztag_t;
typedef struct zgc zgc_t;
zgc_t* zNewGC(zu_t  , zu_t  );
void zDelGC(zgc_t*);
zu_t* zAlloc(zgc_t*, zu_t  , zu_t  );
int zRunGC(zgc_t*);
int zFullGC(zgc_t*);
void zGCPushFrame(zgc_t*, int  );
void zGCPopFrame(zgc_t*);
int zGCTopFrameSize(zgc_t*);
int zGCBotFrameSize(zgc_t*);
ztag_t zGCTopFrame(zgc_t*, int  );
ztag_t zGCBotFrame(zgc_t*, int  );
void zGCSetTopFrame(zgc_t*, int  , ztag_t  , int  );
void zGCSetBotFrame(zgc_t*, int  , ztag_t  , int  );
void zSetMajorMinSizeGC(zgc_t*, zu_t  );
int zAllowCyclicRefGC(zgc_t*, int);
zu_t zGCNGen(zgc_t*); 
zu_t zGCReservedSlots(zgc_t*, int  );
zu_t zGCLeftSlots(zgc_t*, int  );
zu_t zGCAllocatedSlots(zgc_t*, int  );
void zPrintGCStatus(zgc_t*, zu_t *dst);
typedef struct ztup {
  ztag_t tag;
  struct ztup *slots[0];
} ztup_t;
ztup_t *zAllocTup(zgc_t*, zu_t  , zu_t  );
typedef struct zstr {
  zu_t len;
  char c[1];
} zstr_t;
zstr_t *zAllocStr(zgc_t*, zu_t  );
#endif
const static int ZZ_DEFAULT_MINOR_HEAP_SIZE = 1 << 18; 
const static int ZZ_DEFAULT_MAJOR_HEAP_SIZE = 1 << 18;  
const static int ZZ_N_GENS = 8;
const static int ZZ_HEAP_MIN_SIZE = 16; 
const static int ZZ_MARK_STK_BOT_SIZE = 512; 
const static zu_t ZZ_NEW_HEAP_SIZE_FACTOR = 3; 
const static zu_t ZZ_HEAP_EMPTY_LIMIT_INV = 5; 
typedef struct zgen { 
  zu_t size; 
  zu_t left; 
  zb_t *m; 
  zb_t *s; 
  zu_t *p; 
  zu_t n_reachables; 
  zb_t *body;
} zgen_t;
typedef struct zframe { 
  struct zframe *prev;
  int size; 
  zb_t *s; 
  ztag_t *v; 
  zp_t p[1]; 
} zframe_t;
typedef struct zgc {
  zu_t major_heap_min_size; 
  int has_cyclic_ref; 
  int sz_gens, n_gens; 
  zgen_t **gens;
  zframe_t *bot_frame, *top_frame;
  zi_t mark_sp, sz_mark_stk;
  zp_t *mark_stk;
  int gc_target; 
  int mark_top; 
  int move_top; 
  zu_t n_collection;
} zgc_t;
#define ZZ_COLOR 0xff 
#define ZZ_NEG_COLOR (0xff ^ ZZ_COLOR)
#define ZZ_GRAY 0x02 
#define ZZ_BLACK 0x01
#define ZZ_WHITE 0x00
#define ZZ_NPTR 0x01 
#define ZZ_SEP 0x02 
static zgen_t* zNewGen(zu_t sz) {
  zgen_t *X = (zgen_t*) malloc(sizeof(zgen_t));
  zu_t alloc_sz = (sizeof(zu_t) + sizeof(zb_t) * 2) * (sz + 1);
  zu_t *b = (zu_t*) malloc(alloc_sz);
  memset(b, 0x00, (sizeof(zb_t) * 2) * (sz + 1));
  if(X == NULL || b == NULL) goto L_fail;
  X->size = X->left = sz;
  X->body = (zp_t) b;
  X->n_reachables = 0;
  X->m = (zb_t*) b;
  X->s = (zb_t*) (X->m + (sz + 1));
  X->p = (zu_t*) (X->m + (sz + 1) * 2);
  X->m[X->size] = ZZ_COLOR;
  X->s[X->size] = ZZ_SEP;
  X->p[X->size] = 0xFA15E;
  return X;
L_fail:
  return NULL;
}
static void zDelGen(zgen_t *X) {
  free(X->body);
  free(X);
}
static zu_t* zGenAlloc(zgen_t *X, zu_t np, zu_t p) {
  if(X->left < np + p) return NULL;
  X->left -= np + p;
  memset(X->s + X->left, ZZ_NPTR, sizeof(zb_t) * np);
  X->s[X->left] |= ZZ_SEP;
  return X->p + X->left;
}
static void zGenCleanMarks(zgen_t *X) {
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->n_reachables = 0;
}
static void zGenCleanAll(zgen_t *X) {
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->s + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->left = X->size;
  X->n_reachables = 0;
}
static zi_t zGenPtrIdx(zgen_t *X, zp_t p) {
  const zu_t px = ((zu_t) p - (zu_t) X->p) / sizeof(zp_t);
  return px >= X->size || px < X->left ? -1 : px;
}
static zframe_t* zNewFrame(int sz, zframe_t *prev) {
  zu_t asz = sizeof(zframe_t) + (sizeof(zp_t) + sizeof(zb_t)) * sz;
  zframe_t *f = (zframe_t*) malloc(asz);
  memset(f, 0x00, asz);
  f->size = sz;
  f->prev = prev;
  f->s = (zb_t*) (f + 1);
  f->v = (ztag_t*) (f->s + sz);
  return f;
}
zgc_t* zNewGC(zu_t sz_roots, zu_t sz_minor) {
  zgc_t *G = (zgc_t*) malloc(sizeof(zgc_t));
  zgen_t **gens = (zgen_t**) malloc(sizeof(zgen_t*) * ZZ_N_GENS);
  zframe_t *bot_frame = zNewFrame(sz_roots, NULL);
  zp_t *stk = (zp_t*) malloc(sizeof(zp_t) * ZZ_MARK_STK_BOT_SIZE);
  if(sz_minor <= ZZ_HEAP_MIN_SIZE) sz_minor = ZZ_DEFAULT_MINOR_HEAP_SIZE;
  zgen_t *minor = zNewGen(sz_minor);
  if(!G || !gens || !bot_frame || !stk || !minor) goto L_fail;
  memset(gens, 0x00, sizeof(zgen_t*) * ZZ_N_GENS);
  gens[0] = minor;
  G->gens = gens;
  G->bot_frame = G->top_frame = bot_frame;
  G->major_heap_min_size = ZZ_DEFAULT_MAJOR_HEAP_SIZE;
  G->sz_gens = ZZ_N_GENS, G->n_gens = 1;
  G->mark_stk = stk;
  stk[0] = stk[ZZ_MARK_STK_BOT_SIZE - 1] = NULL;
  G->mark_sp = 1;
  G->sz_mark_stk = ZZ_MARK_STK_BOT_SIZE;
  G->n_collection = 0;
  return G;
L_fail:
  if(G) free(G);
  if(gens) free(gens);
  if(bot_frame) free(bot_frame);
  return NULL;
}
void zDelGC(zgc_t *G) {
  int k;
  for(k = 0; k < G->n_gens; k++)
    zDelGen(G->gens[k]);
  free(G->gens);
  zframe_t *f;
  while(G->top_frame) {
    f = G->top_frame;
    G->top_frame = f->prev;
    free(f);
  }
  free(G);
}
void zSetMajorMinSizeGC(zgc_t *G, zu_t msz) {
  if(msz >= ZZ_HEAP_MIN_SIZE) G->major_heap_min_size = msz;
}
zu_t* zAlloc(zgc_t *G, zu_t np, zu_t p) {
  const zu_t sz = np + p;
  zgen_t * const minor = G->gens[0];
  if(sz >= minor->size) {
    int k;
    if(!G->has_cyclic_ref && p > 0) {
      if(zRunGC(G) < 0) return NULL;
    } else {
      zu_t *ptr;
      for(k = 1; k < G->n_gens; k++) {
        if((ptr = zGenAlloc(G->gens[k], np, p))) return ptr;
      }
    }
    zgen_t *J = zNewGen(sz * ZZ_NEW_HEAP_SIZE_FACTOR);
    if(J == NULL) return NULL;
    for(k = G->n_gens; k >= 2; k--) {
      G->gens[k] = G->gens[k - 1];
    }
    G->gens[1] = J;
    G->n_gens++;
    return zGenAlloc(J, np, p);
  }
  if(minor->left < sz) zRunGC(G);
  minor->left -= sz;
  memset(minor->s + minor->left, ZZ_NPTR, sizeof(zb_t) * np);
  minor->s[minor->left] |= ZZ_SEP;
  return minor->p + minor->left;
}
static void zMarkStkPush(zgc_t *G, int gen, zu_t idx) {
  if(G->mark_sp >= G->sz_mark_stk - 1) {
    if(G->mark_stk[G->sz_mark_stk - 1]) {
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1];
    } else {
      zp_t *fr = (zp_t*) malloc(sizeof(zp_t) * (G->sz_mark_stk << 1));
      if(fr == NULL) return;
      fr[0] = G->mark_stk;
      fr[(G->sz_mark_stk << 1) - 1] = NULL;
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1] = fr;
    }
    G->mark_sp = 1, G->sz_mark_stk <<= 1;
  }
  G->mark_stk[G->mark_sp] = (zp_t) (zi_t) gen;
  G->mark_stk[G->mark_sp + 1] = (zp_t) idx;
  G->mark_sp += 2;
}
static int zMarkStkPop(zgc_t *G, int *gen, zu_t *idx) {
  if(G->mark_sp <= 1) {
    if(G->mark_stk[0] == NULL) return 0;
    G->mark_stk = G->mark_stk[0], G->sz_mark_stk >>= 1;
    G->mark_sp = G->sz_mark_stk - 1;
  }
  G->mark_sp -= 2;
  *idx = (int) (zi_t) G->mark_stk[G->mark_sp + 1];
  *gen = (zu_t) G->mark_stk[G->mark_sp];
  return 1;
}
static void zMarkStkClean(zgc_t *G) {
  while(G->mark_stk[G->sz_mark_stk - 1]) {
    G->mark_stk = G->mark_stk[G->sz_mark_stk - 1];
    G->sz_mark_stk <<= 1;
  }
  while(G->mark_stk[0]) {
    G->mark_stk = G->mark_stk[0];
    G->sz_mark_stk >>= 1;
    free(G->mark_stk[G->sz_mark_stk - 1]);
    G->mark_stk[G->sz_mark_stk - 1] = NULL;
  }
  G->mark_sp = 1;
}
static int zMarkPropagate(zgc_t *G, int gen, zu_t idx) {
  zgen_t * const J = G->gens[gen];
  zu_t xoff = idx;
  const zu_t kf = G->has_cyclic_ref ? 0 : gen;
  do {
    if(!(J->s[xoff] & ZZ_NPTR)) { 
      const zp_t ref = (zp_t) J->p[xoff];
      zu_t k;
      for(k = kf; k < G->mark_top; k++) {
        zgen_t * const K = G->gens[k];
        const zi_t idy = zGenPtrIdx(K, ref);
        if(idy >= 0 && (K->s[idy] & ZZ_SEP) &&
            (K->m[idy] == ZZ_WHITE)) {
          K->m[idy] = ZZ_BLACK;
          zMarkStkPush(G, k, idy);
          break;
    } } }
  } while(!(J->s[++xoff] & ZZ_SEP));
  J->n_reachables += xoff - idx;
  return 0;
}
static int zMarkGC(zgc_t *G) {
  zframe_t *f;
  int j, k;
  int gen;
  zu_t idx;
  for(f = G->top_frame; f; f = f->prev) {
    for(k = 0; k < f->size; k++) {
      if(!(f->s[k] & ZZ_NPTR)) {
        for(j = 0; j < G->mark_top; j++) {
          zgen_t * const J = G->gens[j];
          const zi_t idy = zGenPtrIdx(J, f->v[k].p);
          if(idy >= 0 && (J->s[idy] & ZZ_SEP) &&
            (J->m[idy] == ZZ_WHITE)) {
            J->m[idy] = ZZ_BLACK;
            zMarkPropagate(G, j, idy);
            while(zMarkStkPop(G, &gen, &idx)) {
              zMarkPropagate(G, gen, idx);
  } } } } } }
  zMarkStkClean(G);
  return 0;
}
static zu_t zFindTopEmptyGenByAlloc(zgc_t *G) {
  int k = G->gc_target;
  zu_t acc = G->gens[k]->size - G->gens[k]->left;
  for(k++; k < G->n_gens && acc > G->gens[k]->left; k++)
    acc += G->gens[k]->size - G->gens[k]->left;
  return k;
}
static zu_t zFindTopEmptyGenByReachable(zgc_t *G) {
  int k = G->gc_target;
  zu_t acc = G->gens[k++]->n_reachables;
  for(; k < G->n_gens && acc > G->gens[k]->left; k++)
    acc += G->gens[k]->n_reachables;
  return k;
}
static int zReallocGenGC(zgc_t *G, zgen_t *dst, zgen_t *src) {
  zu_t off = src->left;
  zu_t p = 0;
  const zu_t lim = src->size;
  while(off < lim) {
    if(src->m[off]) {
      p = off;
      while(p < lim && (!(src->s[p] & ZZ_SEP) || src->m[p])) p++;
      const zu_t sz = p - off;
      dst->left -= sz;
      memcpy(dst->s + dst->left, src->s + off, sizeof(zb_t) * sz);
      memcpy(dst->p + dst->left, src->p + off, sizeof(zu_t) * sz);
      const zu_t *dp = dst->p + dst->left - off;
      for(; off < p; off++) {
        if(src->s[off] & ZZ_SEP) src->p[off] = (zu_t) (dp + off);
      }
    } else off++;
  }
  return 0;
}
static void zGenUpdatePointers(zgc_t *G, zgen_t *J) {
  int k;
  zu_t off = J->left;
  const zu_t sz = J->size;
  zb_t * const s = J->s;
  zu_t * const p = J->p;
  const int tgt = G->gc_target, top = G->move_top;
  for(; off < sz; off++) {
    if(!(s[off] & ZZ_NPTR)) {
      const zp_t ptr = (zp_t) p[off];
      for(k = tgt; k < top; k++) {
        zgen_t * const K = G->gens[k];
        const zi_t idx = zGenPtrIdx(K, ptr);
        if(idx >= 0) {
          p[off] = (zu_t) K->p[idx];
          break;
} } } } }
static void zUpdateRootPointers(zgc_t *G) {
  int k;
  zframe_t *f;
  for(f = G->top_frame; f; f = f->prev) {
    zu_t i;
    for(i = 0; i < f->size; i++) {
      if(!(f->s[i] & ZZ_NPTR)) {
        const zp_t ptr = (zp_t) f->v[i].p;
        for(k = G->gc_target; k < G->move_top; k++) {
          zgen_t * const K = G->gens[k];
          const zi_t idx = zGenPtrIdx(K, ptr);
          if(idx >= 0) {
            f->v[i].u = K->p[idx];
            break;
} } } } } }
static int zMoveGC(zgc_t *G) {
  int j, k;
  zgen_t *dst;
  int bot = G->gc_target, top = G->move_top;
  if(top >= G->n_gens) {
    zu_t sz = 0;
    for(k = bot; k < top; k++) sz += G->gens[k]->n_reachables;
    sz *= ZZ_NEW_HEAP_SIZE_FACTOR;
    if(sz < G->major_heap_min_size) sz = G->major_heap_min_size;
    if((dst = zNewGen(sz)) == NULL) return -1;
    if(top >= G->sz_gens) {
      G->gens = realloc(G->gens, sizeof(zgen_t**) * (G->sz_gens << 1));
      G->sz_gens <<= 1;
    }
    G->gens[top] = dst;
    G->n_gens++;
  } else dst = G->gens[top];
  for(j = top - 1; j >= (zi_t) bot; j--) {
    if(zReallocGenGC(G, dst, G->gens[j]) < 0) return -1;
  }
  const int jt = G->has_cyclic_ref ? G->n_gens : top + 1;
  for(j = 0; j < bot; j++) zGenUpdatePointers(G, G->gens[j]);
  for(j = top; j < jt; j++) zGenUpdatePointers(G, G->gens[j]);
  zUpdateRootPointers(G);
  for(k = 0; k < bot; k++) zGenCleanMarks(G->gens[k]);
  for(; k < top; k++) zGenCleanAll(G->gens[k]);
  for(; k < G->n_gens; k++) zGenCleanMarks(G->gens[k]);
  return 0;
}
static int zReduceEmptyGC(zgc_t *G) {
  int k;
  zu_t total = 0, allocated = 0;
  for(k = 1; k < G->n_gens; k++) {
    total += G->gens[k]->size;
    allocated += G->gens[k]->size - G->gens[k]->left;
  }
  for(k = G->n_gens - 1;
      k >= 1 && total > allocated * ZZ_HEAP_EMPTY_LIMIT_INV; k--) {
    if(G->gens[k]->left == G->gens[k]->size) {
      total -= G->gens[k]->size;
      zDelGen(G->gens[k]);
      G->gens[k] = NULL;
    }
  }
  int d = 0;
  for(k = 1; k < G->n_gens; k++) {
    if(G->gens[k] == NULL) d++;
    else G->gens[k - d] = G->gens[k];
  }
  G->n_gens -= d;
  return 0;
}
int zRunGC(zgc_t *G) {
  if(G->gens[0]->left >= G->gens[0]->size) return 1;
  G->gc_target = 0;
  G->mark_top = G->has_cyclic_ref ?
    G->n_gens : zFindTopEmptyGenByAlloc(G);
  if(zMarkGC(G) < 0) return -1;
  G->move_top = zFindTopEmptyGenByReachable(G);
  if(zMoveGC(G) < 0 || zReduceEmptyGC(G) < 0) return -1;
  ++G->n_collection;
  return 0;
}
int zFullGC(zgc_t *G) {
  G->gc_target = 0;
  G->mark_top = G->move_top = G->n_gens;
  if(zMarkGC(G) < 0 || zMoveGC(G) < 0 || zReduceEmptyGC(G) < 0)
    return -1;
  ++G->n_collection;
  return 0;
}
void zGCPushFrame(zgc_t *G, int sz) {
  G->top_frame = zNewFrame(sz, G->top_frame);
}
void zGCPopFrame(zgc_t *G) {
  if(G->top_frame != NULL && G->top_frame != G->bot_frame) {
    zframe_t *f = G->top_frame;
    G->top_frame = f->prev;
    free(f);
  }
}
int zGCTopFrameSize(zgc_t *G) {
  return G->top_frame->size;
}
int zGCBotFrameSize(zgc_t *G) {
  return G->bot_frame->size;
}
ztag_t zGCTopFrame(zgc_t *G, int idx) {
  return G->top_frame->v[idx];
}
ztag_t zGCBotFrame(zgc_t *G, int idx) {
  return G->bot_frame->v[idx];
}
void zGCSetTopFrame(zgc_t *G, int idx, ztag_t v, int is_nptr) {
  G->top_frame->v[idx] = v;
  G->top_frame->s[idx] = is_nptr ? ZZ_NPTR : 0;
}
void zGCSetBotFrame(zgc_t *G, int idx, ztag_t v, int is_nptr) {
  G->bot_frame->v[idx] = v;
  G->bot_frame->s[idx] = is_nptr ? ZZ_NPTR : 0;
}
int zAllowCyclicRefGC(zgc_t *G, int v) {
  if(v > 0) { 
    G->has_cyclic_ref = 1;
    return 1;
  } else if(v == 0) { 
    if(zFullGC(G) < 0) return -1;
    G->has_cyclic_ref = 0;
    return 0;
  } else { 
    G->has_cyclic_ref = 0;
    return 0;
} }
zu_t zNGen(zgc_t *G) {
  return G->n_gens;
}
zu_t zGCReservedSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->size;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->size;
  return sum;
}
zu_t zGCLeftSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->left;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->left;
  return sum;
}
zu_t zGCAllocatedSlots(zgc_t *G, int idx) {
  return zGCReservedSlots(G, idx) - zGCLeftSlots(G, idx);
}
void zPrintGCStatus(zgc_t *G, zu_t *dst) {
  zu_t arr[4];
  if(dst == NULL) dst = arr;
  dst[0] = zGCReservedSlots(G, -1); dst[1] = zGCLeftSlots(G, -1);
  dst[2] = zGCReservedSlots(G, 0); dst[3] = zGCLeftSlots(G, 0);
  printf("GC Stat (%p, %d gens) [alloc(%%) / left(%%) / total]\n",
    G, G->n_gens);
  zu_t t = dst[0], l = dst[1];
  zu_t a = t - l;
  printf(
    "* Entire: %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "\n",
    a, 100 * (double) a / t, l, 100 * (double) l / t, t);
  zu_t k;
  for(k = 0; k < G->n_gens; k++) {
    t = zGCReservedSlots(G, k), l = zGCLeftSlots(G, k);
    a = t - l;
    printf(
      "* [%" PRIuPTR "]: %" PRIuPTR "(%.2lf%%) / %"
      PRIuPTR "(%.2lf%%) / %" PRIuPTR "\n",
      k, a, 100 * (double) a / t, l, 100 * (double) l / t, t);
  }
}
ztup_t *zAllocTup(zgc_t *G, zu_t tag, zu_t dim) {
  ztup_t *t = (ztup_t*) zAlloc(G, 1, dim);
  t->tag.u = tag;
  return t;
}
zstr_t *zAllocStr(zgc_t *G, zu_t len) {
  zu_t sz = 2 + len / ZZ_SZPTR;
  zstr_t *s = (zstr_t*) zAlloc(G, sz, 0);
  s->len = len;
  s->c[0] = s->c[len] = '\0';
  return s;
}
// ----------------------