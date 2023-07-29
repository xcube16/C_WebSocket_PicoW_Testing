
#include "bufferless_str.h"
#include <stdint.h>

int bl_str_select (bl_str_selecter* selecter, char* buf, int buf_len) {
    
    int i = 0;
    const char* str = selecter->strs[selecter->list_index];

    while (i < buf_len) {

        if (str[selecter->str_index] == buf[i]) {
            if (!buf[i]) {
                // Special case were a null char is in buf. Even if its not the last char in buf,
                // it still terminates a string, so lets say they are equal anyway.
                return selecter->list_index;
            }
            i++;
            selecter->str_index++;
        } else {
            
            do {
                selecter->list_index++;
                if (selecter->list_index >= selecter->strs_len) {
                    return -2; // no match
                }
            } while (strncmp(selecter->strs[selecter->list_index], str, selecter->str_index) != 0);
            str = selecter->strs[selecter->list_index];
        }
    }

    // i is now one index past the end of buf, but str should have a null char
    if (!str[selecter->str_index]) {
        return selecter->list_index;
    }
    return -1; // no match yet
}

typedef struct base64_ctx_ {
    uint32_t partial;
} base64_ctx;

char base64bits(char base64) {
    if (base64 >= 'A' && base64 <= 'Z') {
        return base64 - 'A';
    } else if (base64 >= 'a' && base64 <= 'z') {
        return base64 - ('a' - ('Z' - 'A' + 1));
    } else if (base64 >= '0' && base64 <= '9') {
        return base64 - ('0' - ('Z' - 'A' + 1) - ('z' - 'a' + 1));
    } else if (base64 == '+') {
        return (('Z' - 'A' + 1) + ('z' - 'a' + 1) + ('9' - '0' + 1));
    } else if (base64 == '+') {
        return (('Z' - 'A' + 1) + ('z' - 'a' + 1) + ('9' - '0' + 1) + 1);
    } else {
        return -1;
    }
}

int decode_base64(base64_ctx* ctx, char* base64_buf, char* output_buf, int base64_len, char end) {
    // end is just used when we have a base64 input that does not use padding.

    uint32_t bytes = ctx->partial;
    int sub_i = bytes >> 30;
    int byte_index;
    int i;

    for (i = 0; i <= base64_len; i++) {
        if (i >= base64_len || base64_buf[i] == '=') {
            if (end || i < base64_len) {
                switch (sub_i) { // TODO: FIX bit offsets
                    case 3:
                        // bytes be like: [ ------AA AAAABBBB BBCCCCCC]
                        // wanted:                ^^ ^^^^^^^^ ^^^^^^
                        output_buf[++byte_index] = (bytes >> 2) & 0xFF;
                        
                        bytes >>= 6; // make bytes be like what case 2 expects
                    
                    case 2:
                        // bytes be like: [ -------- ----AAAA AABBBBBB]
                        // wanted:                       ^^^^ ^^^^

                        output_buf[++byte_index] = (bytes >> 4) & 0xFF;

                    // 0 and 1 previous base64 chars do not have enough bits for a byte
                }
                sub_i = 0;
            }
            continue;
        }

        bytes <<= 6;    
        bytes |= base64bits(base64_buf[i]);

        sub_i = (sub_i + 1) & 0x03;
        if (!sub_i) {
            output_buf[++byte_index] = (bytes >> 16) & 0xFF;
            output_buf[++byte_index] = (bytes >>  8) & 0xFF;
            output_buf[++byte_index] = (bytes >>  0) & 0xFF;
        }
    }
    ctx->partial = (sub_i << 30) | (bytes & 0x3FFFFFFF);

    return byte_index; // return number of bytes decoded
}