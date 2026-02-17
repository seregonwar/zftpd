#include "ftp_buffer_pool.h"
#include "ftp_config.h"
#include <stddef.h>
 
int main(void)
{
    void *bufs[FTP_MAX_SESSIONS];
 
    for (size_t i = 0U; i < (size_t)FTP_MAX_SESSIONS; i++) {
        bufs[i] = ftp_buffer_acquire();
        if (bufs[i] == NULL) {
            return 1;
        }
        for (size_t j = 0U; j < i; j++) {
            if (bufs[j] == bufs[i]) {
                return 2;
            }
        }
    }
 
    if (ftp_buffer_acquire() != NULL) {
        return 3;
    }
 
    for (size_t i = 0U; i < (size_t)FTP_MAX_SESSIONS; i++) {
        ftp_buffer_release(bufs[i]);
    }
 
    for (size_t i = 0U; i < (size_t)FTP_MAX_SESSIONS; i++) {
        bufs[i] = ftp_buffer_acquire();
        if (bufs[i] == NULL) {
            return 4;
        }
    }
 
    for (size_t i = 0U; i < (size_t)FTP_MAX_SESSIONS; i++) {
        ftp_buffer_release(bufs[i]);
    }
 
    return 0;
}
