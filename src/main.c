/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file main.c
 * @brief FTP server entry point (multi-platform)
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 * PLATFORMS: Linux, PS3, PS4, PS5
 * 
 */

#include "ftp_server.h"
#include "ftp_config.h"
#include "pal_notification.h"
#include "pal_network.h"
#include "pal_fileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <stdint.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#endif

/*===========================================================================*
 * GLOBAL SERVER CONTEXT
 *===========================================================================*/

static ftp_server_context_t g_server_ctx;
static volatile sig_atomic_t g_shutdown_requested = 0;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static pid_t find_pid_by_name(const char *name)
{
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    pid_t pid = -1;
    size_t buf_size = 0;
    uint8_t *buf = NULL;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) {
        return -1;
    }

    buf = (uint8_t *)malloc(buf_size);
    if (buf == NULL) {
        return -1;
    }

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
        free(buf);
        return -1;
    }

    for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
        int ki_structsize = 0;
        pid_t ki_pid = -1;
        memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
        memcpy(&ki_pid, &ptr[72], sizeof(ki_pid));
        char *ki_tdname = (char *)&ptr[447];

        if (ki_structsize <= 0) {
            break;
        }
        ptr += ki_structsize;
        if ((ki_pid != mypid) && (strcmp(name, ki_tdname) == 0)) {
            pid = ki_pid;
        }
    }

    free(buf);
    return pid;
}
#endif

/*===========================================================================*
 * SIGNAL HANDLERS
 *===========================================================================*/

#ifndef PLATFORM_PS4
/**
 * @brief Signal handler for graceful shutdown
 */
static void signal_handler(int sig)
{
    (void)sig; /* Unused */
    g_shutdown_requested = 1;
}

/**
 * @brief Install signal handlers
 */
static void install_signal_handlers(void)
{
    /* Standard POSIX signals */
    signal(SIGINT, signal_handler);   /* Ctrl+C */
    signal(SIGTERM, signal_handler);  /* kill */
    signal(SIGPIPE, SIG_IGN);         /* Broken pipe (ignore) */
}
#endif

/*===========================================================================*
 * PLATFORM-SPECIFIC CODE
 *===========================================================================*/

#ifdef PLATFORM_PS4

int main(void)
{
    printf("[PS4 FTP] Version " RELEASE_VERSION "\n");
    printf("[PS4 FTP] Initializing...\n");

    (void)pal_notification_init();

    (void)syscall(SYS_thr_set_name, -1, "zftpd.elf");
    signal(SIGPIPE, SIG_IGN);

    if (find_pid_by_name("zftpd.elf") > 0) {
        pal_notification_send("zftpd: payload alredy loaded");
        pal_notification_shutdown();
        return 0;
    }
    
    const char *bind_ip = "0.0.0.0";
    char display_ip[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(display_ip, sizeof(display_ip)) != FTP_OK) {
        (void)snprintf(display_ip, sizeof(display_ip), "%s", bind_ip);
    }

    const char *root_path = "/";
    
    /* Initialize server */
    ftp_error_t err = ftp_server_init(&g_server_ctx, bind_ip,
                                       FTP_DEFAULT_PORT, root_path);
    
    if (err != FTP_OK) {
        printf("[PS4 FTP] Initialization failed: %d\n", (int)err);
        if (err == FTP_ERR_SOCKET_BIND) {
            printf("[PS4 FTP] Bind failed: %s\n", strerror(errno));
            printf("[PS4 FTP] Hint: port %u busy or invalid bind_ip.\n",
                   (unsigned)FTP_DEFAULT_PORT);
        }
        return -1;
    }
    
    printf("[PS4 FTP] Listening on %s:%u\n", display_ip, FTP_DEFAULT_PORT);
    printf("[PS4 FTP] Root: %s\n", root_path);
    
    /* Start server */
    err = ftp_server_start(&g_server_ctx);
    
    if (err != FTP_OK) {
        printf("[PS4 FTP] Failed to start: %d\n", (int)err);
        ftp_server_cleanup(&g_server_ctx);
        return -1;
    }
    
    printf("[PS4 FTP] Server started successfully\n");

    {
        char notify_msg[128];
        (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd: %s:%u root=%s", display_ip,
                       (unsigned)FTP_DEFAULT_PORT, root_path);
        pal_notification_send(notify_msg);
    }
    
    /* Main loop */
    while (!g_shutdown_requested) {
        sleep(1);
    }
    
    /* Shutdown */
    printf("[PS4 FTP] Shutting down...\n");
    ftp_server_stop(&g_server_ctx);
    ftp_server_cleanup(&g_server_ctx);
    pal_notification_shutdown();
    
    printf("[PS4 FTP] Stopped\n");
    
    return 0;
}

#elif defined(PLATFORM_PS5)

#include <ps5/kernel.h>

/**
 * @brief Escalate privileges and spoof AuthID to bypass PPR
 */
static void ps5_jailbreak(void)
{
    pid_t pid = getpid();
    
    printf("[PS5] Escalating privileges...\n");
    
    /* 1. Root User/Group */
    if (kernel_set_ucred_uid(pid, 0) != 0) printf("[PS5] Warning: Failed to set UID=0\n");
    if (kernel_set_ucred_ruid(pid, 0) != 0) printf("[PS5] Warning: Failed to set RUID=0\n");
    if (kernel_set_ucred_svuid(pid, 0) != 0) printf("[PS5] Warning: Failed to set SVUID=0\n");
    if (kernel_set_ucred_rgid(pid, 0) != 0) printf("[PS5] Warning: Failed to set RGID=0\n");
    if (kernel_set_ucred_svgid(pid, 0) != 0) printf("[PS5] Warning: Failed to set SVGID=0\n");

    /* 2. AuthID Spoofing (System App) to bypass PPR checks (0x80410131) */
    /* 0x4801000000000013 = Needed for character devices (sflash0) and widespread access */
    uint64_t auth_id = 0x4801000000000013ULL;
    if (kernel_set_ucred_authid(pid, auth_id) != 0) {
        printf("[PS5] Error: Failed to set AuthID to 0x%llx\n", (unsigned long long)auth_id);
    } else {
        printf("[PS5] AuthID set to 0x%llx (sflash0/char-dev access)\n", (unsigned long long)auth_id);
    }

    /* 3. Capabilities (Allow all) */
    uint8_t caps[16];
    memset(caps, 0xFF, sizeof(caps));
    if (kernel_set_ucred_caps(pid, caps) != 0) {
        printf("[PS5] Error: Failed to set capabilities\n");
    }

    /* 4. Jailbreak (Break out of sandbox) */
    intptr_t rootvnode = kernel_get_root_vnode();
    if (rootvnode) {
        if (kernel_set_proc_rootdir(pid, rootvnode) != 0) printf("[PS5] Warning: Failed to set rootdir\n");
        if (kernel_set_proc_jaildir(pid, rootvnode) != 0) printf("[PS5] Warning: Failed to set jaildir\n");
        printf("[PS5] Sandbox escaped (rootdir/jaildir set to rootvnode)\n");
    } else {
        printf("[PS5] Error: Failed to get rootvnode\n");
    }
}

/**
 * @brief PlayStation 5 entry point
 */
int main(void)
{
    printf("[PS5 FTP] Version " RELEASE_VERSION "\n");
    printf("[PS5 FTP] Initializing...\n");

    (void)syscall(SYS_thr_set_name, -1, "zftpd.elf");
    signal(SIGPIPE, SIG_IGN);
    (void)pal_notification_init();

    if (find_pid_by_name("zftpd.elf") > 0) {
        pal_notification_send("zftpd: payload alredy loaded");
        pal_notification_shutdown();
        return 0;
    }
    
    /* Apply kernel patches/jailbreak immediately */
    ps5_jailbreak();
    
    install_signal_handlers();
    
    /* Get PS5 IP address (simplified) */
    char ip_address[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(ip_address, sizeof(ip_address)) != FTP_OK) {
        (void)snprintf(ip_address, sizeof(ip_address), "%s", "0.0.0.0");
    }
    
    ftp_error_t err = ftp_server_init(&g_server_ctx, ip_address,
                                       FTP_DEFAULT_PORT, "/");
    
    if (err != FTP_OK) {
        fprintf(stderr, "[PS5 FTP] Init failed: %d\n", (int)err);
        return EXIT_FAILURE;
    }
    
    printf("[PS5 FTP] Listening on %s:%u\n", ip_address, FTP_DEFAULT_PORT);
    
    err = ftp_server_start(&g_server_ctx);
    
    if (err != FTP_OK) {
        fprintf(stderr, "[PS5 FTP] Start failed: %d\n", (int)err);
        ftp_server_cleanup(&g_server_ctx);
        return EXIT_FAILURE;
    }
    
    printf("[PS5 FTP] Server running. Press Ctrl+C to stop.\n");

    {
        char notify_msg[128];
        (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd " RELEASE_VERSION " (PS5) started");
        pal_notification_send(notify_msg);

        (void)snprintf(notify_msg, sizeof(notify_msg), "FTP: %s:%u", ip_address,
                       (unsigned)FTP_DEFAULT_PORT);
        pal_notification_send(notify_msg);
    }
    
    /* Main loop */
    while (!g_shutdown_requested) {
        sleep(1);
        
        /* Display stats periodically */
        static int counter = 0;
        if ((counter++ % 60) == 0) { /* Every 60 seconds */
            uint32_t active = ftp_server_get_active_sessions(&g_server_ctx);
            if (active > 0U) {
                printf("[PS5 FTP] Active sessions: %u\n", active);
            }
        }
    }
    
    printf("\n[PS5 FTP] Shutting down...\n");
    ftp_server_stop(&g_server_ctx);
    ftp_server_cleanup(&g_server_ctx);
    pal_notification_shutdown();
    
    printf("[PS5 FTP] Goodbye!\n");
    
    return EXIT_SUCCESS;
}

#else /* POSIX / Linux */

/**
 * @brief Print usage information
 */
static void print_usage(const char *program)
{
    printf("Multi-Platform FTP Server v" RELEASE_VERSION "\n");
    printf("\n");
    printf("Usage: %s [OPTIONS]\n", program);
    printf("\n");
    printf("Options:\n");
    printf("  -p PORT       Listen port (default: %u)\n", FTP_DEFAULT_PORT);
    printf("  -d DIR        Root directory (default: current directory)\n");
    printf("  -h            Show this help message\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -p 2121 -d /home/ftp\n", program);
    printf("\n");
}

/**
 * @brief Linux/POSIX entry point
 */
int main(int argc, char **argv)
{
    uint16_t port = FTP_DEFAULT_PORT;
    char root_path[FTP_PATH_MAX];
    
    /* Get current directory as default */
    if (getcwd(root_path, sizeof(root_path)) == NULL) {
        fprintf(stderr, "Error: Cannot get current directory\n");
        return EXIT_FAILURE;
    }
    
    /* Parse command-line arguments */
    int opt;
    while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
        switch (opt) {
            case 'p':
                {
                    long port_arg = strtol(optarg, NULL, 10);
                    if ((port_arg <= 0) || (port_arg > 65535)) {
                        fprintf(stderr, "Error: Invalid port: %s\n", optarg);
                        return EXIT_FAILURE;
                    }
                    port = (uint16_t)port_arg;
                }
                break;
            
            case 'd':
                {
                    size_t len = strlen(optarg);
                    if (len >= sizeof(root_path)) {
                        fprintf(stderr, "Error: Path too long\n");
                        return EXIT_FAILURE;
                    }
                    memcpy(root_path, optarg, len + 1U);
                }
                break;
            
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }
    
    /* Install signal handlers */
    install_signal_handlers();
    
    printf("Multi-Platform FTP Server v" RELEASE_VERSION "\n");
    printf("=====================================\n");
    printf("Root directory: %s\n", root_path);
    printf("Listen port:    %u\n", port);
    printf("Max sessions:   %u\n", FTP_MAX_SESSIONS);
    printf("=====================================\n");

    (void)pal_notification_init();
    
    /* Initialize server */
    ftp_error_t err = ftp_server_init(&g_server_ctx, "0.0.0.0", port,
                                       root_path);
    
    if (err != FTP_OK) {
        fprintf(stderr, "Error: Server initialization failed: %d\n", (int)err);
        
        if (err == FTP_ERR_SOCKET_BIND) {
            fprintf(stderr, "Hint: Port %u may already be in use.\n", port);
            fprintf(stderr, "      Try a different port with -p option.\n");
        }
        
        return EXIT_FAILURE;
    }
    
    /* Start server */
    err = ftp_server_start(&g_server_ctx);
    
    if (err != FTP_OK) {
        fprintf(stderr, "Error: Failed to start server: %d\n", (int)err);
        ftp_server_cleanup(&g_server_ctx);
        return EXIT_FAILURE;
    }
    
    printf("\n");
    printf("Server started successfully!\n");
    printf("Listening on all interfaces, port %u\n", port);
    printf("Press Ctrl+C to stop.\n");
    printf("\n");

    {
        char notify_msg[128];
        (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd: listening on 0.0.0.0:%u",
                       (unsigned)port);
        pal_notification_send(notify_msg);
    }
    
    /* Main loop */
    uint64_t last_total_conn = 0U;
    
    while (!g_shutdown_requested) {
        sleep(5);
        
        /* Display periodic statistics */
        uint32_t active = ftp_server_get_active_sessions(&g_server_ctx);
        uint64_t total_conn = 0U;
        uint64_t bytes_sent = 0U;
        uint64_t bytes_recv = 0U;
        
        ftp_server_get_stats(&g_server_ctx, &total_conn, &bytes_sent,
                             &bytes_recv);
        
        /* Show status if there's activity */
        if ((active > 0U) || (total_conn != last_total_conn)) {
            printf("[Status] Active: %u | Total: %llu | "
                   "Sent: %llu bytes | Recv: %llu bytes\n",
                   active, (unsigned long long)total_conn,
                   (unsigned long long)bytes_sent,
                   (unsigned long long)bytes_recv);
            
            last_total_conn = total_conn;
        }
    }
    
    /* Graceful shutdown */
    printf("\n");
    printf("Shutdown requested. Closing active connections...\n");
    
    ftp_server_stop(&g_server_ctx);
    ftp_server_cleanup(&g_server_ctx);
    pal_notification_shutdown();
    
    printf("Server stopped.\n");
    
    return EXIT_SUCCESS;
}

#endif
