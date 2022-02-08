#include <stdio.h>
#include "zzcore.h"

int main() {
  ZZGC *G = ZZ_newGC(16);
  ZZTup *t1 = ZZ_alloc(G, 4);
  t1->tag.i = 16;
  t1->slots[0] = NULL;
  t1->slots[1] = NULL;
  t1->slots[2] = NULL;
  t1->slots[3] = NULL;
  ZZ_delGC(G);
  return 0;
}
