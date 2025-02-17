#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <queue>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

static constexpr int32_t    INVALID_FD              = -1;
static int                  signal_pipe[2]          = {INVALID_FD, INVALID_FD};
static constexpr uint32_t   XDND_PROTOCOL_VERSION   = 5;

class DND
{
public:
    DND(void)
    {
    }

    ~DND(void)
    {
        for (auto fd : signal_pipe) {
            if (fd != INVALID_FD) {
                close(fd);
            }
        }

        for (auto window : windows) {
            xcb_destroy_window(connection, window);
        }

        if (connection) {
            xcb_disconnect(connection);
        }
    }

    bool Init(void)
    {
        if (!ListenSignal()) {
            return false;
        }

        connection = xcb_connect(nullptr, &screen_num);
        if (!connection) {
            fprintf(stderr, "xcb_connect() failed\n");
            return false;
        }

        setup = xcb_get_setup(connection);
        if (!setup) {
            fprintf(stderr, "xcb_get_setup() failed\n");
            return false;
        }

        auto iter = xcb_setup_roots_iterator(setup);
        if (!iter.data) {
            fprintf(stderr, "xcb_setup_roots_iterator() failed\n");
            return false;
        }
        screen = iter.data;

        if (!SetWindowAttribute(screen->root)) {
            return false;
        }
        return true;
    }

    bool ShowCase(void)
    {
        auto window = CreateWindow();
        if (window == XCB_WINDOW_NONE) {
            return false;
        }
        windows.insert(window);

        if (!MapWindow(window)) {
            return false;
        }

        if (!ChangeProperty(window, GetAtom("XdndAware"), XCB_ATOM_ATOM, 1, &XDND_PROTOCOL_VERSION)) {
            return false;
        }

        return RunEventLoop();
    }

    bool ChangeProperty(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t data_len, const void *data)
    {
        auto cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, window, property, type, 32, data_len, data);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_change_property_checked() failed\n");
            free(error);
            return false;
        }
        printf(" * xcb_change_property_checked      : 0x%08X, %s\n", window, GetAtomName(property));
        xcb_flush(connection);
        return true;
    }

    bool SetWindowAttribute(xcb_window_t window)
    {
        std::vector<uint32_t> values = {XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE, 0};
        auto cookie = xcb_change_window_attributes_checked(connection, window, XCB_CW_EVENT_MASK, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_change_window_attributes() failed\n");
            return false;
        }
        printf(" * xcb_change_window_attributes     : 0x%08X\n", window);
        return true;
    }

    bool ProcClientMessage(xcb_client_message_event_t *event)
    {
        printf("   - XCB_CLIENT_MESSAGE             : seq: %4u, window: 0x%08X, type: %s", event->sequence, event->window, GetAtomName(event->type));

        if (event->type == GetAtom("XdndEnter")) {
            xcb_window_t source     = event->data.data32[0];
            uint32_t     version    = event->data.data32[1] >> 24;
            bool         type_list  = event->data.data32[1] & 1;
            printf(", source: 0x%08X, version: %u, type_list: %s, formats:\n", source, version, type_list ? "y" : "n");

            std::set<xcb_atom_t> formats = {};
            if (type_list) {
                auto cookie = xcb_get_property(connection, 0, source, GetAtom("XdndTypeList"), XCB_ATOM_ANY, 0, 2048);
                auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                if (!reply) {
                    fprintf(stderr, "xcb_get_property_reply() failed\n");
                    return false;
                }
                auto atoms = reinterpret_cast<xcb_atom_t *>(xcb_get_property_value(reply));
                for (uint32_t i = 0; i < reply->length; i++) {
                    formats.insert(atoms[i]);
                }
                free(reply);
            } else {
                if (event->data.data32[2]) {
                    formats.insert(event->data.data32[2]);
                }
                if (event->data.data32[3]) {
                    formats.insert(event->data.data32[3]);
                }
                if (event->data.data32[4]) {
                    formats.insert(event->data.data32[4]);
                }
            }
            for (auto format : formats) {
                printf("     - %s\n", GetAtomName(format));
            }
            source_ver = version;
            source_formats = formats;
        } else if (event->type == GetAtom("XdndPosition")) {
            xcb_window_t source     = event->data.data32[0];
            uint32_t     flags      = event->data.data32[1];        // reserved for future use
            uint32_t     x          = 0;
            uint32_t     y          = 0;
            uint32_t     root_x     = 0xffff & (event->data.data32[2] >> 16);
            uint32_t     root_y     = 0xffff & event->data.data32[2];
            uint32_t     timestamp  = event->data.data32[3];        // in version 1
            xcb_atom_t   action     = event->data.data32[4];        // in version 2

            {
                auto cookie = xcb_translate_coordinates(connection, screen->root, event->window, root_x, root_y);
                auto reply = xcb_translate_coordinates_reply(connection, cookie, nullptr);
                if (!reply) {
                    return false;
                }
                x = reply->dst_x;
                y = reply->dst_y;
                free(reply);
            }
            printf(", source: 0x%08X, flags: 0x%08X, x: %u, y: %u, root_x: %u, root_y: %u, timestamp: %u, action: %s\n", source, flags, x, y, root_x, root_y, timestamp, GetAtomName(action));

            {
                xcb_client_message_event_t reply = {
                    .response_type  = XCB_CLIENT_MESSAGE,
                    .format         = 32,
                    .sequence       = 0,
                    .window         = source,
                    .type           = GetAtom("XdndStatus"),
                    .data           = { .data32 = { event->window, 1, 0, 0, action }},
                };
                auto cookie = xcb_send_event_checked(connection, 0, source, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&reply));
                auto error = xcb_request_check(connection, cookie);
                if (error) {
                    printf("failed to send event to xcb_window: 0x%08x (err: %d)\n", source, error->error_code);
                    free(error);
                    return false;
                }
                source_action = action;
            }
        } else if (event->type == GetAtom("XdndStatus")) {
            xcb_window_t target     = event->data.data32[0];
            bool         drop       = event->data.data32[1] & 1;
            bool         want       = event->data.data32[1] & 2;
            uint32_t     rect_x     = 0xffff & (event->data.data32[2] >> 16);
            uint32_t     rect_y     = 0xffff & event->data.data32[2];
            uint32_t     rect_w     = 0xffff & (event->data.data32[3] >> 16);
            uint32_t     rect_h     = 0xffff & event->data.data32[3];
            uint32_t     action     = event->data.data32[4];        // in version 2
            printf(", target: 0x%08X, drop: %d, want: %d, rect: (%u, %u, %u, %u), action: %u\n", target, drop, want, rect_x, rect_y, rect_w, rect_h, action);
        } else if (event->type == GetAtom("XdndLeave")) {
            xcb_window_t source     = event->data.data32[0];
            uint32_t     flags      = event->data.data32[1];        // reserved for future use
            printf(", source: 0x%08X, flags: 0x%08X\n", source, flags);
        } else if (event->type == GetAtom("XdndDrop")) {
            xcb_window_t source     = event->data.data32[0];
            uint32_t     flags      = event->data.data32[1];        // reserved for future use
            uint32_t     timestamp  = event->data.data32[2];        // in version 1
            printf(", source: 0x%08X, flags: 0x%08X, timestamp: %u\n", source, flags, timestamp);

            xcb_atom_t target = GetAtom("text/plain");
            if (source_formats.find(target) != source_formats.end()) {
                xcb_timestamp_t time = source_ver >= 1 ? timestamp : XCB_CURRENT_TIME;
                xcb_atom_t property = XCB_ATOM_CUT_BUFFER0 + (cut_buffer_idx++ % 8);
                this->source = source;
                auto cookie = xcb_convert_selection_checked(connection, event->window, GetAtom("XdndSelection"), target, property, time);
                auto error = xcb_request_check(connection, cookie);
                if (error) {
                    printf("failed to send event to xcb_window: 0x%08x (err: %d)\n", source, error->error_code);
                    free(error);
                    return false;
                }
            } else if (source_ver >= 2) {
                xcb_client_message_event_t reply = {
                    .response_type  = XCB_CLIENT_MESSAGE,
                    .format         = 32,
                    .sequence       = 0,
                    .window         = source,
                    .type           = GetAtom("XdndFinished"),
                    .data           = { .data32 = { event->window, 0, 0, 0, 0 }},
                };
                auto cookie = xcb_send_event_checked(connection, 0, source, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&reply));
                auto error = xcb_request_check(connection, cookie);
                if (error) {
                    printf("failed to send event to xcb_window: 0x%08x (err: %d)\n", source, error->error_code);
                    free(error);
                    return false;
                }
            }
        } else if (event->type == GetAtom("XdndFinished")) {        // in version 2
            xcb_window_t target     = event->data.data32[0];
            bool         drop       = event->data.data32[1] & 1;    // in version 5
            uint32_t     action     = event->data.data32[2];        // in version 5
            printf(", target: 0x%08X, drop: %d, action: %u\n", target, drop, action);
        } else {
            printf("\n");
        }
        return true;
    }

    bool ProcSelectionNotify(xcb_selection_notify_event_t *event)
    {
        printf("   - XCB_SELECTION_NOTIFY           : seq: %4u, time: %10u, requestor: 0x%08X, selection: '%s', target: '%s', property: '%s'",
            event->sequence, event->time, event->requestor, GetAtomName(event->selection), GetAtomName(event->target), event->property ? GetAtomName(event->property) : "(null)");

        if (event->selection == GetAtom("XdndSelection")) {
            if (event->target == GetAtom("text/plain")) {
                if (event->property != XCB_ATOM_NONE) {
                    auto cookie = xcb_get_property(connection, true, event->requestor, event->property, XCB_ATOM_ANY, 0, 2048);
                    auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                    if (reply) {
                        std::string text = "";
                        text.assign(reinterpret_cast<char *>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
                        printf(", value: '%s'", text.c_str());
                        free(reply);
                    }
                }
                if (source_ver >= 2) {
                    xcb_client_message_event_t reply = {
                        .response_type  = XCB_CLIENT_MESSAGE,
                        .format         = 32,
                        .sequence       = 0,
                        .window         = source,
                        .type           = GetAtom("XdndFinished"),
                        .data           = { .data32 = { event->requestor, 1, source_action, 0, 0 }},
                    };
                    auto cookie = xcb_send_event_checked(connection, 0, source, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&reply));
                    auto error = xcb_request_check(connection, cookie);
                    if (error) {
                        printf("failed to send event to xcb_window: 0x%08x (err: %d)\n", source, error->error_code);
                        free(error);
                        return false;
                    }
                }
            }
        }
        printf("\n");
        return true;
    }

    bool ProcDestroyNotify(xcb_destroy_notify_event_t *event)
    {
        printf("   - XCB_DESTROY_NOTIFY             : seq: %4u, event: 0x%08X, root: 0x%08X\n", event->sequence, event->event, event->window);
        return true;
    }

    bool ProcButtonPress(xcb_button_press_event_t *event)
    {
        printf("   - XCB_BUTTON_PRESS               : seq: %4u, time: %10u, root: 0x%08X, event: 0x%08X, child: 0x%08X, event_x: %d, event_y: %d, state: %u, same_screen: %u\n",
            event->sequence, event->time, event->root, event->event, event->child, event->event_x, event->event_y, event->state, event->same_screen);
        return true;
    }

    bool ProcPropertyNotify(xcb_property_notify_event_t *event)
    {
        printf("   - XCB_PROPERTY_NOTIFY            : seq: %4u, time: %10u, window: 0x%08X, state: '%s', atom: '%s'\n",
            event->sequence, event->time, event->window, event->state == XCB_PROPERTY_NEW_VALUE ? "new" : "del", GetAtomName(event->atom));

        if (event->atom == GetAtom("_NET_WM_WINDOW_TYPE")) {
            auto cookie = xcb_get_property(connection, 0, event->window, event->atom, XCB_ATOM_ANY, 0, 2048);
            auto reply = xcb_get_property_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "xcb_get_property_reply() failed\n");
                return false;
            }
            auto atoms = reinterpret_cast<xcb_atom_t *>(xcb_get_property_value(reply));
            for (uint32_t i = 0; i < reply->length; i++) {
                printf("     . %s\n", GetAtomName(atoms[i]));
            }
        }
        return true;
    }

    xcb_window_t CreateWindow(void)
    {
        uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
        std::vector<uint32_t> values = { XCB_BACK_PIXMAP_NONE, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS};

        xcb_window_t window = xcb_generate_id(connection);
        auto cookie = xcb_create_window_checked(connection, screen->root_depth, window, screen->root,
            0, 0, 200, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_create_window_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return XCB_WINDOW_NONE;
        }
        return window;
    }

    bool MapWindow(xcb_window_t window)
    {
        auto cookie = xcb_map_window_checked(connection, window);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_map_window_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        return true;
    }

    bool ProcEvent(xcb_generic_event_t *event)
    {
        switch (event->response_type & ~0x80)
        {
            case XCB_CLIENT_MESSAGE:
                if (!ProcClientMessage(reinterpret_cast<xcb_client_message_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_SELECTION_NOTIFY:
                if (!ProcSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_DESTROY_NOTIFY:
                if (!ProcDestroyNotify(reinterpret_cast<xcb_destroy_notify_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_BUTTON_PRESS:
                if (!ProcButtonPress(reinterpret_cast<xcb_button_press_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_PROPERTY_NOTIFY:
                if (!ProcPropertyNotify(reinterpret_cast<xcb_property_notify_event_t *>(event))) {
                    return false;
                }
                break;
        }
        return true;
    }

    xcb_atom_t GetAtom(const char *name)
    {
        auto iter = atoms.find(name);
        if (iter != atoms.end()) {
            return iter->second;
        }

        auto cookie = xcb_intern_atom(connection, 0, strlen(name), name);
        auto reply = xcb_intern_atom_reply(connection, cookie, nullptr);
        if (!reply) {
            fprintf(stderr, "xcb_intern_atom_reply() failed '%s'\n", name);
            return XCB_NONE;
        }

        auto atom = reply->atom;
        atoms[name] = atom;
        atom_names[atom] = name;
        free(reply);
        return atom;
    }

    const char *GetAtomName(xcb_atom_t atom)
    {
        auto iter = atom_names.find(atom);
        if (iter != atom_names.end()) {
            return iter->second.c_str();
        }

        auto cookie = xcb_get_atom_name(connection, atom);
        auto reply = xcb_get_atom_name_reply(connection, cookie, nullptr);
        if (!reply) {
            return "Unknown";
        }

        atom_names[atom].assign(xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
        atoms[atom_names[atom]] = atom;
        free(reply);
        return atom_names[atom].c_str();
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
                auto rc = ProcEvent(event);
                free(event);
                if (!rc) {
                    return false;
                }
            }
        }
        return true;
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
    int                                         screen_num                  = 0;
    xcb_connection_t                           *connection                  = nullptr;
    const xcb_setup_t                          *setup                       = nullptr;
    xcb_screen_t                               *screen                      = nullptr;
    uint8_t                                     cut_buffer_idx              = 0;
    xcb_window_t                                source                      = XCB_WINDOW_NONE;
    uint32_t                                    source_ver                  = 0;
    std::set<xcb_atom_t>                        source_formats              = {};
    xcb_atom_t                                  source_action               = XCB_ATOM_NONE;

    std::set<xcb_window_t>                      windows                     = {};
    std::map<std::string, xcb_atom_t>           atoms                       = {};
    std::map<xcb_atom_t, std::string>           atom_names                  = {};
};

int main(int argc, char **argv)
{
    printf("Example xcb_net_wm\n");

    auto obj = DND();
    if (!obj.Init() || !obj.ShowCase()) {
        printf("\nFailed..\n");
        return EXIT_FAILURE;
    }
    printf("\nSucceed..\n");
    return EXIT_SUCCESS;
}
