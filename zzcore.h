#ifndef __L_ZZCORE_H__
#define __L_ZZCORE_H__

#include <stdint.h>
#include <limits.h>

// PTR size types
typedef uintptr_t zu_t;
typedef uint8_t zb_t;
typedef intptr_t zi_t;
typedef void* zp_t;
#if UINTPTR_MAX == 0xffffffffffffffff
#define ZC_SZPTR 8
  typedef double zf_t;
#elif UINTPTR_MAX == 0xffffffff
#define ZC_SZPTR 4
  typedef float zf_t;
#else
  #error Unsupported ptr size
#endif

typedef struct zgc zgc_t;

// GC APIs
zgc_t* zNewGC(zu_t /* root size */, zu_t /* minor heap size */);
void zDelGC(zgc_t*);

// Option setter
void zSetMajorMinSize(zgc_t*, zu_t /* min major heap size */);

// Allocation
zu_t* zAlloc(zgc_t*, zu_t /* # of non-pointer */, zu_t /* # of pointer */);

// Collection
int zRunGC(zgc_t*);
int zFullGC(zgc_t*);

// GC Helpers
zp_t zRoot(zgc_t*, zu_t, zp_t);

int zEnableCyclicRef(zgc_t*, int);

// GC Information
zu_t zNGen(zgc_t*);
zu_t zReservedSlots(zgc_t*, int /* idx of gen, -1 for whole slots */);
zu_t zLeftSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);
zu_t zAllocatedSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);

// For tests
void zPrintGCStatus(zgc_t*, zu_t *dst);

// -- Helpers --
typedef union ztag {
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
  struct ztup *t;
  int32_t i32[1];
  int16_t i16[1];
} ztag_t;

typedef struct ztup {
  ztag_t tag;
  struct ztup *slots[0];
} ztup_t;

#endif