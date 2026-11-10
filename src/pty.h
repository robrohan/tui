#ifndef PTY_H
#define PTY_H

#include <stddef.h>
#include <sys/types.h>

struct termios;

typedef struct pty_proc
{
    int fd;
    pid_t pid;
} pty_proc;

int pty_spawn(pty_proc *p, int cols, int rows, const struct termios *modes);
long pty_read(pty_proc *p, void *buf, size_t cap);
int pty_write(pty_proc *p, const void *buf, size_t n);
void pty_resize(pty_proc *p, int cols, int rows);
void pty_close(pty_proc *p);
void pty_reap(void);

#endif
