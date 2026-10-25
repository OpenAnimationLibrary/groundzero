#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

#include "zlib.h"

#include "psdutils.h"

#include <algorithm>
#include <new>

void readrow(FILE *psd, TPSDChannelInfo *chan,
             psdPixel row,             // row index
             unsigned char *inbuffer,  // dest buffer for the uncompressed row
             unsigned char *
                 tmpbuffer)  // temp rlebuffer for compressed data, 2xrb in size
{
  psdPixel n = 0, rlebytes;
  psdByte pos;

  int seekres = 0;

  // A row outside the channel, or a channel without data (a damaged file),
  // is read as a row of zeros.
  if (row < 0 || row >= chan->rows) {
    memset(inbuffer, 0, chan->rowbytes);
    return;
  }

  switch (chan->comptype) {
  case RAWDATA: /* uncompressed */
    pos     = chan->filepos + chan->rowbytes * row;
    seekres = fseek(psd, pos, SEEK_SET);
    if (seekres != -1) n = (psdPixel)fread(inbuffer, 1, chan->rowbytes, psd);
    break;
  case RLECOMP:
    if (!chan->rowpos) break;
    pos     = chan->rowpos[row];
    seekres = fseek(psd, pos, SEEK_SET);
    if (seekres != -1) {
      // tmpbuffer holds 2 * rowbytes (see readImageData())
      psdPixel rlesize =
          std::min(chan->rowpos[row + 1] - pos, (psdPixel)(2 * chan->rowbytes));
      rlebytes = (psdPixel)fread(tmpbuffer, 1, rlesize > 0 ? rlesize : 0, psd);
      n        = unpackrow(inbuffer, tmpbuffer, chan->rowbytes, rlebytes);
    }
    break;
  case ZIPWITHPREDICTION:
  case ZIPWITHOUTPREDICTION:
    if (!chan->unzipdata) break;
    memcpy(inbuffer, chan->unzipdata + chan->rowbytes * row, chan->rowbytes);
    return;
  }
  // if we don't recognise the compression type, skip the row
  // FIXME: or would it be better to use the last valid type seen?

  //	if(seekres == -1)
  //	alwayswarn("# can't seek to " LL_L("%lld\n","%ld\n"), pos);

  if (n < chan->rowbytes) {
    // warn("row data short (wanted %d, got %d bytes)", chan->rowbytes, n);
    // zero out unwritten part of row
    memset(inbuffer + n, 0, chan->rowbytes - n);
  }
}

int unpackrow(unsigned char *out, unsigned char *in, psdPixel outlen,
              psdPixel inlen) {
  psdPixel i, len;
  int val;

  for (i = 0; inlen > 1 && i < outlen;) {
    len = *in++;
    --inlen;

    if (len == 128) /* ignore RLE flag value */
      ;
    else {
      if (len > 128) {
        len = 1 + 256 - len;

        val = *in++;
        --inlen;

        if ((i + len) <= outlen)
          memset(out, val, len);
        else {
          memset(out, val, outlen - i);  // fill to complete row
          len = 0;
        }
      } else {
        ++len;
        if ((i + len) <= outlen) {
          if (len > inlen) break;
          memcpy(out, in, len);
          in += len;
          inlen -= len;
        } else {
          // copy to complete row, but not past the end of the input
          memcpy(out, in, std::min(outlen - i, inlen));
          len = 0;
        }
      }
      out += len;
      i += len;
    }
  }
  if (i < outlen) {
    // WARNING: not enough RLE data for row;
  }
  return i;
}

void skipBlock(FILE *f) {
  psdByte n = read4Bytes(f);
  // a negative length (a damaged file) would seek backwards
  if (n > 0) {
    fseek(f, n, SEEK_CUR);
  } else {
  }
}

// Read 2byte unsigned binary value.
// BigEndian.
unsigned read2UBytes(FILE *f) {
  unsigned n = fgetc(f) << 8;
  return n |= fgetc(f);
}
// Read 2byte binary value
// BigEndian.
int read2Bytes(FILE *f) {
  unsigned n = fgetc(f) << 8;
  n |= fgetc(f);
  return n < 0x8000 ? n : n - 0x10000;
}

// Read 4byte signed binary value.
// BigEndian.
long read4Bytes(FILE *f) {
  long n = fgetc(f) << 24;
  n |= fgetc(f) << 16;
  n |= fgetc(f) << 8;
  return n | fgetc(f);
}

// The callers do not check the result, so a failed allocation (or a size
// that comes from a damaged file) is reported by an exception.
void *mymalloc(long n) {
  if (n < 0) throw std::bad_alloc();
  void *p = malloc(n ? n : 1);
  if (p) return p;
  throw std::bad_alloc();
}

void *mycalloc(long n, int size) {
  if (n < 0 || size < 0) throw std::bad_alloc();
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (p) return p;
  throw std::bad_alloc();
}
// ZIP COMPRESSION

// ZIP WITHOUT PREDICTION
// If no error returns 1 else returns 0
int psdUnzipWithoutPrediction(unsigned char *src_buf, int src_len,
                              unsigned char *dst_buf, int dst_len) {
  z_stream stream;
  int state;

  memset(&stream, 0, sizeof(z_stream));
  stream.data_type = Z_BINARY;

  stream.next_in   = (Bytef *)src_buf;
  stream.avail_in  = src_len;
  stream.next_out  = (Bytef *)dst_buf;
  stream.avail_out = dst_len;

  if (inflateInit(&stream) != Z_OK) return 0;

  do {
    state = inflate(&stream, Z_PARTIAL_FLUSH);
    if (state == Z_STREAM_END) break;
    if (state == Z_DATA_ERROR || state != Z_OK) break;
  } while (stream.avail_out > 0);

  inflateEnd(&stream);
  if (state != Z_STREAM_END && state != Z_OK) return 0;

  return 1;
}

// ZIP WITH PREDICTION

int psdUnzipWithPrediction(unsigned char *src_buf, int src_len,
                           unsigned char *dst_buf, int dst_len, int row_size,
                           int color_depth) {
  int status;
  int len;
  unsigned char *buf;

  status = psdUnzipWithoutPrediction(src_buf, src_len, dst_buf, dst_len);
  if (!status) return status;

  // A row of pixels that is not a whole number of rows of the buffer (a bit
  // depth or width of a damaged file) is left as it is, instead of running
  // past the end of the buffer.
  if (row_size <= 0) return 1;
  const int row_bytes = color_depth == 16 ? row_size * 2 : row_size;
  if (row_bytes <= 0) return 1;

  buf = dst_buf;
  while (dst_len >= row_bytes) {
    len = row_size;
    if (color_depth == 16) {
      while (--len) {
        buf[2] += buf[0] + ((buf[1] + buf[3]) >> 8);
        buf[3] += buf[1];
        buf += 2;
      }
      buf += 2;
    } else {
      while (--len) {
        *(buf + 1) += *buf;
        buf++;
      }
      buf++;
    }
    dst_len -= row_bytes;
  }

  return 1;
}
