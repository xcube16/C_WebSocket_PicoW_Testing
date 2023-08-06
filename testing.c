
#include "pico/stdlib.h"

#include "lwipopts.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "bufferless_str.h"

#include "mbedtls/sha1.h"

// cyw43_arch.c does not expose this helper function (pure), but we want it.
static const cha | base64r* cyw43_tcpip_link_status_name(int status) {
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

const char wifi_ssid[] = "placeholder";
const char wifi_password[] = "placeholder";

// ================ CLIANT CONNECTION ================

#define WS_STATE_HEADER 1
#define WS_STATE_HEADER_KEY 2

typedef struct ws_state_ {
    int state;
} ws_state;

typedef struct ws_cliant_con_ {
    struct tcp_pcb* printed_circuit_board; // I honistly have no idea

    // fill this buffer with the requested processable size
    char* buf;
    size_t buf_size;
    size_t buf_total_size;

    struct pbuf* p_current;
    size_t recved_current;

    ws_state* state;
    // TODO: abstract data expecter thingy with function pointers and custom state in it
    bl_str_selecter tag_finder;

    ws_state* state


} ws_cliant_con;

typedef err_t (*tcp_recv_fn)(ws_client_state *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err);



typedef struct ws_state_header_key_ {
    ws_state state;
    char* ws_key;
    size_t ws_key_len;
} ws_state_header_key;


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
 * @param buf_ptr Pointer to a buffer. Will be set of the number of bytes is > 0
 * @return size_t Number of bytes available
 */
size_t ws_peak(ws_cliant_con* cli_con, char** buf_ptr) {
    if (cli_con->buf_size != 0) {
        *buf_ptr = cli_con->buf;
        return cli_con->buf_size;
    }
    if (!cli_con->p_current) {
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

static err_t tcp_server_sent(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    ws_cliant_con_t* state = (ws_cliant_con_t*)arg;

    printf("tcp_server_sent %u\n", len);
    state->sent_len += len;

    if (state->sent_len >= BUF_SIZE) {

        // We should get the data back from the client
        state->recv_len = 0;
        DEBUG_printf("Waiting for buffer from client\n");
    }

    return ERR_OK;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    ws_cliant_con* cli_con = (ws_cliant_con*)arg;
    if (!p) {
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

    DEBUG_printf("tcp_server_recv %d/%d err %d\n", p->tot_len, state->recv_len, err);

    cli_con->p_current = p;

    // READ STUFF until we run out of bytes
    switch (cli_con->state) {

    }

    // pbufs got freed as we used them
    tcp_recved(tpcb, cli_con->recved_current);
    cli_con->recved_current = 0;

    // Receive the buffer
    const uint16_t buffer_left = BUF_SIZE - state->recv_len;
    state->recv_len += pbuf_copy_partial(p, state->buffer_recv + state->recv_len,
                                             p->tot_len > buffer_left ? buffer_left : p->tot_len, 0);

    // \/ SCRAP CODE \/

    // Have we have received the whole buffer
    if (state->recv_len == BUF_SIZE) {

        // check it matches
        if (memcmp(state->buffer_sent, state->buffer_recv, BUF_SIZE) != 0) {
            DEBUG_printf("buffer mismatch\n");
            return tcp_server_result(arg, -1);
        }
        DEBUG_printf("tcp_server_recv buffer ok\n");

        // Test complete?
        state->run_count++;
        if (state->run_count >= TEST_ITERATIONS) {
            tcp_server_result(arg, 0);
            return ERR_OK;
        }

        // Send another buffer
        return tcp_server_send_data(arg, state->client_pcb);
    }
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb) {
    DEBUG_printf("tcp_server_poll_fn\n");
    return tcp_server_result(arg, -1); // no response is an error?
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
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("Failure in accept\n");
        tcp_server_result(arg, err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);

    return tcp_server_send_data(arg, state->client_pcb);
}


int main() {
    //const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    //gpio_init(LED_PIN);
    //gpio_set_dir(LED_PIN, GPIO_OUT);
    stdio_init_all();

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
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
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
        cyw43_arch_poll();
    } while (status != CYW43_LINK_UP);

    printf("Connected\n");

    //socket();

    //httpd_init();
    
    while (true) {

        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        printf("1");

        sleep_ms(250);

        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        printf("0");

        sleep_ms(250);

        if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_JOIN) {
            sleep_ms(1000);
        }


    }
}

