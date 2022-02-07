#include <stdio.h>
#include <stdlib.h>

#include "zzcore.h"

typedef struct ZZBlock {
  size_t sz;
  size_t left;
  uint8_t *marks;
  uintptr_t *data;
};

struct ZZGC {
  ZZBlock *young;
  ZZBlock **olds;
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
