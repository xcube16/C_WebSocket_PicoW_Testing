
#include "pico/stdlib.h"

#include "lwipopts.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "bufferless_str.h"
#include "sub_task.h"

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

// ================ CLIANT CONNECTION ================

#define WS_T_YIELD_REASON_END 0
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

    // fill this buffer with the requested processable size
    // TODO: Now that we save pbufs for later and are not forced to process all of them in the tcp_recv callback,
    //       Maybe we should refactor/remove this simple buffer.
    char* buf;
    size_t buf_size;
    size_t buf_total_size;

    // Holds NULL or points to a chain pbufs accumulated by our tcp_recv callback
    struct pbuf* p_current;
    // Count the data we have processed so we can call tcp_receved in one shot, usually after a yield.
    size_t recved_current;

    // Basically a thread that is handling a single connection.
    // TODO: Multiple connections?
    sub_task* task;
    int task_yield_reason;

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
 * @param size 
 * @return char* 
 */
char* ws_read(ws_cliant_con* cli_con, size_t size) {
    if (!cli_con->p_current) {
        return NULL; // We are still waiting for more data
    }

    if (cli_con->buf_size > size) {
        printf("ERROR: Inconsistant read request! previous ask: %d, current ask: %d\n", cli_con->buf_size, size);
    } else if (cli_con->buf_size == 0) {
        // See if the first pbuf has enough bytes in it.
        // That way we don't even need to use our own buffer.
        if (cli_con->p_current->len >= size) {
            char* payload = cli_con->p_current->payload;
            cli_con->p_current = pbuf_free_header(cli_con->p_current, size);
            cli_con->recved_current += size;
            return payload;
            #warning "USE AFTER FREE!!! WARNING! WARNING! WARNING!" // TODO: Fix this ASAP!
        }
    }

    // ensure that enough space is allocated in the buffer
    if (cli_con->buf_total_size < size) {
        char* new_buf = realloc(cli_con->buf, size); // TODO: maybe allocate more than just the min amount?
        if (new_buf) {
            cli_con->buf_total_size = size;
            cli_con->buf = new_buf;
        } else {
            printf("ERROR: Failed to allocate %d bytes\n", size);
            return NULL;
        }
    }

    // fill the buffer until it reaches size or we run out of pbufs in the chain
    u16_t bytes_read = pbuf_copy_partial(
        cli_con->p_current,
        cli_con->buf + cli_con->buf_size,
        size - cli_con->buf_size,
        0);
    cli_con->p_current = pbuf_free_header(cli_con->p_current, bytes_read);

    cli_con->buf_size       += bytes_read;
    cli_con->recved_current += bytes_read;

    if (cli_con->buf_size == size) {
        cli_con->buf_size = 0;
        return cli_con->buf;
    }
    return NULL; // We are still waiting for more data
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
    if (cli_con->buf_size != 0) {
        *buf_ptr = cli_con->buf;
        return cli_con->buf_size;
    }
    if (!cli_con->p_current) {
        *buf_ptr = NULL;
        return 0; // We are still waiting for more data
    }
    *buf_ptr = cli_con->p_current->payload;
    return cli_con->p_current->len;
}

int ws_consume(ws_cliant_con* cli_con, size_t size) {
    if (cli_con->buf_size > 0) {
        // Not really sure why one would fail to read in the last call and
        // then use ws_peak and ws_consume, but as long as the full buffer is consumed
        // we can let this minor inconsistancy slide.
        // TODO: Maybe add a warning?
        if (cli_con->buf_size != size) {
            printf("ERROR: Inconsistant consume request! previous ask: %d, current ask: %d\n", cli_con->buf_size, size);
            return ERR_ARG;
        }
        cli_con->buf_size       -= size;
        cli_con->recved_current += size;

        return ERR_OK;
    }
    
    if (!cli_con->p_current) {
        if (size == 0) {
            return ERR_OK; // consume 0 bytes? well ok.
        }
        printf("ERROR: Can't consume bytes that we dont have yet! %d\n", size);
        return ERR_ARG;
    }

    cli_con->p_current = pbuf_free_header(cli_con->p_current, size);
    cli_con->recved_current += size;
    return ERR_OK;
}

// end normal helper functions

// threaded helper functions

/**
 * @brief Returns a condiguious byte array of the given size by yielding when more is needed.
 * 
 * @param cli_con 
 * @param size 
 * @return char* 
 */
char* ws_t_read(ws_cliant_con* cli_con, size_t size) {
    char* ret;
    while (!(ret = ws_read(cli_con, size))) {
        // TODO: Propogate errors returned by sub_task_yield
        sub_task_yield(WS_T_YIELD_REASON_READ, cli_con->task);
    }
    return ret;
}

/**
 * @brief Provides access to the next raw pbuf payload. Call ws_consume() or ws_read() to
 * actually advance the communication. Threaded, yielding.
 * 
 * @param cli_con 
 * @param buf_ptr Pointer to a buffer. Will be set if the number of bytes is > 0
 * @return size_t Number of bytes available
 */
size_t ws_t_peak(ws_cliant_con* cli_con, char** buf_ptr) {
    size_t ret;
    while (!(ret = ws_peak(cli_con, buf_ptr))) {
        // TODO: Propogate errors returned by sub_task_yield
        sub_task_yield(WS_T_YIELD_REASON_READ, cli_con->task);
    }
    return ret;
}

err_t ws_t_write(ws_cliant_con* cli_con, void* dataptr, size_t len, u8_t apiflags/*, tcpwnd_size_t* countdown*/) {
    err_t ret;

    // Wait until we have at least *some* space on the send buffer
    while (tcp_sndbuf(cli_con->printed_circuit_board) == 0) {
        // TODO: The yield reason should not be a full flush. Just wait for tcp_sent!
        tcp_output(cli_con->printed_circuit_board);
        if (ret = (err_t) sub_task_yield(WS_T_YIELD_REASON_FLUSH, cli_con->task)) {
            return ret;
        }
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
        // TODO: The yield reason should not be a full flush. Just wait for tcp_sent!
        tcp_output(cli_con->printed_circuit_board);
        if (ret = (err_t) sub_task_yield(WS_T_YIELD_REASON_FLUSH, cli_con->task)) {
            return ret;
        }
    }
    tcp_write(cli_con->printed_circuit_board, dataptr, len, apiflags);

    //*countdown = TCP_SND_BUF - tcp_sndbuf(cli_con->printed_circuit_board);
    return ERR_OK;
}

void ws_t_write_barrier(ws_cliant_con* cli_con) {

    // TODO: This is basically just a flush-the-entire-TCP function.
    //       Not very good, but it works. We will need a better system based
    //       on markers (aquired when writing) via our future threaded tcp_write
    //       function. Should these be absolute markers? Or just a countdown
    //       to its-safe-to-free-the-buffer? I'm leaning in the direction
    //       of countdown if we can make the tcp_sent callback not suck.

    while (cli_con->printed_circuit_board->snd_queuelen) {
        // TODO: Propogate errors returned by sub_task_yield
        sub_task_yield(WS_T_YIELD_REASON_FLUSH, cli_con->task);
    }
}

/**
 * @brief Called when an event has happened that might be able to wake up a yielded task
 * 
 * @param cli_con 
 */
void ws_t_wake(ws_cliant_con* cli_con) {

    // We could have other reasons to resume pile up while we wait for the one we want.
    // This is unlikely as the task should be smart enough *not* to yield if it does not
    // need to. It does not hurt, although we might not even need this helper function.
    while (true) {
        switch (cli_con->task_yield_reason) {
            case WS_T_YIELD_REASON_READ:
                if (!cli_con->p_current) {
                    return; // Nothing to read yet.
                }
                // The task is yielding for more data, we have some now, so lets continue.
                cli_con->task_yield_reason = sub_task_continue(cli_con->task, ERR_OK);
                
                // Inform the TCP stack of how many bytes we processed. Could be zero
                // (if the task yields to peek then yields to flush for example).
                tcp_recved(cli_con->printed_circuit_board, cli_con->recved_current);
                cli_con->recved_current = 0;
                break;
            
            case WS_T_YIELD_REASON_FLUSH:
                // Checking that tcp_sndbuf is at it's max would also work.
                if (cli_con->printed_circuit_board->snd_queuelen) {
                    return; // Not flushed yet.
                }

                cli_con->task_yield_reason = sub_task_continue(cli_con->task, ERR_OK);
                break;

            case WS_T_YIELD_REASON_WAIT_FOR_ACK:
                // TODO: Actually check that we got an ack! (maybe put this case in the ack handler?)
                // For now, just wake it anyway. The handler should just spin and yield again if its not right.
                cli_con->task_yield_reason = sub_task_continue(cli_con->task, ERR_OK);

            case WS_T_YIELD_REASON_END:
            default:
                DEBUG_printf("Done with WS task. TODO: Close the connection?\n");
                return;
        }
    }
}

// end threaded helper functions

// TODO: cleanup this crazy header
#include <websocket_framinator.h>

/**
 * @brief Eats ':', ' ', sneezes when it hits a '\n' (returns 1), and returns 0 for any other char.
 *
 * @param cli_con The connection handle
 * @return int 0 for non-whitespace, 1 for '\n'
 */
int ws_eat_whitespace(ws_cliant_con* cli_con) {
    char* buf;
    size_t size;
    do {
        size = ws_t_peak(cli_con, &buf);
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


void ws_consume_line(ws_cliant_con* cli_con) {
    int i;
    char* buf;
    size_t len;

    while(true) {
        len = ws_t_peak(cli_con, &buf);

        for (i = 0; i < len; i++) {
            if (buf[i] == '\r') {
                ws_consume(cli_con, i); // consume everything before '\r'
                return;
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

size_t do_ws_header(sub_task* task, void* args) {
    ws_cliant_con* cli_con = (ws_cliant_con*) args;
    
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
        
        if (ws_eat_whitespace(cli_con)) { // eat the ": "
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

                buffer = ws_t_read(cli_con, WS_KEY_LEN);
                memcpy(wsKey, buffer, WS_KEY_LEN);
                break;

            case BL_STR_NO_MATCH:
            case BL_STR_NO_MATCH_YET:
                break;
        }

        while (!ws_eat_whitespace(cli_con)) { // eat "\r\n"
            DEBUG_printf("Part of value unconsumed.\n");
            ws_consume_line(cli_con);
        }
    }
    header_done:

    if (!websocket_upgrade) {
        printf("Error: Thats not a websocket connection. TODO: handle this error\n");
    }

    // TODO: tcp_write errors instead of blocking when it's queue is full.
    //       WE HAVE A FIX FOR THIS!, use write ws_t_write!!!
    // Write the first part of the responce
    tcp_write(cli_con->printed_circuit_board, ws_responce1, sizeof(ws_responce1) - 1, TCP_WRITE_FLAG_MORE);
    
    // Write the Accept key
    char hashBuf[20];
    char baseBuf[28];
    memcpy(wsKey + WS_KEY_LEN, ws_uuid, sizeof(ws_uuid));
    mbedtls_sha1_ret(wsKey, WS_KEY_LEN + (sizeof(ws_uuid) - 1), hashBuf);
    encode_base64(baseBuf, hashBuf, 20);
    // TODO: tcp_write errors instead of blocking when it's queue is full.
    // Make it threaded. Check to see if tcp_sndbuf(li_con->printed_circuit_board);
    // is large enough.
    //       WE HAVE A FIX FOR THIS!, use write ws_t_write!!!
    tcp_write(cli_con->printed_circuit_board, baseBuf, sizeof(baseBuf), TCP_WRITE_FLAG_MORE);
    
    // Write the first last of the responce
    tcp_write(cli_con->printed_circuit_board, ws_responce2, sizeof(ws_responce2) - 1, 0);

    // Flush the output? I am not really sure if this is needed or even wanted.
    tcp_output(cli_con->printed_circuit_board);

    DEBUG_printf("Header complete.\n");
    ws_t_write_barrier(cli_con);

    DEBUG_printf("Header sent.\n");
    while (true) {
        printf("%c", *ws_t_read(cli_con, 1));
    }

    return WS_T_YIELD_REASON_END; // TODO: Do more stuff with this task? Will a new task be started?
}

// ============= BETTER, BUT STILL KINDA BAD! =============
// TODO: REFACTOR!

static err_t tcp_server_result(void *arg, int status) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (status == 0) {
        DEBUG_printf("test success\n");
    } else {
        DEBUG_printf("test failed %d\n", status);
    }
    
    err_t err = ERR_OK;
    if (cli_con->printed_circuit_board != NULL) {
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
    //TODO: move server deconstruction to its own function/struct
    if (cli_con->server_pcb) {
        tcp_arg(cli_con->server_pcb, NULL);
        tcp_close(cli_con->server_pcb);
        cli_con->server_pcb = NULL;
    }
    return err;
}

static err_t tcp_server_sent(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;

    printf("tcp_server_sent %u\n", len);

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
    
    ws_t_wake(cli_con);

    return ERR_OK;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (!p) {
        // cliant closed the connection
        return tcp_server_result(arg, -1);
    }
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    if (p->tot_len == 0) {
        pbuf_free(p);
        return ERR_OK;
    }

    DEBUG_printf("tcp_server_recv %d err %d\n", p->tot_len, err);

    // We might have some un-processed pbufs if the subtask yielded for some other reason, stack the new ones on top.
    if (cli_con->p_current) {
        pbuf_cat(cli_con->p_current, p);
    } else {
        cli_con->p_current = p;
    }

    ws_t_wake(cli_con);

    // pbufs get freed as we used them. No need to free them here.

    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb) {
    DEBUG_printf("tcp_server_poll_fn\n");
    // Basically a 10 second timeout for now.
    return tcp_server_result(arg, -1);
}

static void tcp_server_err(void *arg, err_t err) {
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_server_result(arg, err);
    }
}


// ================ CLIANT CONNECTION ACCEPTER ================

// goes in ---> tcp_accept()
static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("Failure in accept\n");
        tcp_server_result(arg, err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    cli_con->printed_circuit_board = client_pcb;
    cli_con->task = task_ws_header; // TODO: make the task more local to the connection

    tcp_arg(client_pcb, cli_con);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, 20); // 10 second timeout (20 "TCP coarse grained timer shots")
    tcp_err(client_pcb, tcp_server_err);

    // It will just run until it needs bytes and wait
    cli_con->task_yield_reason = sub_task_run(cli_con->task, do_ws_header, cli_con);

    //return tcp_server_send_data(arg, cli_con->printed_circuit_board);
    return ERR_OK;
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
        tcp_server_result(cli_con, -1);
        return;
    }
    // TODO: deallocate ws_cliant_con later
}

int main() {
    //const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    //gpio_init(LED_PIN);
    //gpio_set_dir(LED_PIN, GPIO_OUT);
    stdio_init_all();

    SUB_TASK_GLOBAL_INIT(task_ws_header)

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA)) {
        printf("Wi-Fi init failed\n");
        return -1;
    } else {
        printf("Wi-Fi init success\n");
    }

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
        int ms = to_ms_since_boot(get_absolute_time());
        
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


    //socket();

    //httpd_init();

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

