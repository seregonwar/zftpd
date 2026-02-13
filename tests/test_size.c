#include "ftp_types.h"
#include <stdio.h>

int main(void) {
    printf("sizeof(ftp_session_t) = %zu bytes\n", sizeof(ftp_session_t));
    printf("sizeof(ftp_server_context_t) = %zu bytes\n", sizeof(ftp_server_context_t));
    return 0;
}
