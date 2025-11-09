#ifndef PROTOCOL_H

#define PROTOCOL_H

#include <stdint.h>

#define BLOCK_SIZE 1024
#define MAX_PATH_LEN 1024

#define MSG_SYNC_START "SYNC_START"
#define MSG_SYNC_END   "SYNC_END"
#define MSG_FILE_HDR   "FILE_HDR"    
#define MSG_BLOCK_REQ  "BLOCK_REQ"  
#define MSG_BLOCK_DATA "BLOCK_DATA"  
#define MSG_DONE       "DONE"

#define MSG_FILE_GET  "FILE_GET"
#define MSG_FILE_DATA "FILE_DATA"
#define MSG_FILE_END  "FILE_END"
#define MSG_FILE_ERR  "FILE_ERR"


typedef struct {
    uint32_t weak;       
    unsigned char strong[16]; 
} block_sig_t;

#endif
