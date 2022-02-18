#include "test.h"
const char *TEST_NAME = "04. Run GC";
void test() {
  zp_t ss[3];
  // New GC
  zgc_t *G = zNewGC(3, 32);
  assert(G != NULL);
  // New Tuples
  zp_t *p1 = (zp_t*) zAlloc(G, 1, 4);
  zp_t *p2 = (zp_t*) zAlloc(G, 1, 4);
  zp_t *p3 = (zp_t*) zAlloc(G, 1, 4);
  zp_t *p4 = (zp_t*) zAlloc(G, 1, 4);
  zGCRoot(G, 0, p1);
  zGCRoot(G, 1, p4);
  p1[0] = (zp_t) 0x42;
  p2[0] = (zp_t) 0x53;
  p3[0] = (zp_t) 0x64;
  p4[0] = (zp_t) 0x7f;
  p1[1] = p3;
  zRunGC(G);
  zPrintGCStatus(G, NULL);
  zp_t *np1 = zGCRoot(G, 0, (zp_t) 1);
  zp_t *np4 = zGCRoot(G, 1, (zp_t) 1);
  zp_t *np3 = np1[1];
  assert(p1 != np1);
  assert(p3 != np3);
  assert(p4 != np4);
  assert(np1[0] == (zp_t) 0x42);
  assert(np3[0] == (zp_t) 0x64);
  assert(np4[0] == (zp_t) 0x7f);
  assert(np3[1] == NULL);
  assert(np4[1] == NULL);
  // Del GC
  zDelGC(G);
}
