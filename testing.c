
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

#include "lwipopts.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "bufferless_str.h"
#include "sub_task.h"
#include "iol_lock.h"

#include "mbedtls/sha1.h"

#define DEBUG_printf printf

// cyw43_arch.c does not expose this helper function (pure), but we want it.
static const char* cyw43_tcpip_link_status_name(int status) {
    switch (status) {
    case CYW43_LINK_DOWN:
        return "link down";
    case CYW43_LINK_JOIN:
        return "joining";
    case CYW43_LINK_NOIP:
        return "no ip";
    case CYW43_LINK_UP:
        return "link up";
    case CYW43_LINK_FAIL:
        return "link fail";
    case CYW43_LINK_NONET:
        return "network fail";
    case CYW43_LINK_BADAUTH:
        return "bad auth";
    }
    return "unknown";
}

// ============== Configuration and Stuff for Main ===========
const char wifi_ssid[] = "placeholder";
const char wifi_password[] = "placeholder";
#define TCP_PORT 8080

// TODO: Support multiple connections
SUB_TASK_GLOBAL(task_ws_header, 1020);

// =============== Header Processing stuff ===========
// recieve
#define WS_H_FIELD_UPGRADE 0
#define WS_H_FIELD_KEY 1

#define WS_KEY_LEN 24

#define WS_H_FIELDS_LEN 2
const char* WS_H_FIELDS[] = {
    "Upgrade",
    "Sec-WebSocket-Key",
};

// send
const char ws_responce1[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: upgrade\r\n"
    "Sec-WebSocket-Accept: ";

const char ws_responce2[] =
    "\r\nSec-WebSocket-Protocol: chat\r\n\r\n";

const char ws_uuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const char ws_page_responce1[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: \r\n"
    "Content-Length: ";

// TODO: Make the linker handle this with a normal HTML file
const char ws_page_responce2[] =
    "\r\n"
    "\r\n";

//const char ws_page_body[] =
//    "<!doctype html><html><body>Test page. TODO: add a script to connet via websocket and display information</body></html>";
#include "index_html.h"

// ================ CLIANT CONNECTION ================

#define WS_T_YIELD_REASON_READ 1
#define WS_T_YIELD_REASON_FLUSH 2
#define WS_T_YIELD_REASON_WAIT_FOR_ACK 3

typedef struct ws_ack_callback_ {
    err_t (*call)(void*, u16_t);
    void* arg;
} ws_ack_callback;

typedef struct ws_cliant_con_ {
    struct tcp_pcb* server_pcb; // TODO: remove server pcb.
    struct tcp_pcb* printed_circuit_board; // I honistly have no idea

    ws_ack_callback ack_callback;

    // Holds NULL or points to a chain pbufs accumulated by our tcp_recv callback
    struct pbuf* p_current;
    // Count the data we have processed so we can call tcp_receved in one shot, usually after a yield.
    size_t recved_current;

    // Basically a thread that is handling a single connection.
    // TODO: Multiple connections!
    // TODO: Support multiple threads. Maybe the connection should only track threads that
    //       are waiting for it. Some users might want a reader and writer thread.
    sub_task* task;
    iol_lock_obj io_task;

    // TODO: better WS_T_YIELD_REASON_WAIT_FOR_ACK
    bool notify_ack;

} ws_cliant_con;

void set_ack_callback(ws_cliant_con* cli_con, err_t (*call)(void*, u16_t), void* arg) {
    cli_con->ack_callback.call = call;
    cli_con->ack_callback.arg = arg;
}

// normal helper functions

/**
 * @brief Returns a condiguious byte array of the given size or null if we need to wait for more data.
 *
 * @param cli_con
 * @param buf
 * @param size size of buf (max size that can be read)
 * @return The number of bytes actually read or an error code
 */
int ws_read(ws_cliant_con* cli_con, char* buf, size_t size) {
    if (!cli_con->p_current) {
        return cli_con->printed_circuit_board == NULL ? ERR_CLSD : 0; // We are still waiting for more data
    }

    // TODO: pbuf_copy_partial returns 0 on 'failure'. Check that
    // fill the buffer until it reaches size or we run out of pbufs in the chain
    u16_t bytes_read = pbuf_copy_partial(
        cli_con->p_current,
        buf,
        size,
        0);
    cli_con->p_current = pbuf_free_header(cli_con->p_current, bytes_read);
    cli_con->recved_current += bytes_read;

    // Inform the TCP stack of how many bytes we processed. Could be zero
    // (if the task yields to peek then yields to flush for example).
    // TODO: Find a better place to put this ACK. We may want to do it more often for example,
    // if a large volume of data is being sent and we are just barly keeping up. Once
    // enough data has not been acked (but we have not hit this point, aka yielded for more because we slow).
    // The connection may hickup resulting in a lag spike (or worse?) when it could smoothly chug along.
    if (cli_con->printed_circuit_board != NULL) {
        tcp_recved(cli_con->printed_circuit_board, cli_con->recved_current);
        cli_con->recved_current = 0;
    }

    return bytes_read; // We are still waiting for more data
}

/**
 * @brief Provides access to the next raw pbuf payload. Call ws_consume() or ws_read() to
 * actually advance the communication.
 *
 * @param cli_con
 * @param buf_ptr Pointer to a buffer. Will be set if the number of bytes is > 0
 * @return size_t Number of bytes available
 */
size_t ws_peak(ws_cliant_con* cli_con, char** buf_ptr) {
    if (!cli_con->p_current) {
        *buf_ptr = NULL;
        return 0; // We are still waiting for more data
    }
    *buf_ptr = cli_con->p_current->payload;
    return cli_con->p_current->len;
}

int ws_consume(ws_cliant_con* cli_con, size_t size) {
    if (!cli_con->p_current) {
        if (size == 0) {
            return ERR_OK; // consume 0 bytes? well ok.
        }
        printf("ERROR: Can't consume bytes that we dont have yet! %d\n", size);
        return ERR_ARG;
    }

    cli_con->p_current = pbuf_free_header(cli_con->p_current, size);
    cli_con->recved_current += size;

    // Inform the TCP stack of how many bytes we processed. Could be zero
    // (if the task yields to peek then yields to flush for example).
    // TODO: Find a better place to put this ACK. We may want to do it more often for example,
    // if a large volume of data is being sent and we are just barly keeping up. Once
    // enough data has not been acked (but we have not hit this point, aka yielded for more because we slow).
    // The connection may hickup resulting in a lag spike (or worse?) when it could smoothly chug along.
    if (cli_con->printed_circuit_board != NULL) {
        tcp_recved(cli_con->printed_circuit_board, cli_con->recved_current);
        cli_con->recved_current = 0;
    }

    return ERR_OK;
}

// end normal helper functions

// threaded helper functions

/**
 * @brief Returns a condiguious byte array of the given size by yielding when more is needed.
 *
 * @param cli_con
 * @param buf
 * @param size size of buf (max size that can be read)
 * @return The number of bytes actually read or an error code
 */
int ws_t_read(ws_cliant_con* cli_con, char* buf, size_t size) {
    int ret = 0;

    while (size > ret) {
        int r = ws_read(cli_con, buf + ret, size - ret);
        if (r < 0) {
            return r;
        }

        ret += r;

        if (size > ret) {

            if (r = (int) sub_task_yield(WS_T_YIELD_REASON_READ, cli_con->task)) {
                return r;
            }
        }
    }
    return ret;
}

/**
 * @brief Provides access to the next raw pbuf payload. Call ws_consume() or ws_read() to
 * actually advance the communication. Threaded, yielding.
 *
 * @param cli_con
 * @param buf_ptr Pointer to a buffer. Will be set if the number of bytes is > 0
 * @return int Number of bytes available, or e negative error code.
 */
int ws_t_peak(ws_cliant_con* cli_con, char** buf_ptr) {
    int ret;
    while ((ret = ws_peak(cli_con, buf_ptr)) == 0) {

        int err;
        if (err = (int) sub_task_yield(WS_T_YIELD_REASON_READ, cli_con->task)) {
            return err;
        }
    }
    return ret;
}

err_t ws_t_write(ws_cliant_con* cli_con, void* dataptr, size_t len, u8_t apiflags/*, tcpwnd_size_t* countdown*/) {
    err_t ret;

    if (cli_con->printed_circuit_board == NULL) {
        return ERR_CLSD;
    }

    // Wait until we have at least *some* space on the send buffer
    while (tcp_sndbuf(cli_con->printed_circuit_board) == 0) {
        tcp_output(cli_con->printed_circuit_board);
        if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, cli_con->task)) {
            return ret;
        }
        cli_con->notify_ack = false;
    }

    while (tcp_sndbuf(cli_con->printed_circuit_board) < len) {
        // Looks like we are trying to send more than can fit on the send buffer.
        // Well, lets shove in as much as we can and wait.

        // write out the max amount
        u16_t space_available = tcp_sndbuf(cli_con->printed_circuit_board);
        if (ret = tcp_write(cli_con->printed_circuit_board, dataptr, space_available,
                apiflags | TCP_WRITE_FLAG_MORE)) { // Set the MORE flag if it's not already set.
            return ret;
        }
        len -= space_available;
        dataptr += space_available;

        // Now that the send buffer is maxed out, lets wait for at some of it to drain out
        tcp_output(cli_con->printed_circuit_board);
        if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_WAIT_FOR_ACK, cli_con->task)) {
            return ret;
        }
        cli_con->notify_ack = false;
    }
    tcp_write(cli_con->printed_circuit_board, dataptr, len, apiflags);

    //*countdown = TCP_SND_BUF - tcp_sndbuf(cli_con->printed_circuit_board);
    return ERR_OK;
}

size_t ws_t_write_barrier(ws_cliant_con* cli_con) {

    // TODO: This is basically just a flush-the-entire-TCP function.
    //       Not very good, but it works. We will need a better system based
    //       on markers (aquired when writing) via our future threaded tcp_write
    //       function. Should these be absolute markers? Or just a countdown
    //       to its-safe-to-free-the-buffer? I'm leaning in the direction
    //       of countdown if we can make the tcp_sent callback not suck.

    if (cli_con->printed_circuit_board == NULL) {
        return ERR_CLSD;
    }

    while (cli_con->printed_circuit_board->snd_queuelen) {
        size_t ret;
        if (ret = (size_t) sub_task_yield(WS_T_YIELD_REASON_FLUSH, cli_con->task)) {
            return ret;
        }
    }
}

bool ws_check_reason(void* user_obj, size_t reason, size_t err) {
    ws_cliant_con* cli_con = user_obj;

    if (reason && err) {
        return true; // Let the task handle the error.
    }

    switch (reason) {
        case WS_T_YIELD_REASON_READ:
            return cli_con->p_current != NULL;

        case WS_T_YIELD_REASON_FLUSH:
            // When no more pbufs are in the send buffer, we are flushed. All of them have been ack'ed.
            // Checking that tcp_sndbuf is at it's max would also work.
            return cli_con->printed_circuit_board == NULL || !cli_con->printed_circuit_board->snd_queuelen;

        case WS_T_YIELD_REASON_WAIT_FOR_ACK:
            // TODO: better WS_T_YIELD_REASON_WAIT_FOR_ACK
            return cli_con->notify_ack;

        case IOL_YIELD_REASON_END:
            return false; // ya, don't continue if we ended. That would cause a crash.

        default:
            DEBUG_printf("Unimplemented reason: %i\n", reason);
            return false;
    }
}

// end threaded helper functions

// TODO: cleanup this crazy header
#include <websocket_framinator.h>

/**
 * @brief Eats ':', ' ', sneezes when it hits a '\n' (returns 1), and returns 0 for any other char.
 *
 * @param cli_con The connection handle
 * @return int 0 for non-whitespace, 1 for '\n', negative for read errors
 */
int ws_eat_whitespace(ws_cliant_con* cli_con) {
    char* buf;
    int size;
    do {
        if ((size = ws_t_peak(cli_con, &buf)) < 0) {
            return size; // error
        }
        int i;
        for (i = 0; i < size; i++) {
            char c = buf[i];
            if (c != ':' && c != ' ' && c != '\r') {
                ws_consume(cli_con, i + (c == '\n'));
                return c == '\n';
            }
        }
        ws_consume(cli_con, i);
    } while (true);
}

int ws_consume_line(ws_cliant_con* cli_con) {
    int i;
    char* buf;
    int len;

    while (true) {
        if ((len = ws_t_peak(cli_con, &buf)) < 0) {
            return len; // error
        }

        for (i = 0; i < len; i++) {
            if (buf[i] == '\r') {
                ws_consume(cli_con, i); // consume everything before '\r'
                return ERR_OK;
            }
        }
        ws_consume(cli_con, len);
    }
}

int ws_confirm_tag(ws_cliant_con* cli_con, char* tag) {
    int i;
    char* buf;
    size_t len;
    size_t tlen = 0;

    int ret;

    while(true) {
        len = ws_t_peak(cli_con, &buf);

        for (i = 0; i < len; i++) {
            if (tag[i] == '\0') {
                // Reached the end of tag.
                // We also need to make sure that we consume the entire line
                // even if it did not match the tag.

                if (buf[i] == '\r') {
                    ws_consume(cli_con, i);
                    return true; // They are equal
                } else {
                    ws_consume(cli_con, i + 1);
                    ws_consume_line(cli_con);
                    return false;
                }
            }
            if (buf[i] != tag[i]) {
                ws_consume(cli_con, i);
                ws_consume_line(cli_con);
                return false;
            }
        }

        tag += len; // advance the tag pointer
        ws_consume(cli_con, len);
    }
}

size_t do_ws_header(ws_cliant_con* cli_con) {
    int ret;

    bl_str_selecter tag_finder;

    bool websocket_upgrade = false;
    bool websocket_gotKey = false;
    char wsKey[WS_KEY_LEN + sizeof(ws_uuid)];

    // Read the header
    ws_consume_line(cli_con); // Throw away the first line, hehe
    ws_eat_whitespace(cli_con);
    while (true) { // break when we hit a double end line? (\r\n\r\n)

        int i;
        char* buffer;
        int len;
        int selected;

        bl_str_reset(&tag_finder, WS_H_FIELDS, WS_H_FIELDS_LEN);

        do {
            len = ws_t_peak(cli_con, &buffer); // *grab*

            // *inspect*
            for (i = 0; i < len && buffer[i] != ':'; i++) {
                if (buffer[i] == '\n') {
                    // END OF HEADER DETECTED!!!!
                    ws_consume(cli_con, i + 1);
                    goto header_done;
                }
            }
            selected = bl_str_select(&tag_finder, buffer, i);

            ws_consume(cli_con, i); // *munch!*

        } while(i == len); // If i == len, we have read part of the string, but have not hit the ':' yet

        if (ws_eat_whitespace(cli_con) == 1) { // eat the ": "
            DEBUG_printf("Unexpected line end.\n");
        }

        switch (selected) {
            case WS_H_FIELD_UPGRADE:

                if (ws_confirm_tag(cli_con, "websocket")) {
                    websocket_upgrade = true;
                } else {
                    DEBUG_printf("Error, thats not websocket.\n");
                }
                break;

            case WS_H_FIELD_KEY:

                if ((ret = ws_t_read(cli_con, wsKey, WS_KEY_LEN)) < 0) {
                    return ret;
                }
                break;

            case BL_STR_NO_MATCH:
            case BL_STR_NO_MATCH_YET:
                break;
        }

        while (!ws_eat_whitespace(cli_con)) { // eat "\r\n"
            // This will happen if we did not fully process a known key
            // or if we have an unknown key that we need to just skip.
            DEBUG_printf("Part of value unconsumed.\n");
            ws_consume_line(cli_con);
        }
    }
    header_done:

    if (!websocket_upgrade) {
        printf("Normal HTTP request recieved.\n");

        // TODO: For now we just assume the header is a valid HTTP 1.1 GET request.

        // Write the first part of the responce
        ws_t_write(cli_con, ws_page_responce1, sizeof(ws_page_responce1) - 1, TCP_WRITE_FLAG_MORE);

        char contentLenBuf[12];
        sprintf(contentLenBuf, "%i", sizeof(index_html));

        ws_t_write(cli_con, contentLenBuf, strlen(contentLenBuf), TCP_WRITE_FLAG_MORE);
        ws_t_write(cli_con, ws_page_responce2, sizeof(ws_page_responce2) - 1, TCP_WRITE_FLAG_MORE);
        ws_t_write(cli_con, index_html, sizeof(index_html), 0);

        // Flush the output? I am not really sure if this is needed or even wanted.
        //tcp_output(cli_con->printed_circuit_board);

        DEBUG_printf("Header complete.\n");
        ws_t_write_barrier(cli_con);

        DEBUG_printf("Header sent.\n");

        return IOL_YIELD_REASON_END;
    }

    // Write the first part of the responce
    ws_t_write(cli_con, ws_responce1, sizeof(ws_responce1) - 1, TCP_WRITE_FLAG_MORE);

    // Write the Accept key
    char hashBuf[20];
    char baseBuf[28];
    memcpy(wsKey + WS_KEY_LEN, ws_uuid, sizeof(ws_uuid));
    mbedtls_sha1_ret(wsKey, WS_KEY_LEN + (sizeof(ws_uuid) - 1), hashBuf);
    encode_base64(baseBuf, hashBuf, 20);

    ws_t_write(cli_con, baseBuf, sizeof(baseBuf), TCP_WRITE_FLAG_MORE);

    // Write the first last of the responce
    ws_t_write(cli_con, ws_responce2, sizeof(ws_responce2) - 1, 0);

    // Flush the output? I am not really sure if this is needed or even wanted.
    //tcp_output(cli_con->printed_circuit_board);

    DEBUG_printf("Header complete.\n");
    ws_t_write_barrier(cli_con);

    DEBUG_printf("Header sent.\n");

    ws_framinator framinator;
    websocket_initialize_framinator(&framinator, cli_con);

    //char cool_message[] = "The PI Pico now has WebSockets!\n";
    //websocket_write(&framinator, cool_message, sizeof(cool_message) - 1); // subtract the null char

    //websocket_write(&framinator, cool_message, sizeof(cool_message) - 1); // subtract the null char
    //websocket_flush(&framinator);

    while (true) {
        char command; // 0 = off, 1 = on, 2 = toggle

        if ((ret = websocket_read(&framinator, &command, 1))) {
            return ret;
        }

        if (command == '1') {
            gpio_put(11, 1); // on
        } else if (command == '0') {
            gpio_put(11, 0); // off
        } else if (command == '2') {
            gpio_put(11, !gpio_get(11)); // toggle
        } else if (command == 'b') {
            char number_str[10];
            sprintf(number_str, "%d", adc_read());
            websocket_write(&framinator, number_str, strlen(number_str));
            websocket_flush(&framinator);
        }

        printf("%c", command);

        // o god. I wanted to write messages in a loop, but we need a threaded sleep!
        //sleep_ms(1000);
    }

    return IOL_YIELD_REASON_END; // TODO: Do more stuff with this task? Will a new task be started?
}

/**
 * @brief Closes the PCB if it is not already closed
 */
err_t ws_cli_con_close_pcb(ws_cliant_con* cli_con) {
    err_t err = ERR_OK;
    if (cli_con->printed_circuit_board != NULL) {
        // TODO: make it so that the task handles this.

        tcp_arg(cli_con->printed_circuit_board, NULL);
        tcp_poll(cli_con->printed_circuit_board, NULL, 0);
        tcp_sent(cli_con->printed_circuit_board, NULL);
        tcp_recv(cli_con->printed_circuit_board, NULL);
        tcp_err(cli_con->printed_circuit_board, NULL);
        err = tcp_close(cli_con->printed_circuit_board);
        if (err != ERR_OK) {
            DEBUG_printf("close failed %d, calling abort\n", err);
            tcp_abort(cli_con->printed_circuit_board);
            err = ERR_ABRT;
        }
        cli_con->printed_circuit_board = NULL;
    }
    return err;
}

err_t ws_cli_con_close(ws_cliant_con* cli_con, err_t status) {

    if (status == 0) {
        DEBUG_printf("Connection closed\n");
    } else {
        DEBUG_printf("Connection closed with error: %d\n", status);
    }

    return ws_cli_con_close_pcb(cli_con);
}

size_t do_cli_con_task(sub_task* task, void* args) {
    ws_cliant_con* cli_con = (ws_cliant_con*) args;

    return ws_cli_con_close(cli_con, do_ws_header(cli_con));
}

// ============= BETTER, BUT STILL KINDA BAD! =============
// TODO: REFACTOR!

static err_t tcp_cli_con_sent(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;

    //printf("tcp_cli_con_sent %u\n", len);

    // Do we need the len arg? Maybe not!
    // Here is what I found:
    // This sent/acked callback is called from tcp_input() soon after
    // tcp_input() -> tcp_process() -> tcp_receive() updates PCB's snd_buf
    // This means that len should be (pcb->snd_buf - old_snd_buf) as long as
    // the value fits into 16 bits*. More useful would be (TCP_SND_BUF - pcb->snd_buf)
    // to find the absolute number of bytes waiting to be ack'ed and freeing buffers that
    // fall outside of that.
    //
    // *The sent callback will be called multiple times if the value cant fit.
    // See ugly special case code in tcp_in.c#tcp_input(); don't worry,
    // that can't happen if LWIP_WND_SCALE is disabled!
    #if LWIP_WND_SCALE
    #error "You better check this before enabling LWIP_WND_SCALE!"
    #endif

    // call all the ack_callback's
    if (cli_con->ack_callback.call) {
        cli_con->ack_callback.call(cli_con->ack_callback.arg, len);
    }

    cli_con->notify_ack = true;

    // TODO: FIXME: Handling multiple like this may cause a problem when the task
    // waits for the first reason, and then waits for the second reason. Thus
    // hitting one after the other right here in the same ACK!
    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_WAIT_FOR_ACK, ERR_OK);
    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_FLUSH, ERR_OK);

    return ERR_OK;
}

static void tcp_cli_con_err(void *arg, err_t err) {
    DEBUG_printf("tcp_client_err_fn %d\n", err);

    ws_cliant_con* cli_con = (ws_cliant_con*)arg;

    // The PCB is already freed according to the tcp_err() spec.
    cli_con->printed_circuit_board = NULL;

    // TODO: FIXME: See reason why this is bad in ACK handler.
    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_READ, err);
    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_WAIT_FOR_ACK, err);
    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_FLUSH, err);
}

err_t tcp_cli_con_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (!p) {
        // cliant closed the connection
        ws_cli_con_close_pcb(cli_con);
        tcp_cli_con_err(arg, ERR_CLSD);
        return ERR_OK;
    }
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    if (p->tot_len == 0) {
        pbuf_free(p);
        return ERR_OK;
    }

    //DEBUG_printf("tcp_cli_con_recv %d err %d\n", p->tot_len, err);

    // We might have some un-processed pbufs if the subtask yielded for some other reason, stack the new ones on top.
    if (cli_con->p_current) {
        pbuf_cat(cli_con->p_current, p);
    } else {
        cli_con->p_current = p;
    }

    iol_notify(&cli_con->io_task, WS_T_YIELD_REASON_READ, ERR_OK);

    // pbufs get freed as we used them. No need to free them here.

    return ERR_OK;
}

static err_t tcp_cli_con_poll(void *arg, struct tcp_pcb *tpcb) {
    // DEBUG_printf("tcp_cli_con_poll_fn\n");
    // Basically a 10 second timeout for now.

    // TODO: implement connection timeouts.
    // return tcp_cli_con_result(arg, -1);
    DEBUG_printf("-");

    return ERR_OK;
}


// ================ CLIANT CONNECTION ACCEPTER ================

int reset_cli_con_for_new_client(ws_cliant_con* cli_con) {
    // TODO: DELETE THIS FUNCTION! When we support multiple clients, new objects will be allocated.
    // For now, just zero out the client connection specific parts of our mega struct while
    // not touching the server stuff.

    if (cli_con->task != NULL && !sub_task_reset(cli_con->task)) {
        // There is likely already an active connection
        DEBUG_printf("Failed to reset the task.\n");
        return ERR_INPROGRESS;
    }

    cli_con->ack_callback.call = NULL;
    // cli_con->io_task handled cleanly by iol_task_run
    cli_con->notify_ack = 0;
    cli_con->p_current = NULL;
    cli_con->printed_circuit_board = NULL;
    cli_con->recved_current = 0;
    cli_con->task = NULL;

    return ERR_OK;
}

// goes in ---> tcp_accept()
// A new client connection is accepted.
static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (err != ERR_OK || client_pcb == NULL || (err = reset_cli_con_for_new_client(cli_con))) {
        DEBUG_printf("Failure in accept: %i\n", err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    cli_con->printed_circuit_board = client_pcb;
    cli_con->task = task_ws_header; // TODO: make the task more local to the connection

    tcp_arg(client_pcb, cli_con);
    tcp_sent(client_pcb, tcp_cli_con_sent);
    tcp_recv(client_pcb, tcp_cli_con_recv);
    // tcp_cli_con_poll seems to only be called when no traffic is flowing. Maybe one or two blips at other times.
    tcp_poll(client_pcb, tcp_cli_con_poll, 2); // 1 second polling (2 "TCP coarse grained timer shots")
    tcp_err(client_pcb, tcp_cli_con_err);

    // It will just run until it needs bytes and wait
    size_t ret = iol_task_run(&cli_con->io_task, ws_check_reason, cli_con, cli_con->task, do_cli_con_task, cli_con);

    //return tcp_server_send_data(arg, cli_con->printed_circuit_board);
    return ret;
}

// ================ MAIN FUNCTIONS ================

static bool tcp_server_open(ws_cliant_con* cli_con) {
    DEBUG_printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, NULL, TCP_PORT);
    if (err) {
        DEBUG_printf("failed to bind to port %u\n", TCP_PORT);
        return false;
    }

    cli_con->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!cli_con->server_pcb) {
        DEBUG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(cli_con->server_pcb, cli_con);
    tcp_accept(cli_con->server_pcb, tcp_server_accept);

    return true;
}

void run_tcp_server_test() {
    // TODO: use a server struct and allocate this later?
    ws_cliant_con* cli_con = calloc(1, sizeof(ws_cliant_con));
    if (!cli_con) {
        return;
    }

    if (!tcp_server_open(cli_con)) {
        DEBUG_printf("Server failed to open :(\n");
        return;
    }
    // TODO: deallocate ws_cliant_con later

    //TODO: move server deconstruction to its own function/struct
    /*if (cli_con->server_pcb) {
        tcp_arg(cli_con->server_pcb, NULL);
        tcp_close(cli_con->server_pcb);
        cli_con->server_pcb = NULL;
    }*/
}

int main() {
    //const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    //gpio_init(LED_PIN);
    //gpio_set_dir(LED_PIN, GPIO_OUT);
    stdio_init_all();

    // init digital output on GP11
    gpio_init(11);
    gpio_set_dir(11, true); // output

    // init analog input on ADC0
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);

    SUB_TASK_GLOBAL_INIT(task_ws_header)

    iol_init(); // ugly global init thingy

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA)) {
        printf("Wi-Fi init failed\n");
        return -1;
    } else {
        printf("Wi-Fi init success\n");
    }

    cyw43_state.trace_flags |= CYW43_TRACE_ASYNC_EV;
    cyw43_arch_enable_sta_mode();

    if (cyw43_arch_wifi_connect_async(
        wifi_ssid,
        wifi_password,
        CYW43_AUTH_WPA2_MIXED_PSK)) {
            printf("cyw43_arch_wifi_connect_async failed\n");
            return -2;
    }
    int status_old = CYW43_LINK_DOWN;
    int status;
    do {
        //poll for now, TODO: use netif_set_status_callback();
        status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        uint32_t ms = to_ms_since_boot(get_absolute_time());

        int rate = 500;
        int on_time = rate / 2;
        switch (status) {
            case CYW43_LINK_DOWN:
                break;
            case CYW43_LINK_JOIN:
                rate = 300;
                on_time = rate / 2;
                break;
            case CYW43_LINK_NOIP:
                rate = 200;
                on_time = rate / 2;
                break;
            case CYW43_LINK_FAIL:
            case CYW43_LINK_NONET:
            case CYW43_LINK_BADAUTH:
                rate = 500;
                on_time = 50;

                if (cyw43_arch_wifi_connect_async(
                    wifi_ssid,
                    wifi_password,
                    CYW43_AUTH_WPA2_MIXED_PSK)) {
                        printf("cyw43_arch_wifi_connect_async failed\n");
                        return -2;
                }

                break;
        }

        if (ms % rate == 0) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        } else if (ms % on_time == 0) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        }

        if (status != status_old) {
            printf("Link status changed to: %s\n", cyw43_tcpip_link_status_name(status));
        }
        status_old = status;
        //cyw43_arch_poll();
    } while (status != 3);

    printf("Connected to wifi with IP: %s\n", ip4addr_ntoa(netif_ip4_addr(cyw43_state.netif)));

    // Start our test server. Interrupts or calls to cyw43_arch_poll/cyw43_arch_wait_for_work_until
    // should be all it need to keep it alive.
    run_tcp_server_test();

    while (true) {

        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

#if PICO_CYW43_ARCH_POLL
        // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
        // main loop (not from a timer) to check for Wi-Fi driver or lwIP work that needs to be done.
        cyw43_arch_poll();
        // you can poll as often as you like, however if you have nothing else to do you can
        // choose to sleep until either a specified time, or cyw43_arch_poll() has work to do:
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(100));
#else
        // if you are not using pico_cyw43_arch_poll, then WiFI driver and lwIP work
        // is done via interrupt in the background. This sleep is just an example of some (blocking)
        // work you might be doing.
        sleep_ms(100);
#endif


        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

#if PICO_CYW43_ARCH_POLL
        // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
        // main loop (not from a timer) to check for Wi-Fi driver or lwIP work that needs to be done.
        cyw43_arch_poll();
        // you can poll as often as you like, however if you have nothing else to do you can
        // choose to sleep until either a specified time, or cyw43_arch_poll() has work to do:
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(100));
#else
        // if you are not using pico_cyw43_arch_poll, then WiFI driver and lwIP work
        // is done via interrupt in the background. This sleep is just an example of some (blocking)
        // work you might be doing.
        sleep_ms(100);
#endif

        // TODO: What happens if the wifi link goes down? will the server/connections error out?
        // should we check and re-initialize it?
    }

    cyw43_arch_deinit();
}

