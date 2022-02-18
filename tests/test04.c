#include "test.h"
const char *TEST_NAME = "04. Run GC";
void test() {
  zp_t ss[3];
  // New GC
  zgc_t *G = zNewGC(3, 32);
  assert(G != NULL);
  // New Tuples
  zp_t *p1 = (zp_t*) zAlloc(G, 0, 4);
  zp_t *p2 = (zp_t*) zAlloc(G, 0, 4);
  zp_t *p3 = (zp_t*) zAlloc(G, 0, 4);
  zp_t *p4 = (zp_t*) zAlloc(G, 0, 4);
  zRoot(G, 0, p1);
  zRoot(G, 1, p4);
  p1[0] = p3;
  zRunGC(G);
  zPrintGCStatus(G, NULL);
  // Del GC
  zDelGC(G);
}
