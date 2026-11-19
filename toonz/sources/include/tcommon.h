#pragma once

#ifndef T_COMMON_INCLUDED
#define T_COMMON_INCLUDED

#ifdef _MSC_VER
#pragma warning(disable : 4786)
#pragma warning(disable : 4251)
#pragma warning(disable : 4146)
#pragma warning(disable : 4267)
#endif

#ifndef _DEBUG
#undef _STLP_DEBUG
#else
#define _STLP_DEBUG 1
#endif
#if defined(NDEBUG) && defined(VERIFY_TNZCORE)
#undef NDEBUG
#include <assert.h>
#define NDEBUG
#else
#include <assert.h>
#endif

#include <string>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <limits>
#include <utility>

#if 0 && defined(__GNUC__)
// typedef seems strong on GCC and breaks code with TException..
#define TString std::wstring
#else
typedef std::wstring TString;
#endif

// STL

/*
 * On  Irix stl_alloc.h refers to nanosleep and timespec
 */

#ifdef __sgi
/*
#if (_MIPS_SZLONG == 32)
struct timespec {TLong tv_sec; TLong tv_nsec;};
#else
struct timespec {int tv_sec; TLong tv_nsec;};
#endif

#ifdef __cplusplus
extern "C"
#endif
int nanosleep(struct timespec *, int);
*/

#endif

#include <list>
#include <vector>
#include <map>
#include <cmath>
// .. and so on

namespace TConsts {
const double epsilon = 1e-8;
}

// use macros instead of constexprs, because MSVC2013 does not support
// `constexpr`.
#define M_PI_3 (M_PI / 3)
#define M_PI_180 (M_PI_4 / 45)
#define M_180_PI (90 * M_2_PI)
#define M_2PI (2 * M_PI)

// typedef's

#include "tnztypes.h"

typedef signed char SCHAR;
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef short SHORT;
typedef unsigned int UINT;

#ifndef _WIN32
typedef unsigned char BYTE;
#endif

template <class T>
inline T tcrop(T x, T a, T b) {
  return std::min(std::max(x, a), b);
}
template <class T>
inline void notLessThan(T threshold, T &x) {
  if (x < threshold) x = threshold;
}
template <class T>
inline void notMoreThan(T threshold, T &x) {
  if (x > threshold) x = threshold;
}

/*! round(x) returns the nearest integer to x.
    Works well for x<0, but is slower than roundp()
    /sa roundp() */
inline int tround(double x) {
  return ((int)(((int)(-0.9) == 0 && x < 0.0) ? (x - 0.5) : (x + 0.5)));
  // return ((int)(((int)(-0.9F) == 0 && x<0.0F) ? (x - 0.5F) : (x + 0.5F)));
}

/*! roundp(x) is like round(x), but the result if x<0 is
    platform dependent
        /sa round() */
inline int troundp(double x) { return ((int)((x) + 0.5F)); }

/*! byteFromUshort(u) converts integer from [0..65535] to [0..255] */
inline UCHAR byteFromUshort(USHORT u) {
  return ((256U * 255U + 1U) * u + (1 << 23)) >> 24;
}

/*! from[0. .. 1.] to [0..255] */
inline UCHAR byteFromFloat(float f) {
  return (f >= 1.f) ? 0xFF : (f <= 0.f) ? 0 : (UCHAR)(std::floor(f * 256.f));
}

/*! ditheredByteFromUshort(u) is like byteFromUshort().
    It is used in dithering ...
 */

inline UCHAR ditheredByteFromUshort(USHORT u, int r) {
  return ((((u * (256U * 255U + 1U)) - ((u * (256U * 255U + 1U)) >> 24)) + r) >>
          24);
}

/*! ushortFromByte(u) converts integer from [0..255] to  [0..65535]*/
inline USHORT ushortFromByte(UCHAR u) { return u | u << 8; }

const int c_maxint = ((int)((~0U) >> 1));
const int c_minint = ~c_maxint;

const unsigned int c_maxuint = (unsigned int)(~0U);

#ifdef _WIN32
#define DV_EXPORT_API __declspec(dllexport)
#define DV_IMPORT_API __declspec(dllimport)
#define DV_EXPORT_VAR __declspec(dllexport)
#define DV_IMPORT_VAR __declspec(dllimport)
#else
#define DV_EXPORT_API
#define DV_IMPORT_API
#define DV_EXPORT_VAR
#define DV_IMPORT_VAR
#endif

#ifdef _MSC_VER
#define DV_ALIGNED(val) __declspec(align(val))
#else
#define DV_ALIGNED(val) __attribute__((aligned(val)))
#endif

inline short swapShort(short val) {
  const USHORT bits = static_cast<USHORT>(val);
  return static_cast<short>(static_cast<USHORT>((bits >> 8) | (bits << 8)));
}
inline TINT32 swapTINT32(TINT32 val) {
  const TUINT32 bits = static_cast<TUINT32>(val);
  return static_cast<TINT32>((bits >> 24) | ((bits >> 8) & 0x0000ff00u) |
                             ((bits << 8) & 0x00ff0000u) | (bits << 24));
}
inline USHORT swapUshort(USHORT val) { return val >> 8 | val << 8; }

inline std::ostream &operator<<(std::ostream &out, const std::string &s) {
  return out << s.c_str();
}

const std::string styleNameEasyInputWordsFileName = "stylename_easyinput.ini";

#endif  //__T_COMMON_INCLUDED
