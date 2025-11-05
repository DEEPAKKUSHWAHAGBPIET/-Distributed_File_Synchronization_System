// protocol.h
#ifndef PROTOCOL_H

#define PROTOCOL_H

#include <stdint.h>

#define BLOCK_SIZE 1024
#define MAX_PATH_LEN 1024

// Messages types (simple text markers)
#define MSG_SYNC_START "SYNC_START"
#define MSG_SYNC_END   "SYNC_END"
#define MSG_FILE_HDR   "FILE_HDR"    // FILE_HDR <filename> <filesize> <n_blocks>\n
#define MSG_BLOCK_REQ  "BLOCK_REQ"   // BLOCK_REQ <file> <count> <idx1> <idx2> ...
#define MSG_BLOCK_DATA "BLOCK_DATA"  // BLOCK_DATA <file> <idx> <len>\n<binary data>
#define MSG_DONE       "DONE"

typedef struct {
    uint32_t weak;       // rolling/weak checksum
    unsigned char strong[16]; // md5 (16 bytes)
} block_sig_t;

#endif
