#ifndef __L_ZZCORE_H__
#define __L_ZZCORE_H__

#include <stdint.h>
#include <limits.h>

typedef union ZZTag ZZTag;
typedef struct ZZTup ZZTup;
typedef struct ZZGC ZZGC;
typedef struct ZZStr ZZStr;

union ZZTag {
  uintptr_t u;
  intptr_t i;
#if UINTPTR_MAX == 0xffffffffffffffff
  double f;
#elif UINTPTR_MAX == 0xffffffff
  float f;
#else
  #error Unsupported int size
#endif
  void *p;
  ZZTup *t;
  ZZStr *s;
  int32_t i32[1];
  int16_t i16[1];
};

struct ZZTup {
  ZZTag tag;
  ZZTup *slots[0];
};

struct ZZGC;

// GC APIs
ZZGC* ZZ_newGC(size_t /* root size */, size_t /* minor heap size */);
void ZZ_delGC(ZZGC*);
// Set Options
void ZZ_setMinMajorHeapSize(ZZGC*, size_t /* min major heap size */);
// Allocation
ZZTup* ZZ_alloc(ZZGC*, size_t /* # of slots */);
// run gc
int ZZ_runGC(ZZGC*);
// GC Helpers
ZZTup* ZZ_root(ZZGC*);
ZZTup* ZZ_frame(ZZGC*);
ZZTup* ZZ_pushFrame(ZZGC*, size_t /* # of slots in a frame */);
int ZZ_popFrame(ZZGC*);
// GC Information
size_t ZZ_nGen(ZZGC*);
size_t ZZ_reservedSlots(ZZGC*, int /* idx of gen, -1 for whole slots */);
size_t ZZ_leftSlots(ZZGC*, int /* idx of gen, -1 for whold slots */);
size_t ZZ_allocatedSlots(ZZGC*, int /* idx of gen, -1 for whold slots */);
// For tests
void ZZ_printGCStatus(ZZGC*, size_t *dst);

// Helpers

struct ZZStr {
  size_t len;
  char c[1];
};

ZZStr* ZZ_newStr(size_t);
void ZZ_delStr(ZZStr*);

#endif
