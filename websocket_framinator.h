
#include <stdint.h>

// Lets plan on *NOT* sending a frame larger than 64KB.
#define WS_MAX_NO_MASK_HEADER_LEN  (2 + 2)
#define WS_MAX_MASK_HEADER_LEN     (WS_MAX_NO_MASK_HEADER_LEN + 4)

typedef struct ws_framinator_ {

    ws_cliant_con* con;

    // Group our small writes togeather into a larger WS frame.
    // NOTE! To cleverly avoid copying all the time,
    // the max header length (that we will actually ever use) is allocated
    // at the beginning of this buffer!
    char* buf;
    size_t buf_len;

    // Huristics:
    // 1. Small writes will be buffered
    // 2. large writes will flush previously buffered small stuff and it's self
    // 3. 


    // TODO: Timer that will go off after X amount of time has passed
    // since the first element was placed on the buffer. This will prevent
    // a small amout of data from getting stuck if the application pauses
    // for a bit. Although, this is not so very important if the application
    // sends us some hints.

} ws_framinator;


/**
 * @brief Basically a websocket frame template. Place it on buffers and do stuff.
 */
typedef struct ws_frame_ {

    char flags_n_op;
    char len_little;

} ws_frame;




void websocket_write(ws_framinator* ws_con, char* buf, size_t len) {

}