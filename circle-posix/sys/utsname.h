//
// sys/utsname.h — the header for uname(), which the C library here does
// not have in any form.
//
// Found by this directory being on the include path, which posix.mk
// arranges. The implementation is in posix.cpp, and it fills every field
// from what the platform actually is.
//
#ifndef _circle_posix_sys_utsname_h
#define _circle_posix_sys_utsname_h

#ifdef __cplusplus
extern "C" {
#endif

#define _UTSNAME_LENGTH 65

struct utsname
{
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif // _circle_posix_sys_utsname_h
