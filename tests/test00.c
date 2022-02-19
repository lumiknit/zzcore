#include "test.h"
const char *TEST_NAME = "00. zNewGC and zDelGC";
void test() {
  // New GC
  size_t sz[4];
  zgc_t *G = zNewGC(16, 64);
  assert(G != NULL);
  zPrintGCStatus(G, sz);
  assert(sz[0] /* Reserved */ == 64);
  assert(sz[1] /* Left */ == 64);
  // New Tuple
  ztup_t *t1 = (ztup_t*) zAlloc(G, 1, 4);
  assert(t1 != NULL);
  t1->tag.i = 16;
  zPrintGCStatus(G, sz);
  // Check size
  assert(zGCLeftSlots(G, -1) == 59);
  // Del GC
  zDelGC(G);
}
