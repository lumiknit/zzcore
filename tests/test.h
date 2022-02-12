#ifndef __TEST_H__
#define __TEST_H__

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zzcore.h"

extern const char *TEST_NAME;

void test();

int main(int argc, char **argv) {
  printf("[INFO] -- TEST: %s\n", TEST_NAME);
  test();
  return 0;
}

#endif