//
// clipboard.cpp - SDL_SetClipboardText, SDL_GetClipboardText,
// SDL_HasClipboardText, and the primary selection alongside them.
//
// A clipboard is normally the window system's: one shared buffer that every
// running program can read and write, which is how text moves between them.
// There is no window system here and no other program to move text to, so
// the clipboard is the library's own - one buffer, held in memory, for as
// long as the application runs.
//
// This is a real clipboard, not a stub: from inside the application it is
// indistinguishable from a desktop one, text put in comes back out, copy and
// paste inside a game work, and SDL_HasClipboardText answers truthfully
// about what is there. What it cannot do is carry text to another program,
// and there is no other program.
//
// SDL's own fallback - what SDL uses on a platform whose video backend
// provides no clipboard hooks - is exactly this, so the behaviour here is
// SDL's documented behaviour rather than an approximation of it, down to the
// SDL_CLIPBOARDUPDATE event that a change posts.
//
// Two ownership rules that applications rely on: what SDL_GetClipboardText
// returns belongs to the caller and is released with SDL_free, and is a copy
// so the application may keep it or edit it; and an empty clipboard answers
// with an empty allocated string, never a null pointer, since an application
// pastes the result straight into a string function and null would be a
// crash where "" is a no-op.
//
#include <SDL2/SDL.h>

#include <cstring>

namespace
{

// Null means "nothing has ever been put in it", which reads the same to a
// caller as an empty string and costs nothing until something is.
char *s_clipboard = nullptr;
char *s_primary = nullptr;

// Replace one of the two buffers. Storing is a copy, because the text the
// caller passed in is the caller's and may be gone by the next call.
int Store(char **slot, const char *text)
{
    if (!text)
        text = "";

    const size_t bytes = strlen(text) + 1;
    char *copy = (char *)SDL_malloc(bytes);
    if (!copy)
        return SDL_SetError("out of memory");
    memcpy(copy, text, bytes);

    SDL_free(*slot);
    *slot = copy;
    return 0;
}

char *Fetch(char *const *slot)
{
    const char *text = *slot ? *slot : "";
    const size_t bytes = strlen(text) + 1;
    char *copy = (char *)SDL_malloc(bytes);
    if (!copy)
    {
        SDL_SetError("out of memory");
        return nullptr;
    }
    memcpy(copy, text, bytes);
    return copy;
}

SDL_bool Has(char *const *slot)
{
    return (*slot && **slot) ? SDL_TRUE : SDL_FALSE;
}

// SDL tells the application when the clipboard changes, whoever changed it.
// An application that mirrors the clipboard into its own UI watches for this
// rather than polling.
void AnnounceChange(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_CLIPBOARDUPDATE;
    event.common.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

} // namespace

extern "C" int SDL_SetClipboardText(const char *text)
{
    if (Store(&s_clipboard, text) != 0)
        return -1;
    AnnounceChange();
    return 0;
}

extern "C" char *SDL_GetClipboardText(void)
{
    return Fetch(&s_clipboard);
}

extern "C" SDL_bool SDL_HasClipboardText(void)
{
    return Has(&s_clipboard);
}

// The primary selection is X11's second clipboard - the one that middle-click
// pastes from. It has its own buffer here for the same reason the main one
// does, and applications that use both keep getting two independent answers.

extern "C" int SDL_SetPrimarySelectionText(const char *text)
{
    if (Store(&s_primary, text) != 0)
        return -1;
    AnnounceChange();
    return 0;
}

extern "C" char *SDL_GetPrimarySelectionText(void)
{
    return Fetch(&s_primary);
}

extern "C" SDL_bool SDL_HasPrimarySelectionText(void)
{
    return Has(&s_primary);
}
