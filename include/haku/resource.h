#ifndef HAKU_RESOURCE_H
#define HAKU_RESOURCE_H

#include <haku/abi.h>

typedef haku_rlimit_t rlimit_t;

#define RLIMIT_CPU      HAKU_RLIMIT_CPU
#define RLIMIT_FSIZE    HAKU_RLIMIT_FSIZE
#define RLIMIT_DATA     HAKU_RLIMIT_DATA
#define RLIMIT_STACK    HAKU_RLIMIT_STACK
#define RLIMIT_CORE     HAKU_RLIMIT_CORE
#define RLIMIT_RSS      HAKU_RLIMIT_RSS
#define RLIMIT_NPROC    HAKU_RLIMIT_NPROC
#define RLIMIT_NOFILE   HAKU_RLIMIT_NOFILE
#define RLIMIT_MEMLOCK  HAKU_RLIMIT_MEMLOCK
#define RLIMIT_AS       HAKU_RLIMIT_AS
#define RLIM_INFINITY   HAKU_RLIM_INFINITY

/* The limits are fixed by the kernel's table sizes; setrlimit accepts a
 * value but does not change anything. */
int getrlimit(int resource, rlimit_t *limits);
int setrlimit(int resource, const rlimit_t *limits);

#endif
