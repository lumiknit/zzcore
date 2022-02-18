#include "test.h"
const char *TEST_NAME = "06. Collect cyclic reference";

void run(const char *label, zgc_t *G) {
  /* Pseudo code:
   * let rec x = Tup2 (x, None)  # Create mutable tuple
   * gc.collect ()               # Move x into 1st major area
   * x[1] := Tup2 (x, None)      # Make x -> x[1] -> x cyclic reference
   * # At this point, x[1] is in 0th minor and x is in 1st major.
   * # There is mutual ref between 0th and 1st
   * gc.collect ()               # Move x[1] into 1st major.
   * # If cyclic reference is allowed, x must points new pointer of x[1] in 1st
   * # Otherwise, x may points old pointer of x[1] in 0th,
   * # and x[1] may be collected as old obj is assumed not to ref new obj
   * print_pointer x[1] */
  zp_t *x = (zp_t*) zAlloc(G, 0, 2), *nx;
  x[0] = x;
  x[1] = NULL;
  zGCRoot(G, 0, (zp_t) x);
  zRunGC(G);
  nx = zGCRoot(G, 0, (zp_t) 1);
  zp_t *y = (zp_t*) zAlloc(G, 0, 2);
  y[0] = x;
  y[1] = NULL;
  nx[1] = y;
  printf("[%s] Before GC: x = %p, x[1] = %p\n", label, nx, nx[1]);
  zRunGC(G);
  nx = zGCRoot(G, 0, (zp_t) 1);
  printf("[%s] After GC: x = %p, x[1] = %p\n", label, nx, nx[1]);
  zPrintGCStatus(G, NULL);
}

void test() {
  { /* --  CYCLIC REFERENCE NOT ALLOWED -- */
    zgc_t *G = zNewGC(10, 32);
    assert(G != NULL);
    zAllowCyclicRef(G, 0);
    run("Cyclic Ref Not Allowed", G);
    zDelGC(G);
  }
  { /* --  CYCLIC REFERENCE ALLOWED -- */
    zgc_t *G = zNewGC(10, 32);
    assert(G != NULL);
    zAllowCyclicRef(G, 1);
    run("Cyclic Ref Allowed", G);
    zDelGC(G);
  }
}
