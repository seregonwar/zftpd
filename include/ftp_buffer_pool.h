#ifndef FTP_BUFFER_POOL_H
#define FTP_BUFFER_POOL_H

#include <stddef.h>

void *ftp_buffer_acquire(void);
void ftp_buffer_release(void *buffer);
size_t ftp_buffer_size(void);

#endif
