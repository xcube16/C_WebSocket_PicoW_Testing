#ifndef BUFFERLESS_STR_H
#define BUFFERLESS_STR_H

#include <string.h>

#define BL_STR_NO_MATCH_YET -1
#define BL_STR_NO_MATCH -2

typedef struct bl_str_selecter_ {
    const char** strs;
    int strs_len;
    int list_index;
    int str_index;
} bl_str_selecter;

/**
 * @brief Compares a stream of incomming data to an array of strings without the need
 * to buffer the entire incomming string.
 * 
 * If the end of buf is reached and partial matches are matches exist, -1 is returned.
 * Example:
 * selecter contains {"foo"}
 * 
 * @param selecter 
 * @param buf 
 * @param buf_len 
 * @return int 
 */
int bl_str_select (bl_str_selecter* selecter, char* buf, int buf_len);

void bl_str_reset (bl_str_selecter* selecter, char** strs);
#endif