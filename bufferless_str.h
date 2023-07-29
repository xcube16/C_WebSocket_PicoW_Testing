#ifndef BUFFERLESS_STR_H
#define BUFFERLESS_STR_H

#include <string.h>

typedef struct bl_str_selecter_ {
    char** strs;
    int strs_len;
    int list_index;
    int str_index;
} bl_str_selecter;

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

#endif