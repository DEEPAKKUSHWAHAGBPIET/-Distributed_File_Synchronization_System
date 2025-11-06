#include "file_hasher.h"
#include <string.h>
#include <openssl/evp.h>
#include <stdint.h>

uint32_t rsync_weak_checksum(const unsigned char *buf, size_t len) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + buf[i]) & 0xffff;
        b = (b + a) & 0xffff;
    }
    return (b << 16) | a;
}

void rsync_roll_checksum(uint32_t *a_out, uint32_t *b_out,
                         const unsigned char *old_block,
                         const unsigned char *new_block,
                         size_t block_size, size_t offset) {
    (void)old_block;
    (void)offset;
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < block_size; ++i) {
        a = (a + new_block[i]) & 0xffff;
        b = (b + a) & 0xffff;
    }
    *a_out = a;
    *b_out = b;
}

void md5_hash(const unsigned char *buf, size_t len, unsigned char out16[16]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return;

    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, buf, len);
    EVP_DigestFinal_ex(ctx, out16, NULL);
    EVP_MD_CTX_free(ctx);
}

void compute_sigs_for_file(FILE *f, block_sig_t *sigs, int nblocks, size_t file_size) {
    for (int i = 0; i < nblocks; ++i) {
        size_t offset = (size_t)i * BLOCK_SIZE;
        size_t toread = BLOCK_SIZE;
        if (offset + toread > file_size)
            toread = file_size - offset;

        unsigned char *buf = malloc(toread ? toread : 1);
        if (!buf) { perror("malloc"); exit(1); }

        fseek(f, offset, SEEK_SET);
        size_t r = fread(buf, 1, toread, f);
        (void)r;

        sigs[i].weak = rsync_weak_checksum(buf, toread);
        md5_hash(buf, toread, sigs[i].strong);

        free(buf);
    }
}