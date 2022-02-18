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

typedef union ztag {
  zu_t u; zi_t i; zf_t f; zp_t p;
  struct ztup *t;
  int32_t i32[0];
  int16_t i16[0];
} ztag_t;

typedef struct zgc zgc_t;

// GC APIs
zgc_t* zNewGC(zu_t /* root size */, zu_t /* minor heap size */);
void zDelGC(zgc_t*);

// Allocation
zu_t* zAlloc(zgc_t*, zu_t /* # of non-pointer */, zu_t /* # of pointer */);

// Collection
int zRunGC(zgc_t*);
int zFullGC(zgc_t*);

// GC root frames
void zGCPushFrame(zgc_t*, zu_t /* size */);
void zGCPopFrame(zgc_t*);
zu_t zGCTopFrameSize(zgc_t*);
zu_t zGCBotFrameSize(zgc_t*);
ztag_t zGCTopFrame(zgc_t*, zu_t /* idx */);
ztag_t zGCBotFrame(zgc_t*, zu_t /* idx */);
void zGCSetTopFrame(zgc_t*, zu_t /* idx */, ztag_t /*v*/, zu_t /*is_not_ptr*/);
void zGCSetBotFrame(zgc_t*, zu_t /* idx */, ztag_t /*v*/, zu_t /*is_not_ptr*/);

// Option setter
void zSetMajorMinSize(zgc_t*, zu_t /* min major heap size */);
int zAllowCyclicRef(zgc_t*, int);

// GC Information
zu_t zNGen(zgc_t*);
zu_t zReservedSlots(zgc_t*, int /* idx of gen, -1 for whole slots */);
zu_t zLeftSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);
zu_t zAllocatedSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);

// For tests
void zPrintGCStatus(zgc_t*, zu_t *dst);

// -- Helpers --

typedef struct ztup {
  ztag_t tag;
  struct ztup *slots[0];
} ztup_t;

ztup_t *zAllocTup(zgc_t*, zu_t /* tag */, zu_t /* dim */);

typedef struct zstr {
  zu_t len;
  char c[1];
} zstr_t;

zstr_t *zAllocStr(zgc_t*, zu_t /* len */);

#endif