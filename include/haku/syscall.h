#ifndef HAKU_SYSCALL_H
#define HAKU_SYSCALL_H

#include <haku/abi.h>

int haku_syscall0(int number);
int haku_syscall1(int number, int a);
int haku_syscall2(int number, int a, int b);
int haku_syscall3(int number, int a, int b, int c);
int haku_syscall4(int number, int a, int b, int c, int d);
int haku_errno_check(int result);

#endif
