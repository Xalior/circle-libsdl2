//
// dlfcn.h — the header for dynamic loading, which this platform does not
// do and the C library has no header for.
//
// A program here is one statically linked image. There is no loader, no
// shared object to open and no symbol table to search at run time, so
// dlopen and dlsym in posix.cpp always report failure and dlerror says
// why. That is the same answer a desktop program gets when the library it
// asked for is not installed, and a program that calls dlopen already has
// to handle it.
//
#ifndef _circle_posix_dlfcn_h
#define _circle_posix_dlfcn_h

#ifdef __cplusplus
extern "C" {
#endif

// The standard flags. They are accepted and ignored, since nothing is ever
// opened.
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 4
#define RTLD_LOCAL  8

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif // _circle_posix_dlfcn_h
