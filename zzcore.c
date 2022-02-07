#include <stdio.h>
#include <stdlib.h>

#include "zzcore.h"

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
  J->data = (uintptr_t*) (size + (uint8_t*) J->p);
  return J;
}

static void delGen(ZZGen *J) {
  free(J->p);
  free(J);
}

#define markFirstPtr(J) (J->marks)
#define markLastPtr(J) (J->marks + J->size)

#define cellFirstPtr(J) (J->cells)
#define cellLastPtr(J) (J->cells + J->size)

#define isPtrInMarks(J, p) (markFirstPtr(J) <= p && p < markLastPtr(J))
#define isPtrInCells(J, p) (cellFirstPtr(J) <= p && p < cellLastPtr(J))

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
  const ZZTup *ptr = (ZZTup*) (J->cells + J->left);
  const ZZMark m_left = markConstr(1, colorFree, 0);
  const ZZMark m_2nd = markConstr(1, colorFree, slots >> 5);
  const ZZMark m_1st = markConstr(1, colorBlack, slots & 0x1f);
  size_t idx = J->left;
  J->mark[idx] = m_1st;
  if(n >= 1) {
    J->mark[idx + 1] = m_2nd;
    for(size_t k = 1; k < slots; k++)
      J->mark[idx + 1 + k] = m_left;
  }
  return ptr;
}

static size_t ptrSizeAtIdx(ZZGen *J, const size_t idx) {
  const ZZMark m_1st = J->mark[idx];
  const ZZMark m_2nd = (idx < J->size) ? J->mark[idx + 1] : 0x00;
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
  ZZTup *bot, *top;
};

ZZGC* ZZ_newGC() {
  ZZGC *G = (ZZGC*) malloc(sizeof(ZZGC));
  G->bot = G->top = NULL;
  return G;
}

void ZZ_delGC(ZZGC *G) {
  free(G);
}

ZZTup* ZZ_alloc(ZZGC *G, size_t n_slots) {

}

void ZZ_minorGC(ZZGC *G) {

}

void ZZ_majorGC(ZZGC *G) {

}

ZZTup* ZZ_bot(ZZGC *G) {
  return G->bot;
}

ZZTup* ZZ_top(ZZGC *G) {
  return G->top;
}

void ZZ_pushFrame(ZZGC *G, size_t frame_size) {

}

void ZZ_popFrame(ZZGC *G) {

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
