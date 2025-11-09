#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../common_utils/protocol.h"
#include "../common_utils/compressor.h"
#include "../common_utils/file_hasher.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9000

ssize_t read_n(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left > 0)
    {
        ssize_t r = read(fd, p, left);
        if (r <= 0)
            return r;
        left -= r;
        p += r;
    }
    return n;
}

ssize_t write_n(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;
    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w <= 0)
            return w;
        left -= w;
        p += w;
    }
    return n;
}

/* Download a file from the server */
int download_file(const char *fname)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &sa.sin_addr);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        perror("connect");
        return 1;
    }

    char header[512];
    snprintf(header, sizeof(header), "FILE_GET %s\n", fname);
    write_n(sock, header, strlen(header));

    char line[512];
    ssize_t r = recv(sock, line, sizeof(line) - 1, 0);
    if (r <= 0)
    {
        perror("recv");
        close(sock);
        return 1;
    }

    line[r] = '\0';
    if (strncmp(line, MSG_FILE_ERR, strlen(MSG_FILE_ERR)) == 0)
    {
        printf("Server: file not found on server.\n");
        close(sock);
        return 1;
    }

    if (strncmp(line, MSG_FILE_DATA, strlen(MSG_FILE_DATA)) != 0)
    {
        printf("Invalid response from server: %s\n", line);
        close(sock);
        return 1;
    }

    size_t fsize = 0;
    sscanf(line, "FILE_DATA %zu", &fsize);
    printf("Downloading file (%zu bytes)...\n", fsize);

    char outname[256];
    snprintf(outname, sizeof(outname), "downloaded_%s", fname);
    FILE *outf = fopen(outname, "wb");
    if (!outf)
    {
        perror("fopen");
        close(sock);
        return 1;
    }

    size_t total = 0;
    unsigned char buf[4096];
    while (1)
    {
        ssize_t rr = recv(sock, buf, sizeof(buf), 0);
        if (rr <= 0)
            break;

        if (memcmp(buf, MSG_FILE_END, strlen(MSG_FILE_END)) == 0)
            break;

        fwrite(buf, 1, rr, outf);
        total += rr;

        if (total >= fsize)
            break;
    }

    fclose(outf);
    printf("File saved as %s (%zu bytes received)\n", outname, total);
    close(sock);
    return 0;
}

int upload_file(const char *fname)
{
    FILE *f = fopen(fname, "rb");
    if (!f)
    {
        perror("fopen");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    int nblocks = (fsize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    block_sig_t *sigs = malloc(sizeof(block_sig_t) * nblocks);
    if (!sigs)
    {
        perror("malloc");
        fclose(f);
        return 1;
    }

    unsigned char buf[BLOCK_SIZE];
    for (int i = 0; i < nblocks; i++)
    {
        size_t r = fread(buf, 1, BLOCK_SIZE, f);
        sigs[i].weak = rsync_weak_checksum(buf, r);
        md5_hash(buf, r, sigs[i].strong);
    }
    rewind(f);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &sa.sin_addr);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        perror("connect");
        free(sigs);
        fclose(f);
        return 1;
    }

    printf("Performing file synchronization for %s...\n", fname);

    char header[2048];
    int hlen = snprintf(header, sizeof(header),
                        "FILE_HDR %s %zu %d\n", fname, fsize, nblocks);
    write_n(sock, header, hlen);
    write_n(sock, sigs, sizeof(block_sig_t) * nblocks);

    char resp[8192];
    ssize_t rlen = recv(sock, resp, sizeof(resp) - 1, 0);
    if (rlen <= 0)
    {
        perror("recv");
        free(sigs);
        fclose(f);
        close(sock);
        return 1;
    }

    resp[rlen] = '\0';
    int req_count = 0;
    int idxs[1024];
    char *tok = strstr(resp, "\n");
    if (tok)
    {
        tok++;
        char *p = strtok(tok, " \n");
        while (p && req_count < 1024)
        {
            idxs[req_count++] = atoi(p);
            p = strtok(NULL, " \n");
        }
    }
    printf("Server requested %d blocks\n", req_count);

    for (int i = 0; i < req_count; i++)
    {
        int bi = idxs[i];
        fseek(f, (long)bi * BLOCK_SIZE, SEEK_SET);
        size_t got = fread(buf, 1, BLOCK_SIZE, f);

        unsigned char *cbuf = NULL;
        int clen = compress_block(buf, got, &cbuf);
        if (clen < 0)
        {
            cbuf = malloc(got);
            memcpy(cbuf, buf, got);
            clen = (int)got;
        }

        char bheader[128];
        int blen = snprintf(bheader, sizeof(bheader),
                            "BLOCK_DATA %d %d %zu\n", bi, clen, got);
        write_n(sock, bheader, blen);
        write_n(sock, cbuf, clen);
        free(cbuf);
    }

    write_n(sock, "BLOCK_END\n", 10);

    rlen = recv(sock, resp, sizeof(resp) - 1, 0);
    if (rlen > 0)
    {
        resp[rlen] = '\0';
        printf("Server: %s\n", resp);
    }

    fclose(f);
    free(sigs);
    close(sock);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage:\n");
        printf("  %s <filename>           # Upload/sync file\n", argv[0]);
        printf("  %s <filename> --get     # Download file from server\n", argv[0]);
        return 1;
    }

    const char *fname = argv[1];

    if (argc == 3 && strcmp(argv[2], "--get") == 0)
        return download_file(fname);
    else
        return upload_file(fname);
}

