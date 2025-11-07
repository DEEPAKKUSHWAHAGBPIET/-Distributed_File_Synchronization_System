#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "protocol.h"
#include "../common_utils/compressor.h"
#include "../common_utils/file_hasher.h"
#include "../common_utils/protocol.h"

#define SERVER_PORT 9000
#define SERVER_IP "127.0.0.1"

ssize_t read_n(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r <= 0) return r;
        left -= r;
        p += r;
    }
    return n;
}

ssize_t write_n(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) return w;
        left -= w;
        p += w;
    }
    return n;
}

int read_line_fd(int fd, char *buf, int maxlen) {
    int pos = 0;
    char ch;
    while (pos < maxlen - 1) {
        ssize_t r = read(fd, &ch, 1);
        if (r <= 0) return -1;
        buf[pos++] = ch;
        if (ch == '\n') break;
    }
    buf[pos] = '\0';
    return pos;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file> [server_ip]\n", argv[0]);
        return 1;
    }
    const char *fname = argv[1];
    const char *server_ip = (argc >= 3) ? argv[2] : SERVER_IP;

    FILE *f = fopen(fname, "rb");
    if (!f) { perror("fopen"); return 1; }

    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    int nblocks = (fsize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (nblocks == 0) nblocks = 1;

    block_sig_t *sigs = malloc(sizeof(block_sig_t) * nblocks);
    if (!sigs) { perror("malloc"); fclose(f); return 1; }

    compute_sigs_for_file(f, sigs, nblocks, fsize);
    fclose(f);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &sa.sin_addr);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        free(sigs);
        return 1;
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
                        "FILE_HDR %s %zu %d\n", fname, fsize, nblocks);
    write_n(sock, header, hlen);
    write_n(sock, sigs, sizeof(block_sig_t) * nblocks);

    char line[8192];
    if (read_line_fd(sock, line, sizeof(line)) <= 0) {
        fprintf(stderr, "Error: no BLOCK_REQ received\n");
        close(sock);
        free(sigs);
        return 1;
    }

    int req_count = 0;
    int req_indices[1024] = {0};
    sscanf(line, "BLOCK_REQ %d", &req_count);

    if (req_count > 0) {
        if (read_line_fd(sock, line, sizeof(line)) > 0) {
            char *tok = strtok(line, " \n");
            int k = 0;
            while (tok && k < req_count) {
                req_indices[k++] = atoi(tok);
                tok = strtok(NULL, " \n");
            }
            req_count = k;
        }
    }

    printf("Server requested %d blocks\n", req_count);

    f = fopen(fname, "rb");
    if (!f) { perror("fopen2"); close(sock); free(sigs); return 1; }

    unsigned char buf[BLOCK_SIZE];
    for (int i = 0; i < req_count; i++) {
        int bi = req_indices[i];
        size_t offset = (size_t)bi * BLOCK_SIZE;
        fseek(f, offset, SEEK_SET);
        size_t got = fread(buf, 1, BLOCK_SIZE, f);
        unsigned char *cbuf = NULL;
        int clen = compress_block(buf, got, &cbuf);
        if (clen < 0) { fprintf(stderr, "Compression failed\n"); continue; }

        char hdr2[128];
        int hdrlen = snprintf(hdr2, sizeof(hdr2),
                              "BLOCK_DATA %d %d %zu\n", bi, clen, got);
        write_n(sock, hdr2, hdrlen);
        write_n(sock, cbuf, clen);
        free(cbuf);
    }

    write_n(sock, "BLOCK_END\n", 10);

    if (read_line_fd(sock, line, sizeof(line)) > 0) {
        printf("%s", line);
    }

    write_n(sock, "SYNC_END\n", 9);

    close(sock);
    free(sigs);
    return 0;
}
