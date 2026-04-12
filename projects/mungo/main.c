#include "mungo.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t s_signal;
static void handle_signal(int signo) { s_signal = signo; }

static void handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
                      "mungo-server ready\nMethod: %.*s, URI: %.*s\n",
                      (int)hm->method.len, hm->method.buf,
                      (int)hm->uri.len, hm->uri.buf);
    }
}

int main(int argc, char **argv) {
    int port = 8000;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port < 1 || port > 65535) {
                fprintf(stderr, "Invalid port\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--port <1-65535>]\n", argv[0]);
            return 0;
        }
    }

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    mg_log_level = MG_LL_ERROR;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection *listener = mg_http_listen(&mgr, url, handler, NULL);
    if (!listener) {
        fprintf(stderr, "Failed to listen on %s\n", url);
        mg_mgr_free(&mgr);
        return 1;
    }

    printf("mungo-server listening on %s\n", url);
    while (!s_signal) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nShutting down (%d)\n", s_signal);
    mg_mgr_free(&mgr);
    return 0;
}
