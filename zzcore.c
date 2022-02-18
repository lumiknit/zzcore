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
const static int ZZ_DEFAULT_MINOR_HEAP_SIZE = 1024;
const static int ZZ_DEFAULT_MAJOR_HEAP_SIZE = 16384;
const static int ZZ_N_GENS = 8;

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
  zCleanMarkGen(X);
  memset(X->s + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->p + X->left, 0x00, sizeof(zu_t) * (X->size - X->left));
  X->left = X->size;
}

static zu_t zGenAllocated(zgen_t *X) {
  return X->size - X->left;
}

static zu_t zGenReachables(zgen_t *X) {
  if(X->is_calc_reachables) return X->n_reachables;
  zu_t acc = 0;
  zu_t off = X->left;
  while(off < X->size) {
    zu_t sz = 1;
    while(!(X->s[off + sz] & ZZ_SEP)) sz++;
    if((X->m[off] & ZZ_COLOR) != ZZ_WHITE) acc += sz;
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
  G->n_collection = 0;
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
  // Check very large chunk required
  if(np + p >= G->gens[0]->size) {
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
  zu_t *ptr = zGenAlloc(G->gens[0], np, p);
  if(ptr != NULL) return ptr;
  if(zRunGC(G) < 0) return NULL;
  return zGenAlloc(G->gens[0], np, p);
}

// Collection

// Mark stack API
static int sp = 0;
static int zMarkStkPush(zgc_t *G, zp_t p) {
  if(G->mark_sp >= G->sz_mark_stk - 1) {
    if(G->mark_stk[G->sz_mark_stk - 1]) {
      G->mark_stk = G->mark_stk[G->sz_mark_stk - 1];
    } else {
      zp_t *fr = (zp_t*) malloc(sizeof(zp_t) * (G->sz_mark_stk << 1));
      assert(fr != NULL);
      fr[0] = G->mark_stk;
      fr[(G->sz_mark_stk << 1) - 1] = NULL;
      G->mark_stk[G->sz_mark_stk - 1] = fr;
      G->mark_stk = fr;
    }
    G->mark_sp = 1;
    G->sz_mark_stk <<= 1;
  }
  G->mark_stk[G->mark_sp++] = p;
  sp++;
  return 0;
}
static zp_t zMarkStkPop(zgc_t *G) {
  if(G->mark_sp <= 1) {
    if(G->mark_stk[0] == NULL) return NULL;
    G->mark_stk = G->mark_stk[0];
    G->sz_mark_stk >>= 1;
    G->mark_sp = G->sz_mark_stk - 1;
  }
  sp--;
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
  for(j = 0; j < G->n_gens; j++) {
    zgen_t *J = G->gens[j];
    zi_t idx = zGenPtrIdx(J, p);
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
          for(k = 0; k < G->n_gens; k++) {
            zgen_t *K = G->gens[k];
            zi_t idy = zGenPtrIdx(K, (zp_t) J->p[idx + off]);
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

static zu_t zFindTopEmptyGen(zgc_t *G, zu_t (*sizeCalc)(zgen_t*)) {
  zu_t k = G->gc_target;
  zu_t acc = sizeCalc(G->gens[k++]);
  for(; k < G->n_gens && acc < G->gens[k]->left; k++)
    acc += sizeCalc(G->gens[k]);
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
} }

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
  zu_t i;
  for(i = 0; i < G->n_roots; i++) {
    zp_t ptr = G->roots[i];
    G->roots[i] = zFindNewPointer(G, G->roots[i]);
} }

static int zMoveGC(zgc_t *G) {
  zi_t j, k;
  // Find destination gen. to copy
  zgen_t *dst;
  zi_t bot = G->gc_target, top = G->move_top;
  zi_t gap = top - bot;
  if(top >= G->n_gens) {
    zu_t sz = 0;
    for(k = bot; k < top; k++) {
      sz += zGenReachables(G->gens[k]);
    }
    sz *= 2;
    if(sz < G->major_heap_min_size) sz = G->major_heap_min_size;
    dst = zNewGen(sz);
    if(dst == NULL) return -1;
  } else dst = G->gens[top];
  // Reallocate (copy)
  for(j = top - 1; j >= (zi_t) bot; j--) {
    if(zReallocGenGC(G, dst, G->gens[j]) < 0) return -1;
  }
  // Change all reallocated pointers
  zu_t jf = 0, jt = top;
  if(G->has_cyclic_ref) jt = G->n_gens;
  for(j = jf; j < bot; j++) zGenUpdatePointers(G, G->gens[j]);
  for(j = top; j < jt; j++) zGenUpdatePointers(G, G->gens[j]);
  if(top >= G->n_gens) zGenUpdatePointers(G, dst);
  zUpdateRootPointers(G);
  // Clean up gens
  for(k = 0; k < bot; k++) zCleanMarkGen(G->gens[k]);
  for(k = (k == 0) ? 1 : k; k < top; k++) zDelGen(G->gens[k]);
  for(; k < G->n_gens; k++) zCleanMarkGen(G->gens[k]);
  if(bot == 0) {
    zCleanAllGen(G->gens[0]);
    bot++; gap--;
  }
  // Remove copied generations
  if(top >= G->n_gens) { // When alive objs are copied into new gen
    if(bot >= G->sz_gens) {
      G->gens = realloc(G->gens, sizeof(zgen_t**) * (G->sz_gens << 1));
      G->sz_gens <<= 1;
    }
    G->gens[bot] = dst;
    G->n_gens = bot + 1;
  } else { // When alive objs are copied into existing gen
    for(k = bot; k + gap < G->n_gens; k++) {
      G->gens[k] = G->gens[k + gap];
    }
    G->n_gens -= gap;
  }
  return 0;
}

int zRunGC(zgc_t *G) {
  // Check GC is need
  if(G->gens[0]->left >= G->gens[0]->size) return 1;
  // Set-up mark levels
  G->gc_target = 0;
  G->mark_top = zFindTopEmptyGen(G, zGenAllocated);
  // Make a space in minor heap
  if(zMarkGC(G) < 0) return -1;
  G->move_top = zFindTopEmptyGen(G, zGenReachables);
  if(zMoveGC(G) < 0) return -1;
  ++G->n_collection;
  return 0;
}

int zFullGC(zgc_t *G) {
  // Copy all memories into a single major gen.
  G->gc_target = 0;
  G->mark_top = G->move_top = G->n_gens;
  if(zMarkGC(G) < 0 || zMoveGC(G) < 0) return -1;
  ++G->n_collection;
  return 0;
}

// GC Helpers
zp_t zGCRoot(zgc_t *G, zu_t idx, zp_t ptr) {
  if((zp_t) 1 == ptr) return G->roots[idx];
  zp_t *p = G->roots[idx];
  G->roots[idx] = ptr;
  return p;
}

int zAllowCyclicRef(zgc_t *G, int v) {
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
  zu_t k;
  for(k = 0; k < G->n_gens; k++) {
    t = zReservedSlots(G, k), l = zLeftSlots(G, k);
    a = t - l;
    ap = 100 * (double) a / t;
    lp = 100 * (double) l / t;
    printf(
      "* [%" PRIuPTR "]: %" PRIuPTR "(%.2lf%%) / %"
      PRIuPTR "(%.2lf%%) / %" PRIuPTR "\n",
      k, a, ap, l, lp, t);
  }
}