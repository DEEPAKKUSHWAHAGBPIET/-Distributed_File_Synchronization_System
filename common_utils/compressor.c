#include "compressor.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>
#include<stdio.h>

int compress_block(const unsigned char *in, size_t in_len, unsigned char **outptr) {
    if (!in || !outptr) return -1;
    
    uLong bound = compressBound((uLong)in_len);
    unsigned char *out = malloc(bound);
    if (!out) return -1;
    uLongf destLen = bound;
    int rc = compress2(out, &destLen, in, (uLong)in_len, Z_BEST_SPEED);
    if (rc != Z_OK) {
        free(out);
        return -1;
    }
    
    unsigned char *shrink = realloc(out, destLen);
    if (shrink) out = shrink;
    *outptr = out;
    printf("Compressed %zu -> %lu bytes\n", in_len, destLen);
    return (int)destLen;
}

int decompress_block(const unsigned char *in, size_t in_len, unsigned char **outptr, size_t expected_out_len) {
    if (!in || !outptr) return -1;
    unsigned char *out = malloc(expected_out_len);
    if (!out) return -1;
    uLongf destLen = (uLongf)expected_out_len;
    int rc = uncompress(out, &destLen, in, (uLong)in_len);
    if (rc != Z_OK || destLen != expected_out_len) {
        free(out);
        return -1;
    }
    *outptr = out;
    return (int)destLen;
}
