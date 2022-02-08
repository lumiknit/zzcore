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
#define colorFree 0x00
#define colorWhite 0x10
#define colorBlack 0x11
#define colorGray 0x01

static ZZTup* allocGen(ZZGen *J, const size_t slots) {
  const size_t n = slots + 1; // tag(1) + slots
  if(n >= 1024 || J->left < n) return NULL;
  J->left -= n;
  ZZTup* const ptr = (ZZTup*) (J->cells + J->left);
  const ZZMark m_left = markConstr(1, colorFree, 0);
  const ZZMark m_2nd = markConstr(1, colorFree, slots >> 5);
  const ZZMark m_1st = markConstr(1, colorBlack, slots & 0x1f);
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
  if(markIsPtr(m_1st) || markColor(m_1st) == colorFree) return 0;
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
  size_t sz_gens, n_gens;
  ZZGen **gens;
  ZZTup *root;
};

static int pushNewGCGen(ZZGC *G) {
  if(G->sz_gens >= G->n_gens) {
    const size_t nsz= G->sz_gens * 2;
    ZZGen** new_arr = (ZZGen**) realloc(G->gens, nsz);
    if(new_arr == NULL) return -1;
    G->gens = new_arr;
    G->sz_gens = nsz;
  }
  const size_t prev_size =
    G->n_gens > 1 ? G->gens[G->n_gens - 1]->size : ZZ_MAJOR_HEAP_MIN_SIZE;
  G->gens[G->n_gens++] = newGen(prev_size * 2);
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

ZZGC* ZZ_newGC(size_t root_size) {
  ZZGC *G = (ZZGC*) malloc(sizeof(ZZGC));
  G->sz_gens = 4;
  G->n_gens = 1;
  G->gens = (ZZGen**) malloc(sizeof(ZZGen*) * G->sz_gens);
  G->gens[0] = newGen(ZZ_MINOR_HEAP_SIZE);
  G->root = (ZZTup*) malloc(sizeof(ZZTup) + sizeof(ZZCell) * root_size);
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

ZZTup* ZZ_alloc(ZZGC *G, size_t n_slots) {
  if(n_slots + 1 >= G->gens[0]->size) return NULL;
  ZZTup *p;
  while(1) {
    p = allocGen(G->gens[0], n_slots);
    if(p != NULL) return p;
    if(!ZZ_minorGC(G)) return NULL;
  }
}

int ZZ_minorGC(ZZGC *G) {
  printf("[ERR] MINOR GC RUNNING!");
  exit(1);
}

int ZZ_majorGC(ZZGC *G) {
  printf("[ERR] MAJOR GC RUNNING!");
  exit(2);
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

ZZStr* ZZ_newStr(size_t len) {
  ZZStr *S = (ZZStr*) malloc(sizeof(ZZStr) + len);
  S->len = len;
  S->c[0] = '\0';
  return S;
}

void ZZ_delStr(ZZStr *S) {
  free(S);
}
