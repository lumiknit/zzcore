/* zzcore.h 0.0.1
 * author: lumiknit */
#ifndef __L_ZZCORE_H__
#define __L_ZZCORE_H__

#include <stdint.h>
#include <limits.h>

// PTR size types
typedef uintptr_t zu_t;
typedef uint8_t zb_t;
typedef intptr_t zi_t;
typedef void* zp_t;
#if UINTPTR_MAX >= 0xffffffffffffffff
#define ZZ_SZPTR 8
  typedef double zf_t;
#elif UINTPTR_MAX >= 0xffffffff
#define ZZ_SZPTR 4
  typedef float zf_t;
#else
  #error Unsupported pointer size
#endif
#define zBytesToWords(n) (((n) + ZZ_SZPTR - 1) / ZZ_SZPTR)
#define zWordsToBytes(n) ((n) * ZZ_SZPTR)

typedef union ztag {
  // Pointer-size integer/float container
  zu_t u; zi_t i; zf_t f; zp_t p;
  struct ztup *t; struct zstr *s;
  zb_t b[0];
} ztag_t;

typedef struct zgc zgc_t;

// -- GC APIs
zgc_t* zNewGC(zu_t /* root size */, zu_t /* minor heap size */);
void zDelGC(zgc_t*);

// Allocation
zu_t* zAlloc(zgc_t*, zu_t /* # of non-pointer */, zu_t /* # of pointer */);

// RunGC: Make an empty space in minor heap
int zRunGC(zgc_t*);
// FullGC: Arrange minor and all major heap
int zFullGC(zgc_t*);

// GC root frames
void zGCPushFrame(zgc_t*, int /* size */);
void zGCPopFrame(zgc_t*);
int zGCTopFrameSize(zgc_t*);
int zGCBotFrameSize(zgc_t*);
ztag_t zGCTopFrame(zgc_t*, int /* idx */);
ztag_t zGCBotFrame(zgc_t*, int /* idx */);
void zGCSetTopFrame(zgc_t*, int /* idx */, ztag_t /*v*/, int /*is_not_ptr*/);
void zGCSetBotFrame(zgc_t*, int /* idx */, ztag_t /*v*/, int /*is_not_ptr*/);

// Option setter
void zSetMajorMinSizeGC(zgc_t*, zu_t /* min major heap size */);
int zAllowCyclicRefGC(zgc_t*, int);

// GC Information
zu_t zGCNGen(zgc_t*); // return # of generations
// z__Slots: return # of slots(pointers) in the idx-th generation
// [0] is minor, [n] is n-th major heap
zu_t zGCReservedSlots(zgc_t*, int /* idx of gen, -1 for whole slots */);
zu_t zGCLeftSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);
zu_t zGCAllocatedSlots(zgc_t*, int /* idx of gen, -1 for whold slots */);

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