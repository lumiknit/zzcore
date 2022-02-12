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
  zu_t n_reachables, is_calc_reachables;
  // memory pool for marks and pointers, m ++ p
  zb_t *body;
} zgen_t;

typedef struct zgc {
  // Options
  zu_t major_heap_min_size; // [1-] Major heap minimum size
  zu_t has_cyclic_ref; // true when there are cyclic references
  // Generations
  zu_t sz_gens, n_gens; // Gens array size & number of gens
  zgen_t **gens;
  // Roots
  zu_t n_roots;
  zp_t *roots;
  // Mark stack
  zu_t mark_sp, sz_mark_stk;
  zp_t *mark_stk;
} zgc_t;

// GC default options
#define ZZ_DEFAULT_MINOR_HEAP_SIZE 1024
#define ZZ_DEFAULT_MAJOR_HEAP_SIZE 16384
#define ZZ_N_GENS 8

#define ZZ_MARK_STK_BOT_SIZE 256

// Mark constant
#define ZZ_COLOR 0x03 // Color flag
#define ZZ_NEG_COLOR (0xff ^ ZZ_COLOR)
#define ZZ_GRAY 0x02 // Unused
#define ZZ_BLACK 0x01
#define ZZ_WHITE 0x00

// Stat constant
#define ZZ_NPTR 0x01 // Not-pointer flag
#define ZZ_SEP 0x02 // Chunk separator flag

// 2bit-array operations
#define Z2B_GET(ARR, IDX) (0x03 & ((ARR)[(IDX) >> 2] ))

static zgen_t* zNewGen(zu_t sz) {
  zgen_t *X = (zgen_t*) malloc(sizeof(zgen_t));
  zu_t alloc_sz = (sizeof(zu_t) + sizeof(zb_t) * 2) * (sz + 1);
  zu_t *b = (zu_t*) malloc(alloc_sz);
  memset(b, 0x00, alloc_sz);
  if(X == NULL || b == NULL) goto L_fail;
  X->size = X->left = sz;
  X->body = (zp_t) b;
  X->n_reachables = 0, X->is_calc_reachables = 0;
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

static zu_t* zGenRealloc(
    zgen_t *X, zu_t sz, zgen_t *src, zu_t off) {
  if(X->left < sz) return NULL;
  X->left -= sz;
  memcpy(X->s + X->left, src->s + off, sizeof(zb_t) * sz);
  memcpy(X->p + X->left, src->p + off, sizeof(zu_t) * sz);
  return X->p + X->left;
}

static void zCleanMarkGen(zgen_t *X) {
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->n_reachables = 0, X->is_calc_reachables = 0;
}

static void zCleanAllGen(zgen_t *X) {
  memset(X->s + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->p + X->left, 0x00, sizeof(zu_t) * (X->size - X->left));
  X->left = X->size;
}

static zu_t zGenReachables(zgen_t *X) {
  if(X->is_calc_reachables) return X->n_reachables;
  zu_t acc = 0;
  zu_t off = X->left;
  for(; off < X->size;) {
    zu_t sz = 1;
    while(!(X->s[off + sz] & ZZ_SEP)) sz++;
    if(X->m[off]) acc += sz;
    off += sz;
  }
  X->n_reachables = acc;
  X->is_calc_reachables = 1;
  return acc;
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

// GC APIs
zgc_t* zNewGC(zu_t sz_roots, zu_t sz_minor) {
  zgc_t *G = (zgc_t*) malloc(sizeof(zgc_t));
  zgen_t **gens = (zgen_t**) malloc(sizeof(zgen_t*) * ZZ_N_GENS);
  zp_t *roots = (zp_t*) malloc(sizeof(zp_t) * sz_roots);
  zp_t *stk = (zp_t*) malloc(sizeof(zp_t) * ZZ_MARK_STK_BOT_SIZE);
  if(sz_minor <= 16) sz_minor = ZZ_DEFAULT_MINOR_HEAP_SIZE;
  zgen_t *minor = zNewGen(sz_minor);
  if(!G || !gens || !roots || !stk || !minor)
    goto L_fail;
  memset(gens, 0x00, sizeof(zgen_t*) * ZZ_N_GENS);
  memset(roots, 0x00, sizeof(zp_t) * sz_roots);
  gens[0] = minor;
  G->gens = gens;
  G->roots = roots;
  G->major_heap_min_size = ZZ_DEFAULT_MAJOR_HEAP_SIZE;
  G->sz_gens = ZZ_N_GENS, G->n_gens = 1;
  G->n_roots = sz_roots;
  G->mark_stk = stk;
  stk[0] = stk[ZZ_MARK_STK_BOT_SIZE - 1] = NULL;
  G->mark_sp = 1;
  G->sz_mark_stk = ZZ_MARK_STK_BOT_SIZE;
  return G;
L_fail:
  if(G) free(G);
  if(gens) free(gens);
  if(roots) free(roots);
  return NULL;
}

void zDelGC(zgc_t *G) {
  zu_t k;
  for(k = 0; k < G->n_gens; k++)
    zDelGen(G->gens[k]);
  free(G->gens);
  free(G->roots);
  free(G);
}

// Option setter
void zSetMajorMinSize(zgc_t *G, zu_t msz) {
  if(msz >= 16) G->major_heap_min_size = msz;
}

// Allocation
zu_t* zAlloc(zgc_t *G, zu_t np, zu_t p) {
  // Try to allocate in minor heap
  zu_t *ptr = zGenAlloc(G->gens[0], np, p);
  if(ptr != NULL) return ptr;
  zRunGC(G);
  ptr = zGenAlloc(G->gens[0], np, p);
  return ptr;
}

// Collection

// Mark stack API
static int zMarkStkPush(zgc_t *G, zp_t p) {
  if(G->mark_sp >= G->sz_mark_stk - 1) {
    if(G->mark_stk[G->sz_mark_stk - 1]) {
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1];
      G->mark_sp = 1;
    } else {
      zp_t *fr = (zp_t*) malloc(sizeof(zp_t) * (G->sz_mark_stk * 2));
      assert(fr != NULL);
      fr[0] = G->mark_stk;
      G->mark_stk[G->sz_mark_stk - 1] = fr;
      G->mark_stk = fr;
      G->mark_sp = 1;
      G->sz_mark_stk <<= 1;
  } }
  G->mark_stk[G->mark_sp++] = p;
  return 0;
}
static zp_t zMarkStkPop(zgc_t *G) {
  if(G->mark_sp <= 1) {
    if(G->mark_stk[0] == NULL) return NULL;
    G->mark_stk = G->mark_stk[0];
    G->sz_mark_stk >>= 1;
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
  G->mark_sp = 0;
}

static int zMarkPropagate(zgc_t *G, zp_t p) {
  // Propagation of marking in black
  if(p == NULL) return 1;
  // Find generation & index
  zu_t j;
  for(j = 0; j < G->n_gens; j++) {
    zgen_t *J = G->gens[j];
    zu_t idx = zGenPtrIdx(J, p);
    if(idx >= 0) {
      // p must be at the first of chunk
      assert(J->s[idx] & ZZ_SEP);
      // Mark p into black
      J->m[idx] |= ZZ_BLACK;
      // Traverse all references from p
      zu_t off = 0;
      do {
        if(!(J->s[idx + off] & ZZ_NPTR)) { // Ignore non-pointer slots
          // Find generation & index of ref
          zu_t k;
          for(k = 0; j < G->n_gens; k++) {
            zgen_t *K = G->gens[j];
            zu_t idx = zGenPtrIdx(K, (zp_t) J->p[idx + off]);
            // Check ref is not visited
            if(idx >= 0 && (K->s[idx] & ZZ_SEP) &&
               (K->m[idx] & ZZ_COLOR == ZZ_WHITE)) {
              // Mark black
              // (For incremental GC, it should be ZZ_GRAY)
              K->m[idx] |= ZZ_BLACK;
              zMarkStkPush(G, (zp_t) J->p[idx + off]);
              break;
        } } }
      } while(!(J->m[idx + (++off)] & ZZ_SEP));
      break;
  } }
  return 1;
}

static int zMarkGC(zgc_t *G) {
  // Push all roots into stack
  zu_t k;
  for(k = 0; k < G->n_roots; k++) {
    zMarkStkPush(G, G->roots[k]);
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

static zu_t zFindNGensToMoveGC(zgc_t *G, zu_t bot) {
  zu_t acc = zGenReachables(G->gens[bot]);
  zu_t k = bot + 1;
  while(k < G->n_gens) {
    if(acc < G->gens[k]->left) break;
    acc += zGenReachables(G->gens[k]);
    k++;
  }
  return k - bot;
}

static int zReallocGenGC(zgc_t *G, zgen_t *dst, zgen_t *src) {
  zu_t off = src->left;
  while(off < src->size) {
    assert(src->s[off] & ZZ_SEP);
    // Find next chunk
    zu_t sz = zGenIdxSz(src, off);
    // Allocate into dst
    zu_t *n = zGenRealloc(dst, sz, src, off);
    assert(n != NULL);
    // Save a new pointer in the old object
    src->p[off] = (zu_t) n;
    off += sz;
  }
}

static void zGenUpdatePointers(zgc_t *G, zgen_t *J, zu_t bot, zu_t n) {
  zu_t k;
  zu_t off = J->left;
  for(; off < J->size; off++) {
    if(!(J->s[off] & ZZ_NPTR)) {
      zp_t ptr = (zp_t) J->p[off];
      for(k = bot; k < bot + n; k++) {
        zi_t idx = zGenPtrIdx(G->gens[k], ptr);
        if(idx >= 0) {
          J->s[off] = G->gens[k]->p[idx];
          break;
} } } } }

static int zMoveGC(zgc_t *G, zu_t bot, zu_t n) {
  zu_t j, k;
  // Find destination gen. to copy
  zgen_t *dst;
  if(bot + n >= G->n_gens) {
    zu_t sz = 0;
    for(k = bot; k < bot + n; k++) {
      sz += zGenReachables(G->gens[k]);
    }
    dst = zNewGen(sz * 2);
    if(dst == NULL) return -1;
  } else dst = G->gens[bot + n];
  // Reallocate (copy)
  for(j = n + bot - 1; j >= bot; j--) {
    if(zReallocGenGC(G, dst, G->gens[j]) < 0) return -1;
  }
  // Change all reallocated pointers
  zu_t jf = 0, jt = bot + n;
  if(G->has_cyclic_ref) jt = G->n_gens;
  for(j = jf; j < bot; j++) zGenUpdatePointers(G, G->gens[j], bot, n);
  for(j = bot + n; j < jt; j++) zGenUpdatePointers(G, G->gens[j], bot, n);
  if(bot + n >= G->n_gens) zGenUpdatePointers(G, dst, bot, n);
  // Clean up gens
  for(k = 0; k < bot; k++) zCleanMarkGen(G->gens[k]);
  for(; k < bot + n; k++) zDelGen(G->gens[k]);
  for(; k < G->n_gens; k++) zCleanMarkGen(G->gens[k]);
  if(bot == 0) {
    zCleanAllGen(G->gens[k]);
    bot++; n--;
  }
  // Remove copied generations
  if(bot + n >= G->n_gens) { // When alive objs are copied into new gen
    if(bot >= G->sz_gens) {
      G->gens = realloc(G->gens, sizeof(zgen_t**) * (G->sz_gens << 1));
      G->sz_gens <<= 1;
    }
    G->gens[bot] = dst;
    G->n_gens = bot + 1;
  } else { // When alive objs are copied into existing gen
    for(k = bot; k + n < G->n_gens; k++) {
      G->gens[k] = G->gens[k + n];
    }
    G->n_gens -= n;
  }
  return -1;
}

int zRunGC(zgc_t *G) {
  // Make a space in minor heap
  if(zMarkGC(G) < 0) return -1;
  zu_t n = zFindNGensToMoveGC(G, 0);
  if(zMoveGC(G, 0, n) < 0) return -1;
  return 0;
}

int zFullGC(zgc_t *G) {
  // Copy all memories into a single major gen.
  if(zMarkGC(G) < 0) return -1;
  zu_t n = G->n_gens;
  if(zMoveGC(G, 0, n) < 0) return -1;
  return 0;
}

// GC Helpers
zp_t zRoot(zgc_t *G, zu_t idx, zp_t ptr) {
  zp_t *p = G->roots[idx];
  G->roots[idx] = ptr;
  return p;
}

int zEnableCyclicRef(zgc_t *G, int v) {
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
zu_t zReservedSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->size;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->size;
  return sum;
}
zu_t zLeftSlots(zgc_t *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->left;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->left;
  return sum;
}
zu_t zAllocatedSlots(zgc_t *G, int idx) {
  return zReservedSlots(G, idx) - zLeftSlots(G, idx);
}

// For tests
void zPrintGCStatus(zgc_t *G, zu_t *dst) {
  zu_t arr[4];
  if(dst == NULL) {
    dst = arr;
  }
  dst[0] = zReservedSlots(G, -1);
  dst[1] = zLeftSlots(G, -1);
  dst[2] = zReservedSlots(G, 0);
  dst[3] = zLeftSlots(G, 0);
  printf("GC Stat (%p, %" PRIuPTR " gens) [alloc(%%) / left(%%) / total]\n",
    G, G->n_gens);
  zu_t t = dst[0], l = dst[1];
  zu_t a = t - l;
  double ap = 100 * (double) a / t;
  double lp = 100 * (double) l / t;
  printf(
    "* Entire: %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "\n",
    a, ap, l, lp, t);
  t = dst[2], l = dst[3]; a = t - l;
  ap = 100 * (double) a / t;
  lp = 100 * (double) l / t;
  printf(
    "* Minor: %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "(%.2lf%%) / %" PRIuPTR "\n",
    a, ap, l, lp, t);
}