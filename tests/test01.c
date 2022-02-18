#include "test.h"
const char *TEST_NAME = "01. alloc large object w/o full gc";
void test() {
  // New GC
  zgc_t *G = zNewGC(3, 32);
  zAllowCyclicRef(G, 1);
  assert(G != NULL);
  // New Large Tuple
  ztup_t *t1 = (ztup_t*) zAlloc(G, 1, 1000);
  assert(t1 != NULL);
  t1->tag.i = 16;
  assert(t1->slots[0] == NULL);
  assert(t1->slots[999] == NULL);
  // New Large string
  char *c = (char*) zAlloc(G, 16384, 0);
  assert(c != NULL);
  zPrintGCStatus(G, NULL);
  // Del GC
  zDelGC(G);
}
