/* Test 02
 * Allocate tuples until gc run
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zzcore.h"


int main() {
  // New GC
  ZZGC *G = ZZ_newGC(16, 128);
  assert(G != NULL);
  // New Tuple
  for(int i = 0; i < 14; i++) {
    assert(NULL != ZZ_alloc(G, 10));
    printf("[INFO] %d-th allocation \n", 1 + i);
    ZZ_printGCStatus(G, NULL);
  }
  // Del GC
  ZZ_delGC(G);
  return 0;
}
