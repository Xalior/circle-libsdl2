//
// sys/termios.h — the terminal-control structure, so that code which
// manipulates one compiles.
//
// The C library ships <termios.h>, and all it does is include this file,
// which the C library does not have. There is no terminal behind it: this
// platform has no serial line discipline, and the standard input, output
// and error the library binds are a keyboard and a log, not a tty.
//
// So nothing reads the values below. tcgetattr and tcsetattr in posix.cpp
// both report ENOTTY — there are no attributes to fetch and none to set —
// and these names exist so that a program which fetches a structure,
// changes a flag in it and sets it back is a program that still builds.
//
// Only the names needed so far are here. A consumer that needs another
// standard name adds it here, once, for everybody.
//
#ifndef _circle_posix_sys_termios_h
#define _circle_posix_sys_termios_h

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32

struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

// Ordinary POSIX values. Only self-consistency matters, since nothing on
// this platform interprets them.
#define ICANON  0000002
#define ECHO    0000010

#define VTIME   5
#define VMIN    6

#define TCSANOW 0

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#ifdef __cplusplus
}
#endif

#endif // _circle_posix_sys_termios_h
