

#include "tcommon.h"

#include "tsioutils.h"

#include <string.h>

//------------------------------------------------------------------------------
void swapAndCopy16Bits(const short *srcBuffer, short *dstBuffer,
                        TINT32 sampleCount) {
  // The samples of a sound file can start at any byte of the file's buffer, so
  // they are read and written as bytes.
  const UCHAR *srcData = (const UCHAR *)srcBuffer;
  UCHAR *dstData       = (UCHAR *)dstBuffer;
  for (TINT32 i = 0; i < sampleCount; ++i) {
    short sample;
    memcpy(&sample, srcData + i * sizeof(short), sizeof(short));
    sample = swapShort(sample);
    memcpy(dstData + i * sizeof(short), &sample, sizeof(short));
  }
}

//------------------------------------------------------------------------------
void swapAndCopy32Bits(const TINT32 *srcBuffer, TINT32 *dstBuffer,
                        TINT32 sampleCount) {
  const UCHAR *srcData = (const UCHAR *)srcBuffer;
  UCHAR *dstData       = (UCHAR *)dstBuffer;
  for (TINT32 i = 0; i < sampleCount; ++i) {
    TINT32 sample;
    memcpy(&sample, srcData + i * sizeof(TINT32), sizeof(TINT32));
    sample = swapTINT32(sample);
    memcpy(dstData + i * sizeof(TINT32), &sample, sizeof(TINT32));
  }
}

//------------------------------------------------------------------------------
void swapAndCopy24Bits(const void *srcBuffer, void *dstBuffer,
                              TINT32 sampleCount) {
  const UCHAR *srcData = (const UCHAR *)srcBuffer;
  UCHAR *dstData       = (UCHAR *)dstBuffer;
  for (TINT32 i = 0; i < sampleCount; ++i) {
    dstData[0] = srcData[2];
    dstData[1] = srcData[1];
    dstData[2] = srcData[0];
    srcData += 3;
    dstData += 3;
  }
}
