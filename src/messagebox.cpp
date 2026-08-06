//
// messagebox.cpp — SDL_ShowSimpleMessageBox and SDL_ShowMessageBox.
//
// A message box is how an application says something it believes the user
// must see before anything else happens, and it is very often the last thing
// it does before giving up. There is no window manager here to put a dialog
// on the screen and nobody to click it, so the message goes to the serial
// log instead, at a severity taken from the flags the caller gave it.
//
// The alternative — doing nothing — loses exactly the diagnostic an
// application thought was important enough to interrupt for, and a board
// that stops with no explanation is the hardest kind to work out.
//
// A message box that expects an ANSWER is a different matter, and is
// reported below rather than guessed at.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

namespace
{

unsigned SeverityOf(Uint32 flags)
{
    if (flags & SDL_MESSAGEBOX_ERROR)       return SDL2CIRCLE_LOG_ERROR;
    if (flags & SDL_MESSAGEBOX_WARNING)     return SDL2CIRCLE_LOG_WARNING;
    return SDL2CIRCLE_LOG_NOTICE;   // information, or nothing stated
}

void LogBox(Uint32 flags, const char *title, const char *message)
{
    const unsigned severity = SeverityOf(flags);
    SDL2Circle_Log("messagebox", severity, "%s", title ? title : "(no title)");
    if (message)
        SDL2Circle_Log("messagebox", severity, "  %s", message);
}

} // namespace

extern "C" int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                                        const char *message, SDL_Window *)
{
    LogBox(flags, title, message);
    return 0;
}

extern "C" int SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
    if (!data)
        return SDL_SetError("SDL_ShowMessageBox: no message box data");

    LogBox(data->flags, data->title, data->message);

    // The caller is asking which button was pressed. Nobody pressed one, so
    // the honest answer is the button the application itself nominated as
    // the one to assume — the default for a return key, or failing that for
    // the escape key. Each is the application's own statement of what should
    // happen when the user does not choose, which is exactly the situation.
    if (buttonid)
    {
        *buttonid = -1;   // SDL's "no button was chosen"

        for (int i = 0; i < data->numbuttons; i++)
            if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT)
            {
                *buttonid = data->buttons[i].buttonid;
                break;
            }
        if (*buttonid == -1)
            for (int i = 0; i < data->numbuttons; i++)
                if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT)
                {
                    *buttonid = data->buttons[i].buttonid;
                    break;
                }

        for (int i = 0; i < data->numbuttons; i++)
            SDL2Circle_Log("messagebox", SeverityOf(data->flags),
                           "  [%d] %s%s", data->buttons[i].buttonid,
                           data->buttons[i].text ? data->buttons[i].text : "",
                           data->buttons[i].buttonid == *buttonid ? "  <- taken" : "");
    }
    return 0;
}
