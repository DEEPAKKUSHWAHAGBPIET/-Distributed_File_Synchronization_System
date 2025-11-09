#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <errno.h>

#include "../common_utils/protocol.h"
#include "../common_utils/compressor.h"
#include "../common_utils/file_hasher.h"
#include "index_store.h"

#define PORT 9000
#define BACKLOG 10
#define INDEX_FILE "index.db"
#define SYNC_FOLDER "syncedData"

file_index_t *indices = NULL;
int indices_count = 0;
pthread_mutex_t index_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t read_n(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r <= 0) return r;
        left -= r;
        p += r;
    }
    return (ssize_t)n;
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
    return (ssize_t)n;
}

void ensure_folder(const char *folder) {
    struct stat st;
    if (stat(folder, &st) == -1) {
        mkdir(folder, 0755);
    }
}

void handle_client(int client_fd) {
    FILE *outf = NULL;
    char line[4096];

    ssize_t rr = recv(client_fd, line, sizeof(line) - 1, 0);
    if (rr <= 0) { close(client_fd); return; }

    line[rr] = '\0';

    if (strncmp(line, MSG_FILE_GET, strlen(MSG_FILE_GET)) == 0) {
        char req_fname[MAX_PATH_LEN];
        if (sscanf(line, "FILE_GET %s", req_fname) != 1) {
            const char *err = MSG_FILE_ERR "\n";
            write_n(client_fd, err, strlen(err));
            close(client_fd);
            return;
        }

        const char *base = strrchr(req_fname, '/');
        const char *basename = base ? base + 1 : req_fname;

        ensure_folder(SYNC_FOLDER);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", SYNC_FOLDER, basename);

        FILE *f = fopen(path, "rb");
        if (!f) {
            const char *err = MSG_FILE_ERR "\n";
            write_n(client_fd, err, strlen(err));
            fprintf(stderr, "Client requested missing file: %s\n", path);
            close(client_fd);
            return;
        }

        if (fseek(f, 0, SEEK_END) != 0) {
            perror("fseek");
            fclose(f);
            const char *err = MSG_FILE_ERR "\n";
            write_n(client_fd, err, strlen(err));
            close(client_fd);
            return;
        }
        long fsize_long = ftell(f);
        if (fsize_long < 0) fsize_long = 0;
        size_t fsize = (size_t)fsize_long;
        rewind(f);

        char hdr[128];
        int hdrlen = snprintf(hdr, sizeof(hdr), MSG_FILE_DATA " %zu\n", fsize);
        write_n(client_fd, hdr, (size_t)hdrlen);

        unsigned char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (write_n(client_fd, buf, n) <= 0) {
                fprintf(stderr, "Error sending file to client (write)\n");
                break;
            }
        }
        fclose(f);

        write_n(client_fd, MSG_FILE_END "\n", strlen(MSG_FILE_END) + 1);

        printf("Sent file %s (%zu bytes) to client\n", basename, fsize);
        close(client_fd);
        return;
    }

    if (strncmp(line, MSG_FILE_HDR, strlen(MSG_FILE_HDR)) != 0) {
        close(client_fd);
        return;
    }

    char fname[MAX_PATH_LEN];
    size_t fsize;
    int nblocks;
    if (sscanf(line, "FILE_HDR %s %zu %d", fname, &fsize, &nblocks) != 3) {
        fprintf(stderr, "Bad FILE_HDR from client\n");
        close(client_fd);
        return;
    }

    const char *base = strrchr(fname, '/');
    const char *basename = base ? base + 1 : fname;

    char *p = strstr(line, "\n");
    int header_bytes = (p ? (p - line + 1) : rr);
    int remaining = rr - header_bytes;
    block_sig_t *sigs = malloc(sizeof(block_sig_t) * (size_t)nblocks);
    if (!sigs) {
        fprintf(stderr, "malloc sigs failed\n");
        close(client_fd);
        return;
    }
    if (remaining > 0) memcpy(sigs, line + header_bytes, (size_t)remaining);
    if ((int)remaining < (int)(sizeof(block_sig_t) * nblocks)) {
        ssize_t need = (ssize_t)(sizeof(block_sig_t) * nblocks - remaining);
        if (read_n(client_fd, ((char *)sigs) + remaining, (size_t)need) != need) {
            fprintf(stderr, "Failed to read full signatures\n");
            free(sigs);
            close(client_fd);
            return;
        }
    }

    printf("Server: file hdr: %s size=%zu nblocks=%d\n", basename, fsize, nblocks);

    pthread_mutex_lock(&index_lock);
    file_index_t *existing = find_index_by_name(indices, indices_count, basename);
    pthread_mutex_unlock(&index_lock);

    int *req = malloc(sizeof(int) * (size_t)nblocks);
    if (!req) {
        free(sigs);
        close(client_fd);
        return;
    }
    int req_count = 0;
    for (int i = 0; i < nblocks; i++) {
        int match = 0;
        if (existing && existing->nblocks == nblocks) {
            if (existing->sigs[i].weak == sigs[i].weak &&
                memcmp(existing->sigs[i].strong, sigs[i].strong, 16) == 0) {
                match = 1;
            }
        }
        if (!match) req[req_count++] = i;
    }

    char outbuf[8192];
    int pos = snprintf(outbuf, sizeof(outbuf), "BLOCK_REQ %d\n", req_count);
    for (int i = 0; i < req_count; i++) {
        pos += snprintf(outbuf + pos, sizeof(outbuf) - pos, "%d ", req[i]);
    }
    pos += snprintf(outbuf + pos, sizeof(outbuf) - pos, "\n");
    write_n(client_fd, outbuf, (size_t)pos);
    free(req);

    if (req_count > 0) {
        ensure_folder(SYNC_FOLDER);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", SYNC_FOLDER, basename);

        outf = fopen(path, "r+b");
        if (!outf) {
            outf = fopen(path, "wb");
            if (!outf) {
                perror("fopen server output");
                free(sigs);
                close(client_fd);
                return;
            }
        }
        if (ftruncate(fileno(outf), (off_t)fsize) != 0) {
            /* Not fatal; continue */
        }
    } else {
        outf = NULL;
        printf("No blocks requested; file up-to-date.\n");
    }

    while (1) {
        char hdr[256];
        int hpos = 0;
        char ch;
        while (read(client_fd, &ch, 1) == 1) {
            hdr[hpos++] = ch;
            if (ch == '\n' || hpos >= (int)sizeof(hdr) - 1) break;
        }
        if (hpos <= 0) break;
        hdr[hpos] = '\0';

        if (strncmp(hdr, "BLOCK_END", 9) == 0) {
            printf("All blocks received for %s\n", basename);
            break;
        }

        int idx = -1, c_len = 0, orig_len = 0;
        if (sscanf(hdr, "BLOCK_DATA %d %d %d", &idx, &c_len, &orig_len) != 3) {
            fprintf(stderr, "Invalid block header: %s\n", hdr);
            continue;
        }

        unsigned char *cbuf = malloc((size_t)c_len);
        if (!cbuf) {
            fprintf(stderr, "alloc cbuf failed\n");
            break;
        }
        if (read_n(client_fd, cbuf, (size_t)c_len) != (ssize_t)c_len) {
            fprintf(stderr, "Failed to read full block payload (%d bytes)\n", c_len);
            free(cbuf);
            break;
        }

        unsigned char *blockbuf = NULL;
        int dec_len = decompress_block(cbuf, c_len, &blockbuf, (size_t)orig_len);
        free(cbuf);
        if (dec_len < 0) {
            fprintf(stderr, "Decompression failed for block %d\n", idx);
            continue;
        }

        if (outf) {
            if (fseek(outf, (long)idx * BLOCK_SIZE, SEEK_SET) != 0) {
                perror("fseek");
            }
            fwrite(blockbuf, 1, (size_t)dec_len, outf);
        } else {
            fprintf(stderr, "Warning: received data but outf==NULL (idx=%d). Ignoring write.\n", idx);
        }
        free(blockbuf);
        printf("Received block %d (%d bytes compressed)\n", idx, c_len);
    }

    if (outf) {
        fflush(outf);
        fclose(outf);
        outf = NULL;
    }

    file_index_t newidx;
    memset(&newidx, 0, sizeof(newidx));
    strncpy(newidx.filename, basename, MAX_PATH_LEN - 1);
    newidx.filename[MAX_PATH_LEN - 1] = '\0';
    newidx.filesize = fsize;
    newidx.nblocks = nblocks;
    newidx.sigs = sigs;

    pthread_mutex_lock(&index_lock);
    if (replace_or_add_index(&indices, &indices_count, &newidx) != 0) {
        fprintf(stderr, "Failed to update index in memory\n");
    }
    if (save_all_indices(INDEX_FILE, indices, indices_count) != 0) {
        fprintf(stderr, "Failed to save index to %s\n", INDEX_FILE);
    } else {
        printf("Index saved to %s\n", INDEX_FILE);
    }
    pthread_mutex_unlock(&index_lock);

    write_n(client_fd, "FILE_OK\n", 8);
    close(client_fd);
    printf("Connection closed for %s\n", basename);
}

void *thread_main(void *arg) {
    int client_fd = (intptr_t)arg;
    handle_client(client_fd);
    return NULL;
}

int main() {
    ensure_folder(SYNC_FOLDER);

    indices = load_all_indices(INDEX_FILE, &indices_count);
    if (indices)
        printf("Loaded %d existing indices.\n", indices_count);
    else
        indices_count = 0;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }
    if (listen(sockfd, BACKLOG) < 0) {
        perror("listen");
        close(sockfd);
        return 1;
    }
    printf("Server listening on port %d\n", PORT);

    while (1) {
        int c = accept(sockfd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, thread_main, (void *)(intptr_t)c);
        pthread_detach(tid);
    }
    return 0;
}
