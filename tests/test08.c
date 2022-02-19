#include "test.h"
const char *TEST_NAME = "08. Large List";

#include <time.h>

void test() {
  clock_t cb, ce;
  zu_t k;
  cb = clock();
  const zu_t N = 0x1000000;
  for(k = 0; k < N; k++) {
  }
  ce = clock();
  printf("[INFO] %lf s elapsed for %zd empty loop\n",
    (double) (ce - cb) / (double) CLOCKS_PER_SEC,
    N);

  zgc_t *G = zNewGC(2, 0);
  assert(G != NULL);
  // zSetMajorMinSizeGC(G, 48);

  // Allocatd large list
  const zu_t LIST_N = 0x1000000;
  cb = clock();
  for(k = 0; k < LIST_N; k++) {
    zp_t *l = (zp_t*) zAlloc(G, 1, 1);
    //l[0] = (zp_t) k;
    l[1] = zGCTopFrame(G, 0).p;
    zGCSetTopFrame(G, 0, (ztag_t) {.p = l}, 0);
    /*if((k & 0xfffff) == 0) {
      printf("- %zu:\n", k);
      zPrintGCStatus(G, NULL);
    }*/
  }
  ce = clock();
  printf("[INFO] %lf s elapsed for %zd objects\n",
    (double) (ce - cb) / (double) CLOCKS_PER_SEC,
    LIST_N);

  zDelGC(G);

  void **p = NULL, **t;
  cb = clock();
  for(k = 0; k < N; k++) {
    t = (void**) malloc(ZZ_SZPTR * 2);
    *t = (void*) p;
    p = t;
  }
  while(p) {
    t = p;
    p = (void**) *p;
    free(t);
  }
  ce = clock();
  printf("[INFO] %lf s elapsed for %zd objects by malloc\n",
    (double) (ce - cb) / (double) CLOCKS_PER_SEC,
    N);
  (void) p;
}
