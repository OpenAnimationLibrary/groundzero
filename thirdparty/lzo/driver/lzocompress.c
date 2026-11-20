/*
 * usage : lzocompress.exe <size>
 *
 * Read <size> bytes from stdin and write their LZO-compressed data to stdout.
 * Report errors to stderr; return zero only on success.
 */
#include "lzo/lzoconf.h"
#include "lzo/lzo1x.h"

/* portability layer */
#define WANT_LZO_MALLOC 1
#include "lzo/lzoutil.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h>
#endif

static int parse_sizes(const char* argument, lzo_uint* src_len,
                       lzo_uint* max_dst_len) {
  char* end;
  unsigned long long parsed;
  lzo_uint overhead;
  if (*argument < '0' || *argument > '9') return 0;
  errno  = 0;
  parsed = strtoull(argument, &end, 10);
  if (errno == ERANGE || *end != '\0' || parsed > LZO_UINT_MAX) return 0;
  *src_len = (lzo_uint)parsed;
  /* LZO1X worst-case bound, as documented in LZO's examples/simple.c. */
  overhead = *src_len / 16 + 64 + 3;
  if (*src_len > LZO_UINT_MAX - overhead) return 0;
  *max_dst_len = *src_len + overhead;
  return 1;
}

static int compress_buffers(lzo_bytep src, lzo_uint src_len, lzo_bytep dst,
                            lzo_bytep wrkmem) {
  lzo_uint dst_len = 0;
  int result;
  if (fread(src, 1, src_len, stdin) != src_len) {
    fprintf(stderr, "Could not read the declared input buffer size\n");
    return -3;
  }
  result = lzo1x_1_compress(src, src_len, dst, &dst_len, wrkmem);
  if (result != LZO_E_OK) {
    fprintf(stderr, "Compression failed: code = %d\n", result);
    return -3;
  }
  if (fwrite(dst, 1, dst_len, stdout) != dst_len || fflush(stdout) != 0) {
    fprintf(stderr, "Could not write the compressed buffer\n");
    return -3;
  }
  return 0;
}

static int compress_stream(lzo_uint src_len, lzo_uint max_dst_len) {
  lzo_bytep src    = (lzo_bytep)lzo_malloc(src_len == 0 ? 1 : src_len);
  lzo_bytep dst    = (lzo_bytep)lzo_malloc(max_dst_len);
  lzo_bytep wrkmem = (lzo_bytep)lzo_malloc(LZO1X_1_MEM_COMPRESS);
  int result       = -3;
  if (src == NULL || dst == NULL || wrkmem == NULL) {
    fprintf(stderr, "Could not allocate LZO compression buffers\n");
  } else {
    result = compress_buffers(src, src_len, dst, wrkmem);
  }
  lzo_free(wrkmem);
  lzo_free(dst);
  lzo_free(src);
  return result;
}

int main(int argc, char* argv[]) {
  lzo_uint src_len, max_dst_len;
  if (argc != 2) {
    fprintf(stderr,
            "Usage: %s <size>\n    <size> is the input buffer size in bytes\n",
            argv[0]);
    return -1;
  }
  if (!parse_sizes(argv[1], &src_len, &max_dst_len)) {
    fprintf(stderr, "Invalid input buffer size or LZO output size overflow\n");
    return -1;
  }
#if defined(_WIN32)
  /* stdin/stdout must be binary streams */
  if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
      _setmode(_fileno(stdin), _O_BINARY) == -1) {
    fprintf(stderr, "Could not switch standard streams to binary mode\n");
    return -2;
  }
#endif
  if (lzo_init() != LZO_E_OK) {
    fprintf(stderr, "Couldn't initialize lzo\n");
    return -2;
  }
  return compress_stream(src_len, max_dst_len);
}
