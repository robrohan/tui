/* glibc hides posix_openpt and friends without X/Open; on FreeBSD defining
 * it would hide the BSD extras instead, so only ask for it on Linux. */
#if defined(__linux__)
#define _XOPEN_SOURCE 700
#endif
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void set_size(int fd, int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(fd, TIOCSWINSZ, &ws);
}

static void run_shell(int slave)
{
    const char *shell = getenv("SHELL");
    const char *name;

    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
    {
        close(slave);
    }

    setenv("TERM", "xterm-256color", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");

    if (shell == 0 || shell[0] == '\0' || access(shell, X_OK) != 0)
    {
        shell = "/bin/sh";
    }
    name = strrchr(shell, '/');
    name = name ? name + 1 : shell;
    execl(shell, name, (char *)0);
    _exit(127);
}

int pty_spawn(pty_proc *p, int cols, int rows, const struct termios *modes)
{
    int master, slave;
    const char *name;
    pid_t pid;

    p->fd = -1;
    p->pid = -1;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
    {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0 || (name = ptsname(master)) == 0)
    {
        close(master);
        return -1;
    }
    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0)
    {
        close(master);
        return -1;
    }
    if (modes)
    {
        struct termios tio = *modes;
        tio.c_cc[VERASE] = 0x7F; /* we send DEL for Backspace */
#ifdef IUTF8
        tio.c_iflag |= IUTF8;
#endif
        tcsetattr(slave, TCSANOW, &tio);
    }
    set_size(slave, cols, rows);

    pid = fork();
    if (pid < 0)
    {
        close(slave);
        close(master);
        return -1;
    }
    if (pid == 0)
    {
        close(master);
        run_shell(slave);
    }
    close(slave);

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    p->fd = master;
    p->pid = pid;
    return 0;
}

long pty_read(pty_proc *p, void *buf, size_t cap)
{
    ssize_t n;
    if (p->fd < 0)
    {
        return -1;
    }
    n = read(p->fd, buf, cap);
    if (n > 0)
    {
        return (long)n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    {
        return 0;
    }
    /* 0 (BSD, macOS) or EIO (Linux): nothing has the other side open */
    return -1;
}

int pty_write(pty_proc *p, const void *buf, size_t n)
{
    const unsigned char *b = buf;
    int stalls = 0;
    while (n > 0 && p->fd >= 0)
    {
        ssize_t w = write(p->fd, b, n);
        if (w > 0)
        {
            b += w;
            n -= (size_t)w;
            stalls = 0;
            continue;
        }
        if (w < 0 && errno == EINTR)
        {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && stalls++ < 10)
        {
            /* the shell isn't reading: give it a moment, then drop the rest */
            fd_set out;
            struct timeval tv = {0, 50000};
            FD_ZERO(&out);
            FD_SET(p->fd, &out);
            select(p->fd + 1, 0, &out, 0, &tv);
            continue;
        }
        return -1;
    }
    return n == 0 ? 0 : -1;
}

void pty_resize(pty_proc *p, int cols, int rows)
{
    if (p->fd >= 0)
    {
        set_size(p->fd, cols, rows);
    }
}

void pty_close(pty_proc *p)
{
    if (p->pid > 0)
    {
        kill(-p->pid, SIGHUP);
        kill(p->pid, SIGHUP);
        waitpid(p->pid, 0, WNOHANG);
    }
    if (p->fd >= 0)
    {
        close(p->fd);
    }
    p->fd = -1;
    p->pid = -1;
}

void pty_reap(void)
{
    while (waitpid(-1, 0, WNOHANG) > 0)
    {
    }
}
