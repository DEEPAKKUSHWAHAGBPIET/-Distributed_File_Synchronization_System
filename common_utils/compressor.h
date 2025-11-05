#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <stddef.h>

int compress_block(const unsigned char *in, size_t in_len, unsigned char **outptr);

int decompress_block(const unsigned char *in, size_t in_len, unsigned char **outptr, size_t expected_out_len);

#endif
