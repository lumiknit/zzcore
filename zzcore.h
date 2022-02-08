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

ZZGC* ZZ_newGC(size_t);
void ZZ_delGC(ZZGC*);

ZZTup* ZZ_alloc(ZZGC*, size_t n_slots);
int ZZ_minorGC(ZZGC*);
int ZZ_majorGC(ZZGC*);

ZZTup* ZZ_root(ZZGC*);
ZZTup* ZZ_frame(ZZGC*);
ZZTup* ZZ_pushFrame(ZZGC*, size_t frame_size);
int ZZ_popFrame(ZZGC*);

struct ZZStr {
  size_t len;
  char c[1];
};

ZZStr* ZZ_newStr(size_t);
void ZZ_delStr(ZZStr*);

#endif
