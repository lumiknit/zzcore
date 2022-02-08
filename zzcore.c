#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzcore.h"

// Options
const size_t ZZ_MINOR_HEAP_SIZE = 1024;
const size_t ZZ_MAJOR_HEAP_MIN_SIZE = 16384;

typedef uint8_t ZZMark;
typedef uintptr_t ZZCell;

typedef struct ZZGen {
  size_t size; // number of pointers in data
  size_t left;
  void *p; // start position of marks+data, used only for free
  ZZMark *marks;
  ZZCell *cells;
  // Only for GC
  size_t n_reachables;
} ZZGen;

static ZZGen* newGen(size_t size) {
  ZZGen *J = (ZZGen*) malloc(sizeof(ZZGen));
  J->size = size;
  J->left = size;
  const size_t alloc_bytes = size * (sizeof(uint8_t) + sizeof(uintptr_t));
  J->p = malloc(alloc_bytes);
  memset(J->p, 0x00, alloc_bytes);
  J->marks = (uint8_t*) J->p;
  J->cells = (uintptr_t*) (size + (uint8_t*) J->p);
  return J;
}

static void delGen(ZZGen *J) {
  free(J->p);
  free(J);
}

#define p2u(P) ((uintptr_t)(P))
#define isBetween(V, F, T) (p2u(F) <= p2u(V) && p2u(V) < p2u(T))

#define markFirstPtr(J) (J->marks)
#define markLastPtr(J) (J->marks + J->size)

#define cellFirstPtr(J) (J->cells)
#define cellLastPtr(J) (J->cells + J->size)

#define isPtrInMarks(J, p) isBetween(markFirstPtr(J), p, markLastPtr(J))
#define isPtrInCells(J, p) isBetween(cellFirstPtr(J), p, cellLastPtr(J))

#define markGenOff(J, p) ((size_t) ((void*) p - (void*) markFirstPtr(J)))
#define markGenIdx(J, p) (markGenOff(J, p) / sizeof(ZZMark))

#define cellGenOff(J, p) ((size_t) ((void*) p - (void*) cellFirstPtr(J)))
#define cellGenIdx(J, p) (cellGenOff(J, p) / sizeof(ZZCell))

#define markIsPtr(m) (m & 0x01)
#define markColor(m) ((m & 0x06) >> 1)
#define markSize(m) ((m & 0xf8) >> 3)
#define markToIsPtr(v) (v & 0x01)
#define markToColor(v) ((v << 1) & 0x06)
#define markToSize(v) ((v << 3) & 0xf8)
#define markNIsPtr(m) (m & (0xff ^ 0x01))
#define markNColor(m) (m & (0xff ^ 0x06))
#define markNSize(m) (m & (0xff ^ 0xf8))
#define markUpdIsPtr(m, v) (markNIsPtr(m) | markToIsPtr(v))
#define markUpdColor(m, v) (markNColor(m) | markToColor(v))
#define markUpdSize(m, v) (markNSize(m) | markToSize(v))
#define markConstr(p, c, s) (markToIsPtr(p) | markToColor(c) | markToSize(s))
#define COLOR_FREE 0x00
#define COLOR_WHITE 0x10
#define COLOR_BLACK 0x11
#define COLOR_GRAY 0x01

static ZZTup* allocTup(ZZGen *J, const size_t slots) {
  const size_t n = slots + 1; // tag(1) + slots
  if(n >= 1024 || J->left < n) return NULL;
  J->left -= n;
  ZZTup* const ptr = (ZZTup*) (J->cells + J->left);
  const ZZMark m_left = markConstr(1, COLOR_FREE, 0);
  const ZZMark m_2nd = markConstr(1, COLOR_FREE, slots >> 5);
  const ZZMark m_1st = markConstr(1, COLOR_BLACK, slots & 0x1f);
  size_t idx = J->left;
  J->marks[idx] = m_1st;
  if(n >= 1) {
    J->marks[idx + 1] = m_2nd;
    for(size_t k = 1; k < slots; k++)
      J->marks[idx + 1 + k] = m_left;
  }
  return ptr;
}

static size_t ptrSizeAtIdx(ZZGen *J, const size_t idx) {
  const ZZMark m_1st = J->marks[idx];
  const ZZMark m_2nd = (idx < J->size) ? J->marks[idx + 1] : 0x00;
  if(markIsPtr(m_1st) || markColor(m_1st) == COLOR_FREE) return 0;
  size_t s = markSize(m_1st);
  if(markIsPtr(m_2nd)) {
    s |= markSize(m_2nd) << 5;
  }
  return s;
}

static size_t ptrSize(ZZGen *J, void *p) {
  if(!isPtrInCells(J, p)) return 0;
  const size_t idx = cellGenIdx(J, p);
  return ptrSizeAtIdx(J, idx);
}

struct ZZGC {
  // Options
  size_t minor_heap_size;
  size_t min_major_heap_size;
  // Data
  size_t sz_gens, n_gens;
  ZZGen **gens;
  ZZTup *root;
};

static int pushNewGCGen(ZZGC *G, size_t least_size) {
  if(G->sz_gens <= G->n_gens) {
    const size_t nsz= G->sz_gens * 2;
    ZZGen** new_arr = (ZZGen**) realloc(G->gens, nsz);
    if(new_arr == NULL) return -1;
    G->gens = new_arr;
    G->sz_gens = nsz;
  }
  const size_t prev_size =
    G->n_gens > 1 ? G->gens[G->n_gens - 1]->size : G->min_major_heap_size;
  const size_t new_size =
    least_size < prev_size * 2 ? prev_size * 2 : least_size;
  ZZGen *j = newGen(new_size);
  if(j == NULL) return -1;
  G->gens[G->n_gens++] = j;
  return 0;
}

static void delGCGen(ZZGC *G, size_t idx) {
  if(idx >= G->n_gens) return;
  delGen(G->gens[idx]);
  ZZGen **j = G->gens + idx;
  const size_t len = G->n_gens - idx - 1;
  memmove(j, j + 1, sizeof(ZZGen*) * len);
  G->gens[--G->n_gens] = NULL;
}

ZZGC* ZZ_newGC(size_t root_size, size_t minor_heap_size) {
  ZZGC *G = (ZZGC*) malloc(sizeof(ZZGC));
  G->minor_heap_size = ZZ_MINOR_HEAP_SIZE;
  if(minor_heap_size > 0) G->minor_heap_size = minor_heap_size;
  G->min_major_heap_size = ZZ_MAJOR_HEAP_MIN_SIZE;
  G->sz_gens = 4;
  G->n_gens = 1;
  G->gens = (ZZGen**) malloc(sizeof(ZZGen*) * G->sz_gens);
  G->gens[0] = newGen(G->minor_heap_size);
  G->root = (ZZTup*) malloc(sizeof(ZZTup) + sizeof(ZZCell) * root_size);
  G->root->tag.u = root_size;
  return G;
}

void ZZ_delGC(ZZGC *G) {
  for(size_t k = 0; k < G->n_gens; k++) {
    delGen(G->gens[k]);
  }
  free(G->gens);
  free(G->root);
  free(G);
}

void ZZ_setMinMajorHeapSize(ZZGC *G, size_t sz) {
  if(sz > 0) G->min_major_heap_size = sz;
}

static void findGenAndIdxOfPtr(ZZGC *G, size_t *gen, size_t *idx, ZZTup *ptr) {
  while(*gen < G->n_gens && !isPtrInCells(G->gens[*gen], ptr)) ++(*gen);
  assert(*gen < G->n_gens);
  *idx = cellGenIdx(G->gens[*gen], ptr);
}

const size_t MARK_STACK_SIZE = 8192;
static int newMarkStk(ZZTup ***stk) {
  ZZTup **s = malloc(sizeof(ZZTup*) * MARK_STACK_SIZE);
  if(stk == NULL) return -1;
  ((ZZTup***)s)[0] = *stk;
  ((ZZTup***)s)[1] = NULL;
  *stk = s;
  return 0;
}

static void delMarkStk(ZZTup ***stk) {
  if(*stk == NULL) return;
  ZZTup **s = *stk;
  ZZTup *next = (ZZTup*) s[1];
  *stk = (ZZTup**) s[0];
  (*stk)[1] = next;
  free(s);
}

static void delAllMarkStk(ZZTup **stk) {
  if(stk == NULL) return;
  while(stk[1]) stk = (ZZTup**) stk[1];
  while(stk) delMarkStk(&stk);
}

static void pushMarkStk(ZZTup ***stk, int *sp, ZZTup *val) {
  if(*stk == NULL || *sp >= MARK_STACK_SIZE) {
    if((*stk)[1]) *stk = (ZZTup**) (*stk)[1];
    else newMarkStk(stk);
    *sp = 1;
  }
  (*stk)[(*sp)++] = val;
}

static ZZTup* popMarkStk(ZZTup ***stk, int *sp) {
  if(*sp <= 1) {
    delMarkStk(stk);
    *sp = MARK_STACK_SIZE;
  }
  if(*stk == NULL) return NULL;
  return (*stk)[--(*sp)];
}

static inline void markTup(ZZGC *G, ZZTup ***stk, int *sp, ZZTup *t) {
  size_t g, idx;
  findGenAndIdxOfPtr(G, &g, &idx, t);
  ZZMark *m = G->gens[g]->marks + idx;
  markUpdColor(*m, COLOR_BLACK);
  if(ptrSizeAtIdx(G->gens[g], idx) > 0)
    pushMarkStk(stk, sp, t);
}

static inline void pushSlots(ZZGC *G, ZZTup ***stk, int *sp, ZZTup *t) {
  size_t g, idx;
  findGenAndIdxOfPtr(G, &g, &idx, t);
  ZZMark *m = G->gens[g]->marks + idx;
  size_t sz = ptrSizeAtIdx(G->gens[g], idx);
  for(int i = 0; i < sz; i++) {
    markTup(G, stk, sp, t->slots[i]);
  }
}

static int GCMarkPhase(ZZGC *G) {
  ZZTup **stk = NULL;
  int sp;
  // Copy roots into stk
  for(int i = 0; i < G->root->tag.u; i++) {
    markTup(G, &stk, &sp, G->root->slots[i]);
  }
  // Mark recursively
  while(stk != NULL) {
    ZZTup *t = popMarkStk(&stk, &sp);
    pushSlots(G, &stk, &sp, t);
  }
  return -1;
}

static int GCMovePhase(ZZGC *G, size_t gen) {
  return -1;
}

static int runGC(ZZGC *G, size_t gen) {
  if(GCMarkPhase(G) < 0) return -1;
  if(GCMovePhase(G, gen) < 0) return -1;
  return 0;
}

int ZZ_runGC(ZZGC *G) {
  return runGC(G, 0);
}

ZZTup* ZZ_alloc(ZZGC *G, size_t n_slots) {
  size_t gen = 0;
  if(n_slots + 1 >= G->gens[gen]->size) {
    ++gen;
    while(gen < G->n_gens && n_slots + 1 >= G->gens[gen]->size) ++gen;
    if(gen >= G->n_gens) {
      if(pushNewGCGen(G, n_slots * 2) < 0) return NULL;
    }
  }
  ZZTup *p = allocTup(G->gens[gen], n_slots);
  if(p != NULL) return p;
  if(runGC(G, gen) < 0) return NULL;
  return allocTup(G->gens[gen], n_slots);
}

ZZTup* ZZ_root(ZZGC *G) {
  return G->root;
}

ZZTup* ZZ_frame(ZZGC *G) {
  return G->root->slots[0];
}

ZZTup* ZZ_pushFrame(ZZGC *G, size_t frame_size) {
  ZZTup *frame = ZZ_alloc(G, frame_size);
  if(frame == NULL) return NULL;
  frame->slots[0] = G->root->slots[0];
  G->root->slots[0] = frame;
  return frame;
}

int ZZ_popFrame(ZZGC *G) {
  if(G->root->slots[0] == NULL) return -1;
  G->root->slots[0] = G->root->slots[0]->slots[0];
  return 0;
}

size_t ZZ_nGen(ZZGC *G) { return G->n_gens; }
size_t ZZ_reservedSlots(ZZGC *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->size;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->size;
  return sum;
}
size_t ZZ_leftSlots(ZZGC *G, int idx) {
  if(idx < -1 || idx >= (int)G->n_gens) return 0;
  if(idx >= 0) return G->gens[idx]->left;
  size_t sum = 0;
  for(idx = 0; idx < G->n_gens; idx++)
    sum += G->gens[idx]->left;
  return sum;
}
size_t ZZ_allocatedSlots(ZZGC *G, int idx) {
  return ZZ_reservedSlots(G, idx) - ZZ_leftSlots(G, idx);
}

void ZZ_printGCStatus(ZZGC *G, size_t *dst) {
  size_t arr[4];
  if(dst == NULL) {
    dst = arr;
  }
  dst[0] = ZZ_reservedSlots(G, -1);
  dst[1] = ZZ_leftSlots(G, -1);
  dst[2] = ZZ_reservedSlots(G, 0);
  dst[3] = ZZ_leftSlots(G, 0);
  printf("GC Stat (%p, %" PRIuPTR " gens) [alloc(%%) / left(%%) / total]\n",
    G, G->n_gens);
  size_t t = dst[0], l = dst[1];
  size_t a = t - l;
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


ZZStr* ZZ_newStr(size_t len) {
  ZZStr *S = (ZZStr*) malloc(sizeof(ZZStr) + len);
  S->len = len;
  S->c[0] = '\0';
  return S;
}

void ZZ_delStr(ZZStr *S) {
  free(S);
}
