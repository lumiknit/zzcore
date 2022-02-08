/* Test 00
 * GC Initialize & finalize
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zzcore.h"

int main() {
  // New GC
  size_t sz[4];
  ZZGC *G = ZZ_newGC(16, 64);
  assert(G != NULL);
  ZZ_printGCStatus(G, sz);
  assert(sz[0] /* Reserved */ == 64);
  assert(sz[1] /* Left */ == 64);
  // New Tuple
  ZZTup *t1 = ZZ_alloc(G, 4);
  assert(t1 != NULL);
  t1->tag.i = 16;
  ZZ_printGCStatus(G, sz);
  // Check size
  assert(ZZ_leftSlots(G, -1) == 59);
  // Del GC
  ZZ_delGC(G);
  return 0;
}
