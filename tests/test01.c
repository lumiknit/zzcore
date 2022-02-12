/* Test 01
 * Allocate a quite large object
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zzcore.h"


int main() {/*
  // New GC
  ZZGC *G = ZZ_newGC(16, 16);
  assert(G != NULL);
  // New Tuple
  ZZTup *t1 = ZZ_alloc(G, 1000);
  assert(t1 != NULL);
  t1->tag.i = 16;
  assert(t1->slots[0] == NULL);
  assert(t1->slots[999] == NULL);
  // Del GC
  ZZ_delGC(G);*/
  return 0;
}
