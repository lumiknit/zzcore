#include "test.h"
const char *TEST_NAME = "07. Multiple root frames";

void test() {
  zgc_t *G = zNewGC(2, 0);
  assert(G != NULL);
  
  zp_t p1 = zAlloc(G, 4, 4);
  zp_t p2 = zAlloc(G, 4, 4);
  zp_t p3 = zAlloc(G, 4, 4);
  zp_t p4 = zAlloc(G, 4, 4);
  zp_t p5 = zAlloc(G, 4, 4);

  printf("[INFO] Before GC\n");
  zPrintGCStatus(G, NULL);
  
  zGCSetTopFrame(G, 0, (ztag_t) {.p = p1}, 0); // Pointer
  zGCSetTopFrame(G, 1, (ztag_t) {.p = p2}, 0); // Pointer
  zGCPushFrame(G, 2);
  zGCSetTopFrame(G, 1, (ztag_t) {.p = p3}, 0); // Pointer
  zGCPushFrame(G, 2);
  zGCSetTopFrame(G, 1, (ztag_t) {.p = p4}, 1); // Non-Pointer
  zGCSetBotFrame(G, 0, (ztag_t) {.p = p5}, 1); // Non-Pointer

  zRunGC(G);

  printf("[INFO] After GC\n");
  zPrintGCStatus(G, NULL);
  
  zDelGC(G);
}
