#ifndef HAKU_POLL_H
#define HAKU_POLL_H

#include <haku/abi.h>

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN   HAKU_POLLIN
#define POLLOUT  HAKU_POLLOUT
#define POLLERR  HAKU_POLLERR
#define POLLHUP  HAKU_POLLHUP
#define POLLNVAL HAKU_POLLNVAL

/* poll(2) reports the readiness of every descriptor immediately. The
 * timeout is accepted for source compatibility but not waited on: see the
 * README's "Console and pipes" note. */
int poll(struct pollfd *fds, unsigned int count, int timeout);

#endif
