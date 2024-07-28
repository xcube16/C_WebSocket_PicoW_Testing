
#include <stdint.h>
#include <stdalign.h>
#include "lwip/err.h"

// Arbetrary huristics
#define WS_BUF_STARTING_LEN 1024
#define WS_MAX_PAYLOAD_LEN 256 // maybe make this a small part of the buffer we allocate.
#define WS_ITS_LARGE_ENOUGH_JUST_SEND_IT 192
#define WS_JUST_WRAP_ANYWAY_ITS_NOT_WORTH_IT_PAYLOAD_LEN 16

// Lets plan on *NOT* sending a frame larger than 64KB.
#define WS_MAX_NO_MASK_HEADER_LEN  (2 + 2)
#define WS_MAX_MASK_HEADER_LEN     (WS_MAX_NO_MASK_HEADER_LEN + 4)

#define WS_MRK_FLAG_WRAP 0x40000000
#define WS_MRK_SCRATCH_LEN(val)        ((uint32_t)val << 24)
#define WS_MRK_GET_SCRATCH_LEN(packed) (((uint32_t)packed >> 24) & 0x3F)
#define WS_MRK_LEN(val)                ((uint32_t)val)
#define WS_MRK_GET_LEN(packed)         ((uint32_t)packed & 0x00FFFFFF)

#define WS_HEADER_FIN  0x8000
#define WS_HEADER_OPCODE(code) ((uint16_t)code << 8);
#define WS_HEADER_OPCODE_CONTINUATION 0x0
#define WS_HEADER_OPCODE_TEXT         0x1
#define WS_HEADER_OPCODE_DATA         0x2
#define WS_HEADER_OPCODE_CLOSE        0x8
#define WS_HEADER_OPCODE_PING         0x9
#define WS_HEADER_OPCODE_PONG         0xA
#define WS_HEADER_MASK 0x0010
#define WS_HEADER_PAYLOAD_LEN_USE_16BIT 126
#define WS_HEADER_PAYLOAD_LEN_USE_64BIT 127

typedef struct ws_framinator_ {

    ws_cliant_con* con;

    // Group our small writes togeather into a larger WS frame.
    // NOTE! To cleverly avoid copying all the time,
    // the max header length (that we will actually ever use) is allocated
    // at the beginning of this buffer!
    char* buf;
    size_t buf_len;

    size_t head;
    size_t tail;

    // Current marker/frame that we are building
    size_t current_marker;
    // The frame's payload starts at (current_marker + sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN).

    // The current marker/frame's size. We will set this in the header just before writing it out.
    size_t current_payload_len;

    // example buf structure:
    // ...
    // <tail>--->
    // [ws_buf_marker]
    //     [max ws header][...]
    // {alignment}
    // [ws_buf_marker_ack_callback] // not implemented!
    // [ws_buf_marker]
    //     [max ws header][...]
    // {alignment}
    // [ws_buf_marker]
    //     [max ws header][...]
    // <head>--->
    // ...

    // When re-allocating the buffer, TCP must be flushed as it has references into the buffer.
    // Also correcting for a special case when wrapping around to fit in a ws_buf_marker would cause issues.

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
 * @brief pre-pended to the frame in our buffer.
 */
typedef struct ws_buf_marker_ {

    // <2 bits flags><6 bit scratch length><24 bit length to be ack'ed>
    // Scratch length is an extra number of bytes on the buffer that ended
    // up not being sent; thus, they will not be ack'ed and must be skipped.
    int32_t flags_and_len;

} ws_buf_marker;

/**
 * @brief Basically a websocket frame template.
 */
typedef struct ws_frame_small_ {
    ws_buf_marker marker; // Remember to use + sizeof(ws_buf_marker) + sizeof(uint16_t) bytes scratch
    uint16_t empty_space;

    // ----> <>
    uint16_t header;      // <----- Send from here

} ws_frame_small;

/**
 * @brief Basically a websocket large frame template.
 */
typedef struct ws_frame_large_ {
    ws_buf_marker marker; // Remember to use + sizeof(ws_buf_marker) bytes scratch

    uint16_t header;      // <----- Send from here
    uint16_t e_payload_len;

} ws_frame_large;

err_t websocket_framinator_ack_callback(void* arg, u16_t len) {
    ws_framinator* framinator = (ws_framinator*) arg;

    while (len > 0) {
        int flags_and_len = ((ws_buf_marker*) (framinator->buf + framinator->tail))->flags_and_len;

        if (flags_and_len & WS_MRK_FLAG_WRAP) {
            framinator->tail = 0;
        } else {
            size_t to_ack = MIN(len, WS_MRK_GET_LEN(flags_and_len));
            if (len >= to_ack) {
                // Fully consume the last marker.
                framinator->tail += to_ack + WS_MRK_GET_SCRATCH_LEN(flags_and_len);
            } else {
                // Advance the current marker, keep it aligned and keep track of padding bytes.
                size_t advance = WS_MRK_GET_SCRATCH_LEN(flags_and_len) + to_ack;
                size_t new_scratch = advance % alignof(ws_buf_marker);
                advance -= new_scratch;
                framinator->tail += advance;

                ((ws_buf_marker*) (framinator->buf + framinator->tail))->flags_and_len =
                          WS_MRK_SCRATCH_LEN(new_scratch)
                        | WS_MRK_LEN(WS_MRK_GET_LEN(flags_and_len) - to_ack);
            }
            len -= to_ack;
        }
    }

    return ERR_OK;
}

err_t websocket_initialize_framinator(ws_framinator* framinator, ws_cliant_con* con) {
    framinator->con = con;
    if (TCP_SND_BUF - tcp_sndbuf(con->printed_circuit_board) != 0)
        return ERR_BUF; // snd_buf must have max space
    set_ack_callback(con, websocket_framinator_ack_callback, framinator);

    framinator->buf_len = WS_BUF_STARTING_LEN;
    if(!(framinator->buf = malloc(framinator->buf_len)))
        return ERR_MEM;
    framinator->tail = 0;

    framinator->current_marker = 0;
    framinator->head = framinator->current_marker + sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;

    framinator->current_payload_len = 0;

    return ERR_OK;
}

void websocket_complete_and_send_frame(ws_framinator* ws_con) {

    // TODO: Compute number of scratch/no-ack bytes fromws_con->current_payload_len
    // and by assuming ws_con->head is the true end.

    uint16_t header = WS_HEADER_FIN | WS_HEADER_OPCODE(WS_HEADER_OPCODE_DATA);
    if (ws_con->current_payload_len >= WS_HEADER_PAYLOAD_LEN_USE_16BIT) {
        header |= WS_HEADER_PAYLOAD_LEN_USE_16BIT;
        ws_frame_large* frame = ((ws_frame_large*) (ws_con->buf + ws_con->current_marker));

        size_t send_len = ws_con->current_payload_len + (sizeof(ws_frame_large) - offsetof(ws_frame_large, header));

        frame->marker.flags_and_len = WS_MRK_SCRATCH_LEN(
            ws_con->head - ws_con->current_marker // total length
            - send_len) // subtract length to be sent
            | WS_MRK_LEN(send_len);
        frame->header = (header << 8) | (header >> 8); // TODO: oops! quick fix for little endian u16.
        frame->e_payload_len = (ws_con->current_payload_len << 8) | (ws_con->current_payload_len >> 8); // TODO: oops! quick fix for little endian u16.

        ws_t_write(ws_con->con, &(frame->header),
                send_len,
                0 /*no flags*/);

    } else {
        header |= ws_con->current_payload_len;
        ws_frame_small* frame = ((ws_frame_small*) (ws_con->buf + ws_con->current_marker));

        size_t send_len = ws_con->current_payload_len + (sizeof(ws_frame_small) - offsetof(ws_frame_small, header));

        frame->marker.flags_and_len = WS_MRK_SCRATCH_LEN(
            ws_con->head - ws_con->current_marker // total length
            - send_len) // subtract length to be sent
            | WS_MRK_LEN(send_len);
        frame->header = (header << 8) | (header >> 8); // TODO: oops! quick fix for little endian u16.

        ws_t_write(ws_con->con, &(frame->header),
                send_len,
                0 /*no flags*/);
    }

    tcp_output(ws_con->con->printed_circuit_board);

    ws_con->current_payload_len = 0;
}

err_t websocket_write(ws_framinator* ws_con, char* buf, size_t len) {
    static_assert(WS_JUST_WRAP_ANYWAY_ITS_NOT_WORTH_IT_PAYLOAD_LEN <= (0b00111111 - sizeof(ws_buf_marker)),
                  "Can't skip over a size greater than about 6 bits");
    static_assert(alignof(ws_buf_marker) <= sizeof(ws_buf_marker) && sizeof(ws_buf_marker) % alignof(ws_buf_marker) == 0,
                  "Sanity check as we use sizeof(ws_buf_marker) to ensure enough space at the end of the struct");
    err_t ret;

    while (len > 0) {
        size_t space;


        if (ws_con->head < ws_con->tail) {
            // make sure we can fit an aligned ws_buf_marker at the end.
            space = ws_con->tail - ws_con->head
                   - sizeof(ws_buf_marker) - ws_con->tail % alignof(ws_buf_marker);

            if (space <= 0) {
                if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, ws_con->con->task)) {
                    return ret;
                }
                ws_con->con->notify_ack = false;
                continue;
            }

        } else {

            // empty space at the end of the buffer:
            space = ws_con->buf_len - ws_con->head
                  - (sizeof(ws_buf_marker)); // save room for a wrap marker. Assume buf_len is aligned.

        }

        space = MIN(space, MIN(WS_MAX_PAYLOAD_LEN - ws_con->current_payload_len, len));
        memcpy(ws_con->buf + ws_con->head, buf, space);
        // update frame builder's state
        ws_con->current_payload_len += space;
        ws_con->head += space;
        // update what we have left to copy;
        len -= space;
        buf += space;

        // Should we send the frame?
        if (ws_con->head >= ws_con->buf_len - sizeof(ws_buf_marker)
            || ws_con->current_payload_len >= WS_ITS_LARGE_ENOUGH_JUST_SEND_IT
            /*||  TODO: enough time has passed since the first bytes on this frame */) {

            if (ws_con->buf_len - ws_con->head - sizeof(ws_buf_marker) < WS_JUST_WRAP_ANYWAY_ITS_NOT_WORTH_IT_PAYLOAD_LEN) {
                // Case spagetti, yikes.
                // Head/tail could be in any order, but head is getting too close to buf_len.

                // Create a new wrap marker in preparation to loop head back to the start of the buffer.

                // We know that there is at least enough space for a wrap marker
                // >>> ADVANCE HEAD >>>
                // Pad head forward a bit if needed
                uint8_t padding = (alignof(ws_buf_marker) - ws_con->head % alignof(ws_buf_marker)) % alignof(ws_buf_marker);
                ws_con->head += padding;
                ((ws_buf_marker*) (ws_con->buf + ws_con->head))->flags_and_len =
                    WS_MRK_FLAG_WRAP/*| WS_MRK_SCRATCH_LEN(ws_con->buf_len - ws_con->head) ignored when wrap is set*/;

                websocket_complete_and_send_frame(ws_con);

                // Ensure that there is enough space at the start of the buffer.
                while (ws_con->tail > ws_con->head || ws_con->tail < sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN) {
                    if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, ws_con->con->task)) {
                        return ret;
                    }
                    ws_con->con->notify_ack = false;
                }

                // >>> ADVANCE HEAD >>>
                ws_con->current_marker = 0;
                ws_con->head = sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;
                // TODO: Low priority. Maybe we don't need to initialize it to zero as we will enter the values
                // when ready to advance ws_con->head/send the packet anyway.
                ((ws_buf_marker*) (ws_con->buf + ws_con->current_marker))->flags_and_len = 0x00000000;
                // >>>              >>>

            } else if (ws_con->head >= ws_con->tail) {
                // tail behind head

                // got handled above: if (space < WS_JUST_WRAP_ANYWAY_ITS_NOT_WORTH_IT_PAYLOAD_LEN)

                // We have enough space for a frame right after this one.

                // >>> ADVANCE HEAD >>>
                // Pad head forward a bit if needed
                uint8_t padding = (alignof(ws_buf_marker) - ws_con->head % alignof(ws_buf_marker)) % alignof(ws_buf_marker);
                ws_con->head += padding;

                websocket_complete_and_send_frame(ws_con);

                ws_con->current_marker = ws_con->head;
                ws_con->head += sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;
                // TODO: Low priority. Maybe we don't need to initialize it to zero as we will enter the values
                // when ready to advance ws_con->head/send the packet anyway.
                ((ws_buf_marker*) (ws_con->buf + ws_con->current_marker))->flags_and_len = 0x00000000;
                // >>>              >>>

            } else {
                // Snake about to eat it's own tail

                // empty space at the end of the buffer:
                space = ws_con->tail - ws_con->head
                    - (sizeof(ws_buf_marker)); // save room for a wrap marker.

                if (space < sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN) {
                    // not enough space

                    // >>> ADVANCE HEAD >>>
                    // Pad head forward a bit if needed
                    uint8_t padding = (alignof(ws_buf_marker) - ws_con->head % alignof(ws_buf_marker)) % alignof(ws_buf_marker);
                    ws_con->head += padding;

                    websocket_complete_and_send_frame(ws_con);

                    // Ensure that there is enough space at the start of the buffer.
                    while (ws_con->tail > ws_con->head && ws_con->tail - ws_con->head < sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN) {
                        if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, ws_con->con->task)) {
                            return ret;
                        }
                        ws_con->con->notify_ack = false;
                    }

                    // >>> ADVANCE HEAD >>>
                    ws_con->current_marker = ws_con->head;
                    ws_con->head += sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;
                    // TODO: Low priority. Maybe we don't need to initialize it to zero as we will enter the values
                    // when ready to advance ws_con->head/send the packet anyway.
                    ((ws_buf_marker*) (ws_con->buf + ws_con->current_marker))->flags_and_len = 0x00000000;
                    // >>>              >>>
                } else {
                    // We have enough space for a frame right after this one.

                    // >>> ADVANCE HEAD >>>
                    // Pad head forward a bit if needed
                    uint8_t padding = (alignof(ws_buf_marker) - ws_con->head % alignof(ws_buf_marker)) % alignof(ws_buf_marker);
                    ws_con->head += padding;

                    websocket_complete_and_send_frame(ws_con);

                    ws_con->current_marker = ws_con->head;
                    ws_con->head += sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;
                    // TODO: Low priority. Maybe we don't need to initialize it to zero as we will enter the values
                    // when ready to advance ws_con->head/send the packet anyway.
                    ((ws_buf_marker*) (ws_con->buf + ws_con->current_marker))->flags_and_len = 0x00000000;
                    // >>>              >>>
                }
            }
        }
    }
    return ERR_OK;
}

err_t websocket_flush(ws_framinator* ws_con) {
    err_t ret;

    if (ws_con->current_payload_len > 0) {
        websocket_complete_and_send_frame(ws_con);
    }

    // Wait for ACK until the entire buffer is flushed.
    while (ws_con->tail != ws_con->head) {
        if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, ws_con->con->task)) {
            return ret;
        }
        ws_con->con->notify_ack = false;
    }

    // reset the buffer
    ws_con->tail = 0;
    ws_con->current_marker = 0;
    ws_con->head = sizeof(ws_buf_marker) + WS_MAX_NO_MASK_HEADER_LEN;
    // TODO: Low priority. Maybe we don't need to initialize it to zero as we will enter the values
    // when ready to advance ws_con->head/send the packet anyway.
    ((ws_buf_marker*) (ws_con->buf + ws_con->current_marker))->flags_and_len = 0x00000000;

    return ERR_OK;
}
