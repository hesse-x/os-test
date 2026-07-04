#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

// struct termios (sync with kernel/pty.h)
#define NCCS 19
typedef unsigned long tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

// c_cc indices
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9
#define VSUSP   10

// Input flags
#define ICRNL   0x0100
#define IXON    0x0400
#define IGNCR   0x0080
#define INLCR   0x0040
#define BRKINT  0x0002
#define IXOFF   0x1000

// Output flags
#define OPOST   0x0001
#define ONLCR   0x0004

// Control flags
#define CS8     0x0060
#define CLOCAL  0x0800

// Local flags
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x0200

// tcsetattr optional_actions
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

// struct winsize (sync with kernel/pty.h)
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// Functions
LIBC_EXPORT int tcgetattr(int fd, struct termios *termios_p);
LIBC_EXPORT int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

#ifdef __cplusplus
}
#endif

#endif /* _TERMIOS_H */
