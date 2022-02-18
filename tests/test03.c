#include "test.h"
const char *TEST_NAME = "03. stress";
void test() {
  zp_t ss[3];
  // New GC
  zgc_t *G = zNewGC(3, 32);
  zSetMajorMinSize(G, 128);
  assert(G != NULL);
  // New Tuples
  for(int i = 0; i < 10000; i++) {
    printf("[INFO] %d-th allocation \n", 1 + i);
    zp_t *p = (zp_t*) zAlloc(G, 0, 2);
    assert(NULL != p);
    p[0] = p[1] = NULL;
    if(rand() % 2 == 0) {
      int idx = rand() % 3;
      ss[idx] = p;
      zGCRoot(G, idx, p);
    }
    if(rand() % 2 == 0) {
      p[0] = ss[rand() % 3];
    }
    if(rand() % 2 == 0) {
      p[1] = ss[rand() % 3];
    }
    zPrintGCStatus(G, NULL);
  }
  // Del GC
  zDelGC(G);
}
