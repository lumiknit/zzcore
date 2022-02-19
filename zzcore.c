#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzcore.h"

/* ** ZZGC Description (Rough)
 *  ZZGC is a generational mark-and-copy GC. It has multiple generations, which
 * are one minor gen and multiple major gen. Every new object is allocated in
 * the minor gen. If minor gen is full, it collects garbages.
 *  Collectin n-th gen, it'll try to move all alive objects into the next gen.
 * If (n+1)-th gen is also full, it tries to moves n-th and (n+1)-th objects
 * into (n+2)-th. If (n+2)-th gen is full, n-th to (n+2)-th are moved into (n+3)
 * -th, and so on. In other words, it copies consecutive generations into the
 * next empty generation to make a space. If there is no gen enough free, it
 * allocates new gen that is large enough to contains all objects. At the end of
 * collection, cleaned generations are deallocated.
 *  Each object have references (pointer) and non-ref values. ZZGC consider only
 * ref parts as object references in GC. (Even if a ptr of GC obj is in non-ref
 * part, it's not considered as reference.) Default `zAlloc` fn allocates an
 * object, where ref part follows non-ref one.
 *  It only accept number of pointers for an object size, not number of bytes.
 * So non-ref part must be a multiple of sizeof(ptr) including paddings for
 * correct collection.
 *  If there is no cyclic reference between generations, elder objects must
 * contains correct pointers even if there was a lot of move. Thus, ZZGC does
 * not traverse elder gens and enhances a performance in this case.
 * However, there exist various cases obtaining cyclic references: cyclic list,
 * mutual recursive closure, mutable object, etc. If a program has a possiblity
 * of cyclic refs, user explicitly enable an option to handle this.
 * (If no more cyclic references will occur, the option can be disabled after
 * run full GC.)
 *  At this present, ZZGC is not incremental. However it may be easily changed
 * into incremental version. (?)
 */

typedef struct zgen { // generation structure
  zu_t size; // number of pointers in data
  zu_t left;
  zb_t *m; // marks
  zb_t *s; // stats
  zu_t *p; // value/pointer pools
  // Only for GC
  zu_t n_reachables;
  // memory pool for marks and pointers, m ++ p
  zb_t *body;
} zgen_t;

typedef struct zframe {
  struct zframe *prev;
  zu_t size;
  zb_t *s;
  ztag_t *v;
  zp_t p[1];
} zframe_t;

typedef struct zgc {
  // Options
  zu_t major_heap_min_size; // [1-] Major heap minimum size
  zu_t has_cyclic_ref; // true when there are cyclic references
  // Generations
  zu_t sz_gens, n_gens; // Gens array size & number of gens
  zgen_t **gens;
  // Roots
  zframe_t *bot_frame, *top_frame;
  // Mark stack
  zi_t mark_sp, sz_mark_stk;
  zp_t *mark_stk;
  // GC Info
  zu_t n_collection;
  // GC Temp: used during collection
  zu_t gc_target; // collection target generation
  zu_t mark_top; // max marking generation + 1
  zu_t move_top; // max move generation + 1
} zgc_t;

// GC default options
const static int ZZ_DEFAULT_MINOR_HEAP_SIZE = 1 << 18; // 256k words
const static int ZZ_DEFAULT_MAJOR_HEAP_SIZE = 1 << 20; // 1M words
const static int ZZ_N_GENS = 8;
const static int ZZ_HEAP_MIN_SIZE = 16;

const static int ZZ_MARK_STK_BOT_SIZE = 256;

// Mark constant
#define ZZ_COLOR 0x03 // Color flag
#define ZZ_NEG_COLOR (0xff ^ ZZ_COLOR)
#define ZZ_GRAY 0x02 // Unused
#define ZZ_BLACK 0x01
#define ZZ_WHITE 0x00

// Stat constant
#define ZZ_NPTR 0x01 // Not-pointer flag
#define ZZ_SEP 0x02 // Chunk separator flag

static zgen_t* zNewGen(zu_t sz) {
  zgen_t *X = (zgen_t*) malloc(sizeof(zgen_t));
  zu_t alloc_sz = (sizeof(zu_t) + sizeof(zb_t) * 2) * (sz + 1);
  zu_t *b = (zu_t*) malloc(alloc_sz);
  memset(b, 0x00, alloc_sz);
  if(X == NULL || b == NULL) goto L_fail;
  X->size = X->left = sz;
  X->body = (zp_t) b;
  X->n_reachables = 0;
  X->m = (zb_t*) b;
  X->s = (zb_t*) (X->m + sz);
  X->p = (zu_t*) (X->m + sz * 2);
  // CANARI
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

static zu_t* zGenRealloc(zgen_t *X, zu_t sz, zgen_t *src, zu_t off) {
  if(X->left < sz) return NULL;
  X->left -= sz;
  memcpy(X->s + X->left, src->s + off, sizeof(zb_t) * sz);
  memcpy(X->p + X->left, src->p + off, sizeof(zu_t) * sz);
  return X->p + X->left;
}

static void zGenCleanMarks(zgen_t *X) {
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->n_reachables = 0;
}

static void zGenCleanAll(zgen_t *X) {
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->s + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->p + X->left, 0x00, sizeof(zu_t) * (X->size - X->left));
  X->left = X->size;
  X->n_reachables = 0;
}

static zi_t zGenPtrIdx(zgen_t *X, zp_t p) {
  // Check p is in X and return index of p if so
  if((zu_t) p < (zu_t) X->p ||
     (zu_t) p >= (zu_t) (X->p + X->size)) return -2;
  if((zu_t) p < (zu_t) (X->p + X->left)) return -1;
  return ((zu_t) p - (zu_t) X->p) / sizeof(zp_t);
}

static zi_t zGenIdxSz(zgen_t *X, zi_t idx) {
  if(idx < 0 || idx >= X->size) return -1;
  zi_t off = idx + 1;
  while(off < X->size && !(X->s[off] & ZZ_SEP)) off++;
  return off - idx;
}

static zframe_t* zNewFrame(zu_t sz, zframe_t *prev) {
  zu_t asz = sizeof(zframe_t) + (sizeof(zp_t) + sizeof(zb_t)) * sz;
  zframe_t *f = (zframe_t*) malloc(asz);
  memset(f, 0x00, asz);
  f->size = sz;
  f->prev = prev;
  f->s = (zb_t*) (f + 1);
  f->v = (ztag_t*) (f->s + sz);
  return f;
}

// GC APIs
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
  zu_t k;
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

// Option setter
void zSetMajorMinSizeGC(zgc_t *G, zu_t msz) {
  if(msz >= ZZ_HEAP_MIN_SIZE) G->major_heap_min_size = msz;
}

// Allocation
zu_t* zAlloc(zgc_t *G, zu_t np, zu_t p) {
  zgen_t * const minor = G->gens[0];
  // Check very large chunk required
  if(np + p >= minor->size) {
    zu_t k;
    if(!G->has_cyclic_ref && p > 0) {
      // If cyclic is not allowed and there is ref part,
      // run gc
      if(zRunGC(G) < 0) return NULL;
      // and alloc in new generation
    } else {
      // Try to find a empty space
      zu_t *ptr;
      for(k = 1; k < G->n_gens; k++) {
        if((ptr = zGenAlloc(G->gens[k], np, p))) return ptr;
      }
    }
    // Make a new generation
    zgen_t *J = zNewGen((np + p) * 2);
    if(J == NULL) return NULL;
    for(k = G->n_gens; k >= 2; k--) {
      G->gens[k] = G->gens[k - 1];
    }
    G->gens[1] = J;
    G->n_gens++;
    return zGenAlloc(J, np, p);
  }
  // Try to allocate in minor heap
  zu_t *ptr = zGenAlloc(minor, np, p);
  if(ptr != NULL) return ptr;
  if(zRunGC(G) < 0) return NULL;
  return zGenAlloc(minor, np, p);
}

// Collection

// Mark stack API
static int zMarkStkPush(zgc_t *G, zp_t p) {
  if(G->mark_sp >= G->sz_mark_stk - 1) {
    if(G->mark_stk[G->sz_mark_stk - 1]) {
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1];
    } else {
      zp_t *fr = (zp_t*) malloc(sizeof(zp_t) * (G->sz_mark_stk << 1));
      if(fr == NULL) return -1;
      fr[0] = G->mark_stk;
      fr[(G->sz_mark_stk << 1) - 1] = NULL;
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1] = fr;
    }
    G->mark_sp = 1, G->sz_mark_stk <<= 1;
  }
  G->mark_stk[G->mark_sp++] = p;
  return 0;
}
static zp_t zMarkStkPop(zgc_t *G) {
  if(G->mark_sp <= 1) {
    if(G->mark_stk[0] == NULL) return NULL;
    G->mark_stk = G->mark_stk[0], G->sz_mark_stk >>= 1;
    G->mark_sp = G->sz_mark_stk - 1;
  }
  return G->mark_stk[--G->mark_sp];
}
static int zMarkStkIsNonEmpty(zgc_t *G) {
  return G->mark_sp > 1 || G->mark_stk[0];
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
  assert(G->sz_mark_stk == ZZ_MARK_STK_BOT_SIZE);
  G->mark_sp = 1;
}

static int zMarkPropagate(zgc_t *G, zp_t p) {
  // Propagation of marking in black
  if(p == NULL) return 1;
  // Find generation & index
  zu_t j;
  for(j = 0; j < G->mark_top; j++) {
    zgen_t * const J = G->gens[j];
    const zi_t idx = zGenPtrIdx(J, p);
    if(idx >= 0) {
      // p must be at the first of chunk
      assert(J->s[idx] & ZZ_SEP);
      // Mark p into black
      J->m[idx] |= ZZ_BLACK;
      // Traverse all references from p
      zu_t off = 0;
      zu_t kf = G->has_cyclic_ref ? 0 : j;
      do {
        if(!(J->s[idx + off] & ZZ_NPTR)) { // Ignore non-pointer slots
          // Find generation & index of ref
          zu_t k;
          for(k = kf; k < G->mark_top; k++) {
            zgen_t * const K = G->gens[k];
            const zi_t idy = zGenPtrIdx(K, (zp_t) J->p[idx + off]);
            // Check ref is not visited
            if(idy >= 0 && (K->s[idy] & ZZ_SEP) &&
               ((K->m[idy] & ZZ_COLOR) == ZZ_WHITE)) {
              // Mark black
              // (For incremental GC, it should be ZZ_GRAY)
              K->m[idy] |= ZZ_BLACK;
              zMarkStkPush(G, (zp_t) J->p[idx + off]);
              break;
        } } }
      } while(!(J->s[idx + (++off)] & ZZ_SEP));
      J->n_reachables += off;
      return 0;
  } }
  return 1;
}

static int zMarkGC(zgc_t *G) {
  // Push all roots into stack
  zframe_t *f;
  zu_t k;
  for(f = G->top_frame; f; f = f->prev) {
    for(k = 0; k < f->size; k++)
      if(!(f->s[k] & ZZ_NPTR))
        zMarkStkPush(G, f->v[k].p);
  }
  // Pop and marking
  while(zMarkStkIsNonEmpty(G)) {
    zp_t p = zMarkStkPop(G);
    zMarkPropagate(G, p);
  }
  // Cleanup stack
  zMarkStkClean(G);
  return 0;
}

static zu_t zFindTopEmptyGenByAlloc(zgc_t *G) {
  zu_t k = G->gc_target;
  zu_t acc = G->gens[k]->size - G->gens[k]->left;
  for(k++; k < G->n_gens && acc > G->gens[k]->left; k++)
    acc += G->gens[k]->size - G->gens[k]->left;
  return k;
}

static zu_t zFindTopEmptyGenByReachable(zgc_t *G) {
  zu_t k = G->gc_target;
  zu_t acc = G->gens[k++]->n_reachables;
  for(; k < G->n_gens && acc > G->gens[k]->left; k++)
    acc += G->gens[k]->n_reachables;
  return k;
}

static int zReallocGenGC(zgc_t *G, zgen_t *dst, zgen_t *src) {
  zu_t off = src->left;
  while(off < src->size) {
    assert(src->s[off] & ZZ_SEP);
    // Find next chunk
    zu_t sz = zGenIdxSz(src, off);
    // When the object is reachable
    if((src->m[off] & ZZ_COLOR) != ZZ_WHITE) {
      // Allocate into dst
      zu_t *n = zGenRealloc(dst, sz, src, off);
      assert(n != NULL);
      // Save a new pointer in the old object
      src->p[off] = (zu_t) n;
    }
    off += sz;
  }
  return 0;
}

static zp_t zFindNewPointer(zgc_t *G, zp_t ptr) {
  zu_t k;
  for(k = G->gc_target; k < G->move_top; k++) {
    const zi_t idx = zGenPtrIdx(G->gens[k], ptr);
    if(idx >= 0) return (zp_t) G->gens[k]->p[idx];
  }
  return ptr;
}

static void zGenUpdatePointers(zgc_t *G, zgen_t *J) {
  zu_t off = J->left;
  for(; off < J->size; off++) {
    if(!(J->s[off] & ZZ_NPTR)) {
      J->p[off] = (zu_t) zFindNewPointer(G, (zp_t) J->p[off]);
} } }

static void zUpdateRootPointers(zgc_t *G) {
  zframe_t *f;
  for(f = G->top_frame; f; f = f->prev) {
    zu_t i;
    for(i = 0; i < f->size; i++) {
      if(!(f->s[i] & ZZ_NPTR))
        f->v[i].p = zFindNewPointer(G, f->v[i].p);
} } }

static int zMoveGC(zgc_t *G) {
  zi_t j, k;
  // Find destination gen. to copy
  zgen_t *dst;
  zi_t bot = G->gc_target, top = G->move_top;
  if(top >= G->n_gens) {
    zu_t sz = 0;
    for(k = bot; k < top; k++) sz += G->gens[k]->n_reachables;
    sz *= 2;
    if(sz < G->major_heap_min_size) sz = G->major_heap_min_size;
    if((dst = zNewGen(sz)) == NULL) return -1;
    if(top >= G->sz_gens) {
      G->gens = realloc(G->gens, sizeof(zgen_t**) * (G->sz_gens << 1));
      G->sz_gens <<= 1;
    }
    G->gens[top] = dst;
    G->n_gens++;
  } else dst = G->gens[top];
  // Reallocate (copy)
  for(j = top - 1; j >= (zi_t) bot; j--) {
    if(zReallocGenGC(G, dst, G->gens[j]) < 0) return -1;
  }
  // Change all reallocated pointers
  const zu_t jt = G->has_cyclic_ref ? G->n_gens : top + 1;
  for(j = 0; j < bot; j++) zGenUpdatePointers(G, G->gens[j]);
  for(j = top; j < jt; j++) zGenUpdatePointers(G, G->gens[j]);
  zUpdateRootPointers(G);
  // Clean up generations
  for(k = 0; k < bot; k++) zGenCleanMarks(G->gens[k]);
  for(; k < top; k++) zGenCleanAll(G->gens[k]);
  for(; k < G->n_gens; k++) zGenCleanMarks(G->gens[k]);
  return 0;
}

static int zReduceEmptyGC(zgc_t *G) {
  zu_t k;
  zu_t total = 0, allocated = 0;
  for(k = 1; k < G->n_gens; k++) {
    total += G->gens[k]->size;
    allocated += G->gens[k]->size - G->gens[k]->left;
  }
  for(k = G->n_gens - 1; k >= 1 && total > allocated * 4; k--) {
    if(G->gens[k]->left == G->gens[k]->size) {
      total -= G->gens[k]->size;
      zDelGen(G->gens[k]);
      G->gens[k] = NULL;
    }
  }
  zu_t d = 0;
  for(k = 1; k < G->n_gens; k++) {
    if(G->gens[k] == NULL) d++;
    else G->gens[k - d] = G->gens[k];
  }
  G->n_gens -= d;
  return 0;
}

int zRunGC(zgc_t *G) {
  // Check GC is need
  if(G->gens[0]->left >= G->gens[0]->size) return 1;
  // Set-up mark levels
  G->gc_target = 0;
  G->mark_top = G->has_cyclic_ref ?
    G->n_gens : zFindTopEmptyGenByAlloc(G);
  // Make a space in minor heap
  if(zMarkGC(G) < 0) return -1;
  G->move_top = zFindTopEmptyGenByReachable(G);
  if(zMoveGC(G) < 0 || zReduceEmptyGC(G) < 0) return -1;
  ++G->n_collection;
  return 0;
}

int zFullGC(zgc_t *G) {
  // Copy all memories into a single major gen.
  G->gc_target = 0;
  G->mark_top = G->move_top = G->n_gens;
  if(zMarkGC(G) < 0 || zMoveGC(G) < 0 || zReduceEmptyGC(G) < 0)
    return -1;
  ++G->n_collection;
  return 0;
}

// Root frames
void zGCPushFrame(zgc_t *G, zu_t sz) {
  G->top_frame = zNewFrame(sz, G->top_frame);
}
void zGCPopFrame(zgc_t *G) {
  if(G->top_frame != NULL && G->top_frame != G->bot_frame) {
    zframe_t *f = G->top_frame;
    G->top_frame = f->prev;
    free(f);
  }
}
zu_t zGCTopFrameSize(zgc_t *G) {
  return G->top_frame->size;
}
zu_t zGCBotFrameSize(zgc_t *G) {
  return G->bot_frame->size;
}
ztag_t zGCTopFrame(zgc_t *G, zu_t idx) {
  return G->top_frame->v[idx];
}
ztag_t zGCBotFrame(zgc_t *G, zu_t idx) {
  return G->bot_frame->v[idx];
}
void zGCSetTopFrame(zgc_t *G, zu_t idx, ztag_t v, zu_t is_nptr) {
  G->top_frame->v[idx] = v;
  G->top_frame->s[idx] = is_nptr ? ZZ_NPTR : 0;
}
void zGCSetBotFrame(zgc_t *G, zu_t idx, ztag_t v, zu_t is_nptr) {
  G->bot_frame->v[idx] = v;
  G->bot_frame->s[idx] = is_nptr ? ZZ_NPTR : 0;
}

int zAllowCyclicRefGC(zgc_t *G, int v) {
  if(v > 0) { // ENABLE cyclic reference
    G->has_cyclic_ref = 1;
    return 1;
  } else if(v == 0) { // DISABLE cyclic reference
    if(zFullGC(G) < 0) return -1;
    G->has_cyclic_ref = 0;
    return 0;
  } else { // DISABLE cyclic reference w.o. full gc
    // There may be a problem if there are cyclic ref.s betw. gen.s
    G->has_cyclic_ref = 0;
    return 0;
} }

// GC Information
zu_t zNGen(zgc_t *G) {
  return G->n_gens;
}
zu_t zGCReservedSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->size;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->size;
  return sum;
}
zu_t zGCLeftSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->left;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->left;
  return sum;
}
zu_t zGCAllocatedSlots(zgc_t *G, int idx) {
  return zGCReservedSlots(G, idx) - zGCLeftSlots(G, idx);
}

// For tests
void zPrintGCStatus(zgc_t *G, zu_t *dst) {
  zu_t arr[4];
  if(dst == NULL) dst = arr;
  dst[0] = zGCReservedSlots(G, -1); dst[1] = zGCLeftSlots(G, -1);
  dst[2] = zGCReservedSlots(G, 0); dst[3] = zGCLeftSlots(G, 0);
  printf("GC Stat (%p, %" PRIuPTR " gens) [alloc(%%) / left(%%) / total]\n",
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

// Helpers
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