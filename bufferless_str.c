
#include "bufferless_str.h"

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
                    return BL_STR_NO_MATCH; // no match
                }
            } while (strncmp(selecter->strs[selecter->list_index], str, selecter->str_index) != 0);
            str = selecter->strs[selecter->list_index];
        }
    }

    // i is now one index past the end of buf, but str should have a null char
    if (!str[selecter->str_index]) {
        return selecter->list_index;
    }
    return BL_STR_NO_MATCH_YET; // no match yet
}

void bl_str_reset(bl_str_selecter* selecter, const char** strs, int strs_len) {
    selecter->strs = strs;
    selecter->strs_len = strs_len;
    selecter->list_index = 0;
    selecter->str_index = 0;
}

char base64bits(char base64) {
    if (base64 >= 'A' && base64 <= 'Z') {
        return base64 - 'A';
    } else if (base64 >= 'a' && base64 <= 'z') {
        return base64 - ('a' - ('Z' - 'A' + 1));
    } else if (base64 >= '0' && base64 <= '9') {
        return base64 - ('0' - ('Z' - 'A' + 1) - ('z' - 'a' + 1));
    } else if (base64 == '+') {
        return (('Z' - 'A' + 1) + ('z' - 'a' + 1) + ('9' - '0' + 1));
    } else if (base64 == '+') { // ???
        return (('Z' - 'A' + 1) + ('z' - 'a' + 1) + ('9' - '0' + 1) + 1);
    }
    return -1;
}

char bitsToBase64(char bits) {
    if (bits < 26) {
        return 'A' + bits;
    } else if (bits < 52) {
        return 'a' + bits - 26;
    } else if (bits < 62) {
        return '0' + bits - 52;
    } else if (bits == 62) {
        return '+';
    } else if (bits == 63) {
        return '/';
    }
    return -1;
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
                switch (sub_i) {
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

// TODO: pause/resume?
int encode_base64(char* base64_buf, char* input_buf, int input_len) {

    int i;
    int j;

    for (i = j = 0; i < input_len; i += 3) {

        base64_buf[j++] = bitsToBase64(input_buf[i] >> 2);
        if (i < input_len - 1) {
            base64_buf[j++] = bitsToBase64((input_buf[i] << 4 | input_buf[i + 1] >> 4) & 0x3F);
            if (i < input_len - 2) {
                base64_buf[j++] = bitsToBase64((input_buf[i + 1] << 2 | input_buf[i + 2] >> 6) & 0x3F);
                base64_buf[j++] = bitsToBase64(input_buf[i + 2]  & 0x3F);

            } else {
                base64_buf[j++] = bitsToBase64((input_buf[i + 1] << 2) & 0x3F);
                base64_buf[j++] = '=';
            }
        } else {
            base64_buf[j++] = bitsToBase64((input_buf[i] << 4) & 0x3F);
            base64_buf[j++] = '=';
            base64_buf[j++] = '=';
        }
    }
}