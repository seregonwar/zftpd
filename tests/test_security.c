#include "ftp_session.h"
#include "ftp_commands.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_reply_code(int fd)
{
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1U, 0);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    if (n < 3) {
        return -1;
    }
    return (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return 2;
    }

    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    (void)inet_pton(AF_INET, "10.0.0.2", &client_addr.sin_addr);
    client_addr.sin_port = htons(12345);

    ftp_session_t session;
    ftp_error_t init_err = ftp_session_init(&session, sv[0], &client_addr, 1U, "/");
    if (init_err != FTP_OK) {
        return 3;
    }

    (void)ftp_session_process_command(&session, "PWD");
    int code = read_reply_code(sv[1]);
    if (code != 530) {
        return 4;
    }

    session.authenticated = 1U;
    ftp_error_t port_err = cmd_PORT(&session, "127,0,0,1,0,21");
    if (port_err != FTP_OK) {
        return 5;
    }
    code = read_reply_code(sv[1]);
    if (code != 501) {
        return 6;
    }

    session.authenticated = 0U;
    session.user_ok = 0U;
    session.auth_attempts = 0U;

    ftp_error_t u1 = cmd_USER(&session, "nope");
    if (u1 != FTP_OK) {
        return 7;
    }
    (void)read_reply_code(sv[1]);

    ftp_error_t u2 = cmd_USER(&session, "nope");
    if (u2 != FTP_OK) {
        return 8;
    }
    (void)read_reply_code(sv[1]);

    ftp_error_t u3 = cmd_USER(&session, "nope");
    if (u3 != FTP_ERR_AUTH_FAILED) {
        return 9;
    }
    code = read_reply_code(sv[1]);
    if (code != 530) {
        return 10;
    }

    ftp_session_cleanup(&session);
    close(sv[1]);
    return 0;
}
