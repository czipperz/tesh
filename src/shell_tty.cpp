#include "shell.hpp"

#include "config.hpp"

#ifdef _WIN32
#include <process.h>
#define NOMINMAX
#define WIN32LEANANDMEAN
#include <windows.h>
#else
#ifdef __APPLE__
#include <util.h>
#include <sys/ioctl.h>
#else
#include <pty.h>
#endif
#include <termios.h>
#include <unistd.h>
#endif

static bool disable_echo(Pseudo_Terminal* tty) {
#ifndef _WIN32
    struct termios termios;
    if (tcgetattr(tty->child_bi, &termios) < 0)
        return false;
    termios.c_lflag &= ~(ECHO);
    if (tcsetattr(tty->child_bi, TCSANOW, &termios) < 0)
        return false;
#endif
    return true;
}

#ifdef _WIN32
static bool create_named_pipe(cz::File_Descriptor* server,
                              cz::File_Descriptor* client,
                              bool server_write) {
    for (int number = 0; number <= 99999; ++number) {
        char filename[20];
        snprintf(filename, sizeof(filename), "\\\\.\\pipe\\tesh_%05d", number);

        DWORD open_flags =
            (server_write ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED;
        DWORD pipe_flags =
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | (server_write ? PIPE_NOWAIT : PIPE_NOWAIT);
        HANDLE handle_server =
            CreateNamedPipeA(filename, open_flags, pipe_flags, /*max_instances=*/2,
                             /*out_buffer_size=*/(8 << 10), /*in_buffer_size=*/(8 << 10),
                             /*default_timeout=*/50, /*security_attributes=*/NULL);
        if (handle_server == INVALID_HANDLE_VALUE)
            continue;

        bool client_read = server_write;
        DWORD client_mode = (client_read ? (GENERIC_READ | FILE_WRITE_ATTRIBUTES) : GENERIC_WRITE);
        HANDLE handle_client = CreateFile(filename, client_mode, 0, 0, OPEN_EXISTING, 0, 0);
        if (handle_client == INVALID_HANDLE_VALUE) {
            CloseHandle(handle_server);
            return false;
        }

        server->handle = handle_server;
        client->handle = handle_client;
        return true;
    }
    return false;
}
#endif

bool create_pseudo_terminal(Pseudo_Terminal* tty, int width, int height) {
#ifdef _WIN32
    if (!create_named_pipe(&tty->in, &tty->child_in, /*server_write=*/true))
        return false;
    if (!create_named_pipe(&tty->out, &tty->child_out, /*server_write=*/false))
        return false;  // TODO cleanup

    if (!tty->child_in.set_non_blocking())
        return false;  // TODO cleanup
    if (!tty->child_out.set_non_blocking())
        return false;  // TODO cleanup

    COORD size = {};
    if (cfg.windows_wide_terminal)
        size.X = 10000;
    else
        size.X = width;
    size.Y = height;

    HRESULT hr = CreatePseudoConsole(size, tty->child_in.handle, tty->child_out.handle, 0,
                                     &tty->pseudo_console);
    return hr == S_OK;
#else
    struct winsize size = {};
    size.ws_row = height;
    if (cfg.windows_wide_terminal) {
        size.ws_col = 1000;
    } else {
        size.ws_col = width;
    }
    int result =
        openpty(&tty->parent_bi, &tty->child_bi, /*name=*/nullptr, /*termios=*/nullptr, &size);
    if (result < 0)
        return false;

    cz::File_Descriptor parent;
    parent.handle = tty->parent_bi;
    if (!parent.set_non_inheritable()) {
        close(tty->child_bi);
        close(tty->parent_bi);
        return false;
    }
    if (!parent.set_non_blocking()) {
        close(tty->child_bi);
        close(tty->parent_bi);
        return false;
    }

    return true;
#endif
}

void destroy_pseudo_terminal(Pseudo_Terminal* tty) {
#ifdef _WIN32
    ClosePseudoConsole((HPCON)tty->pseudo_console);
    tty->child_in.close();
    tty->child_out.close();
    DisconnectNamedPipe(tty->in.handle);
    DisconnectNamedPipe(tty->out.handle);
#else
    close(tty->child_bi);
    close(tty->parent_bi);
#endif
}

bool set_window_size(Pseudo_Terminal* tty, int width, int height) {
#ifdef _WIN32
    COORD size = {};
    if (cfg.windows_wide_terminal)
        size.X = 1000;
    else
        size.X = width;
    size.Y = height;
    HRESULT result = ResizePseudoConsole(tty->pseudo_console, size);
    return result == S_OK;
#else
    struct winsize size = {};
    size.ws_row = height;
    if (cfg.windows_wide_terminal) {
        size.ws_col = 1000;
    } else {
        size.ws_col = width;
    }
    int result = ioctl(tty->child_bi, TIOCSWINSZ, &size);
    return result == 0;
#endif
}

int64_t tty_write(Pseudo_Terminal* tty, cz::Str message) {
    // Disable echo so we can print stdin in a different color.
    (void)disable_echo(tty);

#ifdef _WIN32
    return tty->in.write(message);
#else
    cz::Output_File in;
    in.handle = tty->parent_bi;
    return in.write(message);
#endif
}
