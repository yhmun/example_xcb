#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <xcb/xcb.h>

constexpr int32_t   INVALID_FD      = -1;
static int          signal_pipe[2]  = {INVALID_FD, INVALID_FD};

class Signal
{
public:
    Signal(void)
    {
    }

    ~Signal(void)
    {
        for (auto fd : signal_pipe) {
            if (fd != INVALID_FD) {
                close(fd);
            }
        }

        if (connection) {
            xcb_disconnect(connection);
        }
    }

    bool Init(void)
    {
        connection = xcb_connect(nullptr, &screen_num);
        if (!connection) {
            fprintf(stderr, "xcb_connect() failed\n");
            return false;
        }
        return true;
    }

    bool ShowCase(void)
    {
        if (!ListenSignal()) {
            return false;
        }
        return RunEventLoop();
    }

    bool ListenSignal(void)
    {
        if (pipe(signal_pipe)) {
            fprintf(stderr, "pipe() failed\n");
            return false;
        }

        for (auto fd : signal_pipe) {
            auto fd_flags = fcntl(fd, F_GETFL);
            if (fd_flags == -1) {
                fprintf(stderr, "fcntl(F_GETFL) failed\n");
                return false;
            }

            if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) == -1) {
                fprintf(stderr, "fcntl(F_SETFL - O_NONBLOCK) failed\n");
                return false;
            }
        }

        signal(SIGINT, OnSignal);
        signal(SIGTERM, OnSignal);
        return true;
    }

    bool RunEventLoop(void)
    {
        printf("\n * Run event loop\n");
        xcb_flush(connection);

        while (true) {
            auto signum = 0;
            auto bytes = read(signal_pipe[0], &signum, sizeof(int));
            if (bytes == -1) {
                if (errno == EINTR) {
                    continue;
                }
            } else if (bytes == sizeof(int)) {
                printf(" - Unix signal (%d) received\n", signum);
                return true;
            }

            auto rc = xcb_connection_has_error(connection);
            if (rc) {
                fprintf(stderr, "xcb_connection_has_error() - %d\n", rc);
                return false;
            }

            auto event = xcb_poll_for_event(connection);
            if (event) {
                free(event);
            }
        }
        return true;
    }

    static void OnSignal(int signum)
    {
        while (true) {
            auto bytes = write(signal_pipe[1], &signum, sizeof(int));
            if (bytes == -1 && EINTR) {
                continue;
            } else if (bytes != sizeof(int)) {
                fprintf(stderr, "Unix signal %d lost\n", signum);
                _exit(EXIT_FAILURE);
            }
            break;
        }
    }

private:
    int                 screen_num      = 0;
    xcb_connection_t   *connection      = nullptr;
};


int main(int argc, char **argv)
{
    printf("Example xcb_signal\n");

    auto obj = Signal();
    if (!obj.Init() || !obj.ShowCase()) {
        printf("\nFailed..\n");
        return EXIT_FAILURE;
    }
    printf("\nSucceed..\n");
    return EXIT_SUCCESS;
}
