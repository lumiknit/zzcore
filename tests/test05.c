#include "test.h"
const char *TEST_NAME = "05. Allocation of Large Constants";

const char *lorem =
"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Proin sollicitudin "
"magna id mi tincidunt, a condimentum mi vestibulum. Vivamus auctor mi ac "
"tortor ornare, quis ultrices leo tristique. Quisque scelerisque, velit a "
"consequat aliquet, dolor arcu consequat quam, eu aliquam neque sem in ligula. "
"Duis ex ipsum, vulputate sed dolor molestie, imperdiet tempor ex. "
"Suspendisse in arcu at sem fringilla varius sit amet sed sem. Donec vulputate "
"justo ut sagittis tristique. Aliquam convallis vulputate justo ac sodales. "
"Fusce ut erat in eros interdum dictum quis molestie erat.";

const char *aliquam =
"Aliquam efficitur mi eget est iaculis suscipit. Aenean mauris ligula, maximus "
"quis arcu sit amet, mattis vulputate enim. In odio purus, consectetur quis "
"est euismod, euismod aliquam urna. Pellentesque vehicula, sapien lacinia "
"faucibus tincidunt, odio quam maximus nibh, quis pellentesque velit ligula "
"eget risus. Donec nibh dolor, placerat ac cursus non, varius eget ex. Ut eget "
"viverra lacus. Quisque volutpat efficitur odio, sit amet condimentum lorem. "
"Curabitur mollis felis id leo mattis vestibulum. Aenean in augue eget nisi "
"laoreet pharetra id non neque. Quisque sodales varius facilisis. Phasellus "
"pulvinar tristique lorem. Etiam maximus nulla ac justo viverra, vitae "
"efficitur neque bibendum.";

zgc_t *G;

int STR_SLOTS = 0;
char* allocStr(const char *s) {
  const zu_t l = strlen(s) + 1;
  const zu_t a = (l + ZZ_SZPTR - 1) / ZZ_SZPTR;
  char *p = (char*) zAlloc(G, a, STR_SLOTS);
  strcpy(p, s);
  p[l - 1] = '\0';
  return p;
}

void test() {
  // New GC
  G = zNewGC(10, 32);
  zSetMajorMinSizeGC(G, 32);
  assert(G != NULL);
  // New large one
  printf("[INFO] Alloc hello -> lorem -> aliquam -> lorem -> aliquam\n");
  char *s1 = allocStr("Hello, World!");
  zGCSetTopFrame(G, 0, (ztag_t) {.p = s1}, 0);
  zPrintGCStatus(G, NULL);
  char *s2 = allocStr(lorem);
  zGCSetTopFrame(G, 1, (ztag_t) {.p = s2}, 0);
  zPrintGCStatus(G, NULL);
  char *s3 = allocStr(aliquam);
  zGCSetTopFrame(G, 2, (ztag_t) {.p = s3}, 0);
  zPrintGCStatus(G, NULL);
  char *s4 = allocStr(lorem);
  zGCSetTopFrame(G, 3, (ztag_t) {.p = s4}, 0);
  zPrintGCStatus(G, NULL);
  char *s5 = allocStr(aliquam);
  zGCSetTopFrame(G, 4, (ztag_t) {.p = s5}, 0);
  zPrintGCStatus(G, NULL);
  printf("[INFO] Make first lorem & aliquam unreachable\n");
  zGCSetTopFrame(G, 2, (ztag_t) {.p = NULL}, 0);
  zGCSetTopFrame(G, 1, (ztag_t) {.p = NULL}, 0);
  printf("[INFO] Full GC\n");
  zFullGC(G);
  zPrintGCStatus(G, NULL);
  printf("[INFO] Alloc aliquam\n");
  char *s6 = allocStr(aliquam);
  zGCSetTopFrame(G, 1, (ztag_t) {.p = s6}, 0);
  zPrintGCStatus(G, NULL);
  printf("[INFO] Alloc 2 lorem with a slot\n");
  STR_SLOTS = 1;
  char *s7 = allocStr(lorem);
  zGCSetTopFrame(G, 5, (ztag_t) {.p = s7}, 0);
  zPrintGCStatus(G, NULL);
  char *s8 = allocStr(lorem);
  zGCSetTopFrame(G, 6, (ztag_t) {.p = s8}, 0);
  zPrintGCStatus(G, NULL);
  // Del GC
  zDelGC(G);
}
