#pragma once
#include <network.h>
#include <errno.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * connectWithTimeout — TCP connect that gives up after timeoutS seconds.
 *
 * On a blocking socket, IOS waits for its own TCP timeout (over a minute
 * for an unreachable address), freezing whoever called it.  The socket is
 * made non-blocking and connect() polled until IOS reports it connected
 * (EISCONN), the usual Wii pattern; then it is made blocking again.
 * Returns 0 or a negative errno.  quit: optional abort flag.
 * ----------------------------------------------------------------------- */
static inline int connectWithTimeout(s32 sock, struct sockaddr_in* addr, int timeoutS,
                                     volatile int* quit = nullptr)
{
    u32 nb = 1;
    net_ioctl(sock, FIONBIO, &nb);
    int r = -ETIMEDOUT;
    for (int waited = 0; waited < timeoutS * 1000; waited += 50) {
        if (quit && *quit) { r = -ECANCELED; break; }
        int c = net_connect(sock, (struct sockaddr*)addr, sizeof(*addr));
        if (c == 0 || c == -EISCONN) { r = 0; break; }
        if (c != -EINPROGRESS && c != -EALREADY && c != -EAGAIN) { r = c; break; }
        usleep(50 * 1000);
    }
    nb = 0;
    net_ioctl(sock, FIONBIO, &nb);
    return r;
}
