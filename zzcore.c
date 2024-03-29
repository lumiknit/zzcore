/* zzcore.c 0.0.1
 * author: lumiknit */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzcore.h"

/* ** ZZGC Description (Rough)
 *  ZZGC is a generational mark-and-copy GC. It has multiple generations, which
 * are one minor gen and multiple major gen. Every new object should be
 * allocated in the minor gen. If minor gen is full, it collects garbages.
 *  When it collects garbages of n-th gen, it'll try to move all alive objects
 * in n-th gen into the next (older) gen. But it's impossible sometimes. (e.g.
 * (n+1)-th gen is not enough empty to contain n-th alive objects.) In this
 * case, ZZGC collects garbages in not only n-th but multiple gens at once.
 *  Each object have references (pointer) and non-ref values. ZZGC consider only
 * ref parts as object references in GC. (Even if a ptr of GC obj is in non-ref
 * part, it's not considered as reference.) Default `zAlloc` fn allocates an
 * object, where ref part follows non-ref one. (e.g. zAlloc(3, 5) gives size 8
 * array with the first 3 slots for non-pointer and the rest for pointers.)
 *  ZZGC only accepts number of pointers for an object size, not number of
 * bytes. So non-ref part must be a multiple of sizeof(ptr) including paddings
 * for correct collection. (Note, word = pointer-size int)
 *  If there is no cyclic reference between generations, elder objects must
 * contains correct pointers even if there was a lot of move in younger
 * generations. Thus, ZZGC does not traverse elder gens and enhances a
 * performance in this case. However, there exist various cases obtaining
 * cyclic references: cyclic list, mutual recursive closure, mutable object,
 * etc. If a program has a possiblity of cyclic refs, user explicitly enable an
 * option to handle this.
 *  ZZGC makes new generation when there is no generation empty enough to
 * keep all alive objects. And ZZGC remove empty major generations when
 * There are too many empty generations.
 * (If no more cyclic references will occur, the option can be disabled after
 * run full GC.)
 *  At this present, ZZGC is not incremental. However it may be easily changed
 * into incremental version. (?)
 */

// GC Options
// Default minor heap size in words (= 8B in 64-bit arch)
const static int ZZ_DEFAULT_MINOR_HEAP_SIZE = 1 << 18; // 256k words
// Default major heap size in words
const static int ZZ_DEFAULT_MAJOR_HEAP_SIZE = 1 << 18;  // 256k words
// Initial generation array size
const static int ZZ_N_GENS = 8;
// Minimal heap size in words. If user set heap size option less than this,
// ZZGC works with above default options
const static int ZZ_HEAP_MIN_SIZE = 16; // 16 words
// Initial stack size(# of objects) for marking 
const static int ZZ_MARK_STK_BOT_SIZE = 512; // 255 objects
// New heap size factor
// : If new gen created for M words, its size will be (M * factor) words
const static zu_t ZZ_NEW_HEAP_SIZE_FACTOR = 3; // 3
// Heap empty limit inv
// : If (heap total size) / limit > allocated, remove empty gens after copy.
const static zu_t ZZ_HEAP_EMPTY_LIMIT_INV = 5; // 20%

typedef struct zgen { // generation structure
  zu_t size; // # of words in data
  zu_t left; // # of free words in data
  zb_t *m; // marks
  zb_t *s; // stats
  zu_t *p; // value/pointer pools
  // Only for GC
  zu_t n_reachables; // # of words in alive objects
  // memory pool for marks and pointers, m ++ p
  zb_t *body;
} zgen_t;

typedef struct zframe { // root stack frame
  struct zframe *prev;
  int size; // # of objects
  zb_t *s; // is-non pointer array
  ztag_t *v; // value array
  zp_t p[1]; // memory pool
} zframe_t;

typedef struct zgc {
  // -- Options
  zu_t major_heap_min_size; // [1-] Major heap minimum size
  int has_cyclic_ref; // true when there are cyclic references
  // -- Generations
  int sz_gens, n_gens; // Gens array size & number of gens
  zgen_t **gens;
  // -- Roots
  zframe_t *bot_frame, *top_frame;
  // --- GC data
  // mark stack
  zi_t mark_sp, sz_mark_stk;
  zp_t *mark_stk;
  // GC Temp: used during collection
  int gc_target; // collection target generation
  int mark_top; // max marking generation + 1
  int move_top; // max move generation + 1
  // -- statistics
  zu_t n_collection;
} zgc_t;

// Mark constant
#define ZZ_COLOR 0xff // Color flag
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
  memset(b, 0x00, (sizeof(zb_t) * 2) * (sz + 1));
  if(X == NULL || b == NULL) goto L_fail;
  X->size = X->left = sz;
  X->body = (zp_t) b;
  X->n_reachables = 0;
  X->m = (zb_t*) b;
  X->s = (zb_t*) (X->m + (sz + 1));
  X->p = (zu_t*) (X->m + (sz + 1) * 2);
  // end of array mark
  X->m[X->size] = ZZ_COLOR;
  X->s[X->size] = ZZ_SEP;
  // canari
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
  // If free words are insufficient, it fails.
  if(X->left < np + p) return NULL;
  // Alloc
  X->left -= np + p;
  // Mark non-pointer parts
  memset(X->s + X->left, ZZ_NPTR, sizeof(zb_t) * np);
  // Mark object separator
  X->s[X->left] |= ZZ_SEP;
  return X->p + X->left;
}

static void zGenCleanMarks(zgen_t *X) {
  // Mark X in white
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->n_reachables = 0;
}

static void zGenCleanAll(zgen_t *X) {
  // Free all objects in X
  memset(X->m + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  memset(X->s + X->left, 0x00, sizeof(zb_t) * (X->size - X->left));
  X->left = X->size;
  X->n_reachables = 0;
}

static zi_t zGenPtrIdx(zgen_t *X, zp_t p) {
  // Check p is in X and return index of p if so
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

// Option setter
void zSetMajorMinSizeGC(zgc_t *G, zu_t msz) {
  if(msz >= ZZ_HEAP_MIN_SIZE) G->major_heap_min_size = msz;
}

// Allocation
zu_t* zAlloc(zgc_t *G, zu_t np, zu_t p) {
  const zu_t sz = np + p;
  zgen_t * const minor = G->gens[0];
  // Check very large chunk required
  if(sz >= minor->size) {
    int k;
    if(!G->has_cyclic_ref && p > 0) {
      // If cyclic is not allowed and there is ref part, run gc
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
    zgen_t *J = zNewGen(sz * ZZ_NEW_HEAP_SIZE_FACTOR);
    if(J == NULL) return NULL;
    for(k = G->n_gens; k >= 2; k--) {
      G->gens[k] = G->gens[k - 1];
    }
    G->gens[1] = J;
    G->n_gens++;
    return zGenAlloc(J, np, p);
  }
  // Try to allocate in minor heap
  if(minor->left < sz) zRunGC(G);
  minor->left -= sz;
  memset(minor->s + minor->left, ZZ_NPTR, sizeof(zb_t) * np);
  minor->s[minor->left] |= ZZ_SEP;
  return minor->p + minor->left;
}

// Collection

// Mark stack API
static void zMarkStkPush(zgc_t *G, int gen, zu_t idx) {
  // Push gen & idx
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
  // Pop values and set into gen & idx
  // return 0 if stack is empty
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
  // Empty stack
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
  // Propagation of marking in black
  zgen_t * const J = G->gens[gen];
  // Traverse all references from p
  zu_t xoff = idx;
  const zu_t kf = G->has_cyclic_ref ? 0 : gen;
  do {
    if(!(J->s[xoff] & ZZ_NPTR)) { // Ignore non-pointer slots
      // Find generation & index of ref
      const zp_t ref = (zp_t) J->p[xoff];
      zu_t k;
      for(k = kf; k < G->mark_top; k++) {
        zgen_t * const K = G->gens[k];
        const zi_t idy = zGenPtrIdx(K, ref);
        // Check ref is not visited
        if(idy >= 0 && (K->s[idy] & ZZ_SEP) &&
            (K->m[idy] == ZZ_WHITE)) {
          // Mark black
          // (For incremental GC, it should be ZZ_GRAY)
          K->m[idy] = ZZ_BLACK;
          zMarkStkPush(G, k, idy);
          break;
    } } }
  } while(!(J->s[++xoff] & ZZ_SEP));
  J->n_reachables += xoff - idx;
  return 0;
}

static int zMarkGC(zgc_t *G) {
  // Push all roots into stack
  zframe_t *f;
  int j, k;
  int gen;
  zu_t idx;
  // Traverse root frames
  for(f = G->top_frame; f; f = f->prev) {
    for(k = 0; k < f->size; k++) {
      if(!(f->s[k] & ZZ_NPTR)) {
        // Pick pointer and find generation & index
        for(j = 0; j < G->mark_top; j++) {
          zgen_t * const J = G->gens[j];
          const zi_t idy = zGenPtrIdx(J, f->v[k].p);
          if(idy >= 0 && (J->s[idy] & ZZ_SEP) &&
            (J->m[idy] == ZZ_WHITE)) {
            // If object is white, mark black and prop
            J->m[idy] = ZZ_BLACK;
            zMarkPropagate(G, j, idy);
            while(zMarkStkPop(G, &gen, &idx)) {
              zMarkPropagate(G, gen, idx);
  } } } } } }
  // Cleanup stack
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
  // Move alive objects in src into dst
  zu_t off = src->left;
  zu_t p = 0;
  const zu_t lim = src->size;
  // Traverse all objects
  while(off < lim) {
    if(src->m[off]) {
      // Find the longest block to be copied
      p = off;
      while(p < lim && (!(src->s[p] & ZZ_SEP) || src->m[p])) p++;
      const zu_t sz = p - off;
      // Alloc & copy in dst
      dst->left -= sz;
      memcpy(dst->s + dst->left, src->s + off, sizeof(zb_t) * sz);
      memcpy(dst->p + dst->left, src->p + off, sizeof(zu_t) * sz);
      // Put new address into original objects
      const zu_t *dp = dst->p + dst->left - off;
      for(; off < p; off++) {
        if(src->s[off] & ZZ_SEP) src->p[off] = (zu_t) (dp + off);
      }
    } else off++;
  }
  return 0;
}

static void zGenUpdatePointers(zgc_t *G, zgen_t *J) {
  // Update copied objects' pointer
  int k;
  zu_t off = J->left;
  const zu_t sz = J->size;
  zb_t * const s = J->s;
  zu_t * const p = J->p;
  const int tgt = G->gc_target, top = G->move_top;
  // Traverse all objects
  for(; off < sz; off++) {
    if(!(s[off] & ZZ_NPTR)) {
      // Find pointer's generation & idx in copied source
      // If they are found, the pointer is for copied object
      const zp_t ptr = (zp_t) p[off];
      for(k = tgt; k < top; k++) {
        zgen_t * const K = G->gens[k];
        const zi_t idx = zGenPtrIdx(K, ptr);
        if(idx >= 0) {
          // Update
          p[off] = (zu_t) K->p[idx];
          break;
} } } } }

static void zUpdateRootPointers(zgc_t *G) {
  // Exactly same as zGenUpdatePointers,
  // except it updates pointers in root frames
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
  // Find destination gen. to copy
  zgen_t *dst;
  int bot = G->gc_target, top = G->move_top;
  if(top >= G->n_gens) {
    // New generation is required
    zu_t sz = 0;
    for(k = bot; k < top; k++) sz += G->gens[k]->n_reachables;
    sz *= ZZ_NEW_HEAP_SIZE_FACTOR;
    if(sz < G->major_heap_min_size) sz = G->major_heap_min_size;
    if((dst = zNewGen(sz)) == NULL) return -1;
    if(top >= G->sz_gens) {
      // If gen array is small, extend it
      G->gens = realloc(G->gens, sizeof(zgen_t**) * (G->sz_gens << 1));
      G->sz_gens <<= 1;
    }
    // Put new gen into array
    G->gens[top] = dst;
    G->n_gens++;
  } else dst = G->gens[top];
  // Reallocate (copy)
  for(j = top - 1; j >= (zi_t) bot; j--) {
    if(zReallocGenGC(G, dst, G->gens[j]) < 0) return -1;
  }
  // Change all reallocated pointers
  const int jt = G->has_cyclic_ref ? G->n_gens : top + 1;
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
  int k;
  zu_t total = 0, allocated = 0;
  // Calculate current total/allocated words
  for(k = 1; k < G->n_gens; k++) {
    total += G->gens[k]->size;
    allocated += G->gens[k]->size - G->gens[k]->left;
  }
  // Remove gens while (allocated / total) < 1 / HEAP_ENTRY_LIMIT_INV
  for(k = G->n_gens - 1;
      k >= 1 && total > allocated * ZZ_HEAP_EMPTY_LIMIT_INV; k--) {
    if(G->gens[k]->left == G->gens[k]->size) {
      total -= G->gens[k]->size;
      zDelGen(G->gens[k]);
      G->gens[k] = NULL;
    }
  }
  // Erase deleted generations
  int d = 0;
  for(k = 1; k < G->n_gens; k++) {
    if(G->gens[k] == NULL) d++;
    else G->gens[k - d] = G->gens[k];
  }
  G->n_gens -= d;
  return 0;
}

int zRunGC(zgc_t *G) {
  // Make a space in minor heap
  // Check GC is need
  if(G->gens[0]->left >= G->gens[0]->size) return 1;
  // Set minor heap as the GC target
  G->gc_target = 0;
  // Find youngest generation which cannot be moved / point objects possible to
  // be moved
  G->mark_top = G->has_cyclic_ref ?
    G->n_gens : zFindTopEmptyGenByAlloc(G);
  // Marking phase
  if(zMarkGC(G) < 0) return -1;
  // Find youngest generation which will not copied
  G->move_top = zFindTopEmptyGenByReachable(G);
  // Copying phase & remove empty generations
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

// For tests
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