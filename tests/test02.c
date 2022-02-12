#include "test.h"
const char *TEST_NAME = "02. run gc once";
void test() {
  // New GC
  zgc_t *G = zNewGC(16, 128);
  assert(G != NULL);
  // New Tuple
  for(int i = 0; i < 14; i++) {
    assert(NULL != zAlloc(G, 0, 10));
    printf("[INFO] %d-th allocation \n", 1 + i);
    zPrintGCStatus(G, NULL);
  }
  // Del GC
  zDelGC(G);
}
