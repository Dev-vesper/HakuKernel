#ifndef HAKU_ERRNO_H
#define HAKU_ERRNO_H

#include <haku/abi.h>

#define E2BIG        HAKU_E2BIG
#define ENOENT       HAKU_ENOENT
#define EIO          HAKU_EIO
#define ENOEXEC      HAKU_ENOEXEC
#define EBADF        HAKU_EBADF
#define ECHILD       HAKU_ECHILD
#define EAGAIN       HAKU_EAGAIN
#define ENOMEM       HAKU_ENOMEM
#define EFAULT       HAKU_EFAULT
#define EEXIST       HAKU_EEXIST
#define ENOTDIR      HAKU_ENOTDIR
#define EISDIR       HAKU_EISDIR
#define EINVAL       HAKU_EINVAL
#define EMFILE       HAKU_EMFILE
#define ENOSPC       HAKU_ENOSPC
#define EROFS        HAKU_EROFS
#define ENAMETOOLONG HAKU_ENAMETOOLONG
#define ENOSYS       HAKU_ENOSYS
#define ENOTEMPTY    HAKU_ENOTEMPTY

extern int errno;

#endif
