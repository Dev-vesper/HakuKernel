#ifndef HAKU_ERRNO_H
#define HAKU_ERRNO_H

#include <haku/abi.h>

#define E2BIG        HAKU_E2BIG
#define ENOENT       HAKU_ENOENT
#define EIO          HAKU_EIO
#define ENOEXEC      HAKU_ENOEXEC
#define ESRCH        HAKU_ESRCH
#define EINTR        HAKU_EINTR
#define EBADF        HAKU_EBADF
#define ECHILD       HAKU_ECHILD
#define EAGAIN       HAKU_EAGAIN
#define ETIMEDOUT    HAKU_ETIMEDOUT
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
#define ENOLCK       HAKU_ENOLCK
#define ENOSYS       HAKU_ENOSYS
#define ENOTEMPTY    HAKU_ENOTEMPTY
#define EPERM        HAKU_EPERM
#define EACCES       HAKU_EACCES
#define EBUSY        HAKU_EBUSY
#define EXDEV        HAKU_EXDEV
#define ENODEV       HAKU_ENODEV
#define ENFILE       HAKU_ENFILE
#define ENOTTY       HAKU_ENOTTY
#define EFBIG        HAKU_EFBIG
#define ESPIPE       HAKU_ESPIPE
#define EPIPE        HAKU_EPIPE
#define ERANGE       HAKU_ERANGE
#define ELOOP        HAKU_ELOOP
#define EOVERFLOW    HAKU_EOVERFLOW
#define EOPNOTSUPP   HAKU_EOPNOTSUPP

extern int errno;

#endif
