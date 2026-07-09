//
// hints.cpp — minimal hint storage (MAME sets a handful at startup)
//
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdlib>

struct Hint
{
    char *name;
    char *value;
    Hint *next;
};

static Hint *s_hints = nullptr;

static Hint *find_hint(const char *name)
{
    for (Hint *h = s_hints; h; h = h->next)
        if (strcmp(h->name, name) == 0)
            return h;
    return nullptr;
}

extern "C" SDL_bool SDL_SetHintWithPriority(const char *name, const char *value,
                                            SDL_HintPriority)
{
    if (!name)
        return SDL_FALSE;
    Hint *h = find_hint(name);
    if (!h)
    {
        h = new Hint{strdup(name), nullptr, s_hints};
        s_hints = h;
    }
    free(h->value);
    h->value = value ? strdup(value) : nullptr;
    return SDL_TRUE;
}

extern "C" SDL_bool SDL_SetHint(const char *name, const char *value)
{
    return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

extern "C" const char *SDL_GetHint(const char *name)
{
    Hint *h = name ? find_hint(name) : nullptr;
    return h ? h->value : nullptr;
}

extern "C" SDL_bool SDL_GetHintBoolean(const char *name, SDL_bool default_value)
{
    const char *v = SDL_GetHint(name);
    if (!v || !*v)
        return default_value;
    return (*v == '0' || strcasecmp(v, "false") == 0) ? SDL_FALSE : SDL_TRUE;
}

extern "C" SDL_bool SDL_ResetHint(const char *name)
{
    return SDL_SetHintWithPriority(name, nullptr, SDL_HINT_NORMAL);
}
