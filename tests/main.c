/* Simple test harness */
#include "event_loop.h"
#include "http_server.h"
#include <stdio.h>
#include <signal.h>

static event_loop_t *g_loop = NULL;

void sighandler(int sig) {
    (void)sig;
    if (g_loop) event_loop_stop(g_loop);
}

int main(void) {
    g_loop = event_loop_create();
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    
    http_server_t *http = http_server_create(g_loop, 8080);
    if (!http) {
        fprintf(stderr, "Failed to create HTTP server\n");
        event_loop_destroy(g_loop);
        return 1;
    }
    
    printf("zhttpd test server running on port 8080\n");
    printf("Open: http://localhost:8080\n");
    printf("Press Ctrl+C to stop\n\n");
    
    signal(SIGINT, sighandler);
    
    event_loop_run(g_loop);
    
    http_server_destroy(http);
    event_loop_destroy(g_loop);
    
    printf("Server stopped\n");
    return 0;
}
