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

#ifndef PAL_RESILIENT_SERVER_H
#define PAL_RESILIENT_SERVER_H

#include "pal_network.h"


/**
 * @brief Accept connections resiliently, recreating the listening socket if it fails.
 *
 * @param listen_fd    Pointer to the listening socket descriptor.
 * @param listen_addr  Socket address the server is bound/should be bound to.
 * @param client_addr  Output client socket address structure.
 * @param addr_len     Input/Output length of client_addr.
 * @param running_flag Pointer to the atomic running flag of the server.
 *
 * @return int The accepted client socket descriptor, or -1 on shutdown.
 */
int pal_resilient_accept(int *listen_fd,
                         const struct sockaddr_in *listen_addr,
                         struct sockaddr *client_addr,
                         socklen_t *addr_len,
                         atomic_int *running_flag);

#endif /* PAL_RESILIENT_SERVER_H */
