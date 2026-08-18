//
// filesystem.cpp - SDL_GetBasePath and SDL_GetPrefPath.
//
// SDL gives an application two directories: the one it was installed in, and
// one it may write its settings and saved games into. On a desktop SDL works
// both out by itself - the first from where the running program came from,
// the second from the user's home directory and the organisation and
// application names the caller passes in.
//
// Neither question has an answer here. The payload was chain-loaded over a
// wire or started from a card; there is no program image to locate, no user,
// and no home directory. Where an application's files were put is a decision
// made when the card was built, and the only party that knows it is the
// consumer.
//
// So the consumer states it, with SDL2Circle_DeclareBasePath (SDL_circle.h),
// before SDL_Init - the same shape as SDL2Circle_DeclareVirtualDevice next
// door. The library never learns an application's name: it stores the
// string it is given, hands it back, and composes below it, without reading
// or checking it and without a default for any particular application.
//
// An undeclared consumer gets the root of the card, "/", rather than an
// error: SDL returns a non-null path on every desktop platform, so
// applications dereference it without checking, and a board has exactly one
// filesystem, of which "/" is a real, readable and writable directory. This
// differs from SDL2Circle_DeclareVirtualDevice, where there is no sane
// default display size and an undeclared virtual device stops SDL_Init
// instead. The warning here is logged once, the first time an undeclared
// path is handed out.
//
// Both functions return a string the caller frees with SDL_free, and both
// end in a separator - SDL's contract, which applications append to the
// result without checking for one.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace
{

// Long enough for any path a FAT card can hold, and fixed so that the
// declaration costs no allocation and can be made before anything is running.
const size_t BASE_PATH_MAX = 256;

char s_base[BASE_PATH_MAX] = "/";
bool s_declared = false;
bool s_answered = false;      // a path has been handed out; the base is fixed
bool s_warned = false;        // the undeclared warning has been given

// SDL hands the caller memory it releases with SDL_free.
char *DuplicatePath(const char *path)
{
    const size_t bytes = strlen(path) + 1;
    char *copy = (char *)SDL_malloc(bytes);
    if (!copy)
    {
        SDL_SetError("out of memory");
        return nullptr;
    }
    memcpy(copy, path, bytes);
    return copy;
}

// The base, and the note that it can no longer change.
const char *SettleBase(void)
{
    if (!s_declared && !s_warned)
    {
        s_warned = true;
        // SDL2Circle_DeclareBasePath is what sets it; the log says what
        // the answer is, not how to change it.
        SDL2Circle_Log("sdl2fs", SDL2CIRCLE_LOG_WARNING,
                       "no base path declared: answering \"%s\"", s_base);
    }
    s_answered = true;
    return s_base;
}

// Create one directory, treating "it is already there" as success. The I/O
// service answers with a negated errno and is valid from any core, which
// matters because this runs wherever the application runs.
bool MakeDirectory(const char *path)
{
    const int r = SDL2Circle_IOMkdir(path);
    return r == 0 || r == -EEXIST;
}

} // namespace

// ---------------------------------------------------------------------------
// The declaration
// ---------------------------------------------------------------------------

extern "C" int SDL2Circle_DeclareBasePath(const char *path)
{
    // The state tests come before the value, and in this order: once a path
    // has been handed out nothing can be declared at all, so complaining
    // about the value there would suggest that correcting it would help.
    if (s_answered)
        return SDL_SetError("SDL2Circle_DeclareBasePath: a path has already "
                            "been answered from \"%s\"", s_base);
    if (s_declared)
        return SDL_SetError("SDL2Circle_DeclareBasePath: a base path of "
                            "\"%s\" is already declared", s_base);
    if (!path || !*path)
        return SDL_SetError("SDL2Circle_DeclareBasePath: no path given");
    if (path[0] != '/')
        return SDL_SetError("SDL2Circle_DeclareBasePath: \"%s\" is not an "
                            "absolute path", path);

    const size_t len = strlen(path);
    const bool needs_separator = path[len - 1] != '/';
    if (len + (needs_separator ? 1 : 0) + 1 > BASE_PATH_MAX)
        return SDL_SetError("SDL2Circle_DeclareBasePath: \"%s\" is longer "
                            "than %u characters", path,
                            (unsigned)(BASE_PATH_MAX - 2));

    memcpy(s_base, path, len);
    if (needs_separator)
        s_base[len] = '/';
    s_base[len + (needs_separator ? 1 : 0)] = '\0';
    s_declared = true;
    return 0;
}

// ---------------------------------------------------------------------------
// The two SDL answers
// ---------------------------------------------------------------------------

extern "C" char *SDL_GetBasePath(void)
{
    return DuplicatePath(SettleBase());
}

extern "C" char *SDL_GetPrefPath(const char *org, const char *app)
{
    // SDL requires an application name and treats a missing organisation as
    // an empty one, which collapses the answer to <base>/<app>/.
    if (!app)
    {
        SDL_SetError("Parameter 'app' is invalid");
        return nullptr;
    }
    if (!org)
        org = "";

    const char *base = SettleBase();

    // <base><org>/<app>/  - or <base><app>/ with no organisation.
    const size_t len = strlen(base) + strlen(org) + strlen(app) + 3;
    char *path = (char *)SDL_malloc(len);
    if (!path)
    {
        SDL_SetError("out of memory");
        return nullptr;
    }
    if (*org)
        snprintf(path, len, "%s%s/%s/", base, org, app);
    else
        snprintf(path, len, "%s%s/", base, app);

    // SDL guarantees the directory exists when it answers, so each component
    // below the base is created. The base itself is not touched: it is the
    // consumer's declaration, and on a card its first component may be the
    // root, which cannot be created and does not need to be.
    //
    // Each component is made without its trailing separator, because that is
    // what a FAT filesystem accepts.
    for (char *p = path + strlen(base); *p; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        const bool made = MakeDirectory(path);
        *p = '/';
        if (!made)
        {
            SDL_SetError("SDL_GetPrefPath: could not create \"%s\"", path);
            SDL_free(path);
            return nullptr;
        }
    }

    return path;
}
