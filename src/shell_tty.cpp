#include "shell.hpp"

#ifdef _WIN32
#include <process.h>
#define NOMINMAX
#define WIN32LEANANDMEAN
#include <windows.h>
#else
#include <pty.h>
#include <unistd.h>
#endif

bool create_pseudo_terminal(Pseudo_Terminal* tty, int width, int height) {
#ifdef _WIN32
    if (!cz::create_pipe(&tty->child_in, &tty->in))
        return false;
    if (!cz::create_pipe(&tty->out, &tty->child_out))
        return false;  // TODO cleanup

    if (!tty->in.set_non_blocking())
        return false;  // TODO cleanup
    if (!tty->out.set_non_blocking())
        return false;  // TODO cleanup

    COORD size = {};
    size.X = width;
    size.Y = height;

    HRESULT hr = CreatePseudoConsole(size, tty->child_in.handle, tty->child_out.handle, 0,
                                     &tty->pseudo_console);
    return hr == S_OK;
#else
    struct winsize size = {};
    size.ws_row = height;
    size.ws_col = width;
    int result = openpty(&tty->parent, &tty->child, /*name=*/nullptr, /*termios=*/nullptr, &size);
    return result == 0;
#endif
}

void destroy_pseudo_terminal(Pseudo_Terminal* tty) {
#ifdef _WIN32
    ClosePseudoConsole((HPCON)tty->pseudo_console);
    tty->child_in.close();
    tty->child_out.close();
    tty->in.close();
    tty->out.close();
#else
    close(tty->child_bi);
    close(tty->parent_bi);
#endif
}

bool set_window_size(Pseudo_Terminal* tty, int width, int height) {
#ifdef _WIN32
    COORD size = {};
    size.X = width;
    size.Y = height;
    HRESULT result = ResizePseudoConsole(tty->pseudo_console, size);
    return result == S_OK;
#else
    struct winsize size = {};
    size.ws_row = height;
    size.ws_col = width;
    int result = ioctl(tty->child, TIOCSWINSZ, &size);
    return result == 0;
#endif
}
