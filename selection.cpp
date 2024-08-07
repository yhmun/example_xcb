#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <xcb/xcb.h>

constexpr int32_t   INVALID_FD      = -1;
static int          signal_pipe[2]  = {INVALID_FD, INVALID_FD};

class Selection
{
public:
    Selection(void)
    {
    }

    ~Selection(void)
    {
        for (auto fd : signal_pipe) {
            if (fd != INVALID_FD) {
                close(fd);
            }
        }

        if (window) {
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

        if (!CreateWindow()) {
            return false;
        }

        if (!MapWindow()) {
            return false;
        }
        return true;
    }

    bool ShowCase(void)
    {
        /**
         * Case 1. Who is the selection owner?
         *         - xcb_get_selection_owner()
         *
         * Case 2. Transfer data from the other application
         *         1) xcb_convert_selection_checked() with 'TARGETS' and user property
         *         2) XCB_SELECTION_NOTIFY says that target (mime_type) list are stored in the given property
         *         3) read list by xcb_get_property()
         *         4) xcb_convert_selection_checked() with one of targets and user property
         *         5) XCB_SELECTION_NOTIFY says that data is ready
         *         6) read data by xcb_get_property()
         *
         * Case 3. Transfer data from us
         *         1) xcb_set_selection_owner() takes selection owership
         *         2) response XCB_SELECTION_REQUEST with TARGETS and user property
         *         3) write target (mime_type) list in the given property
         *         4) send xcb_selection_notify_event_t to the requestor by xcb_send_event_checked()
         *         5) response XCB_SELECTION_REQUEST with one of targets and user property
         *         6) write data in the given property
         *         7) send xcb_selection_notify_event_t to the requestor by xcb_send_event_checked()
         *
         * Case 4. Lost selection ownership
         *         - receive XCB_SELECTION_CLEAR
         *
         *   Note. X server may accept STRING and UTF8_STRING while 'text/plain' | 'text/plain;charset=utf-8' may not
         */

        // Case 1
        auto primary_owner = GetSelectionOwner(GetAtom("PRIMARY"));
        auto secondary_owner = GetSelectionOwner(GetAtom("SECONDARY"));
        auto clipboard_owner = GetSelectionOwner(GetAtom("CLIPBOARD"));

        printf("\n");
        printf(" * xcb_screen_root                  : 0x%08X\n", screen->root);
        printf(" * xcb_window                       : 0x%08X\n", window);
        printf(" * xcb_primary_owner                : 0x%08X\n", primary_owner);
        printf(" * xcb_secondary_owner              : 0x%08X\n", secondary_owner);
        printf(" * xcb_clipboard_owner              : 0x%08X\n", clipboard_owner);

        if (primary_owner && primary_owner != window) {
            // Case 2-1. request available targets aka 'mime_types' from the selection owner
            if (!ConvertSelection(window, GetAtom("PRIMARY"), GetAtom("TARGETS"))) {
                return false;
            }
        }

        if (secondary_owner && secondary_owner != window) {
            // Case 2-1. request available targets aka 'mime_types' from the selection owner
            if (!ConvertSelection(window, GetAtom("SECONDARY"), GetAtom("TARGETS"))) {
                return false;
            }
        }

        if (clipboard_owner && clipboard_owner != window) {
            // Case 2-1. request available targets aka 'mime_types' from the selection owner
            if (!ConvertSelection(window, GetAtom("CLIPBOARD"), GetAtom("TARGETS"))) {
                return false;
            }
        }

        return RunEventLoop();
    }

    xcb_atom_t GetNetCutBuffer(void)
    {
        return static_cast<xcb_atom_t>(XCB_ATOM_CUT_BUFFER0 + (cut_buffer_idx++) % 8);
    }

    bool SetSelectionOwner(xcb_window_t owner, xcb_atom_t selection)
    {
        auto cookie = xcb_set_selection_owner_checked(connection, owner, selection, XCB_CURRENT_TIME);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_set_selection_owner_checked() failed\n");
            free(error);
            return false;
        }
        return true;
    }

    xcb_window_t GetSelectionOwner(xcb_atom_t selection)
    {
        auto cookie = xcb_get_selection_owner(connection, selection);
        auto reply = xcb_get_selection_owner_reply(connection, cookie, nullptr);
        if (!reply) {
            fprintf(stderr, "xcb_get_selection_owner_reply() failed\n");
            return XCB_WINDOW_NONE;
        }
        auto owner = reply->owner;
        free(reply);
        return owner;
    }

    bool ConvertSelection(xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target, xcb_atom_t property = XCB_NONE)
    {
        if (property == XCB_NONE) {
            property = GetNetCutBuffer();
        }

        auto cookie = xcb_convert_selection_checked(connection, requestor, selection, target, property, XCB_CURRENT_TIME);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_convert_selection_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        printf(" * xcb_convert_selection_checked()  : requestor 0x%08X, selection '%s', target '%s', property '%s'\n",
            requestor, GetAtomName(selection), GetAtomName(target), property ? GetAtomName(property) : "(null)");
        return true;
    }

    bool ProcButtonPress(xcb_button_press_event_t *event)
    {
        printf("   - XCB_BUTTON_PRESS               : seq %4u, time %8u, root 0x%08X, event 0x%08X, child 0x%08X, event_x %d, event_y %d, state: %u, same_screen: %u\n",
            event->sequence, event->time, event->root, event->event, event->child, event->event_x, event->event_y, event->state, event->same_screen);

        if (GetSelectionOwner(GetAtom("CLIPBOARD")) == window) {
            return true;
        }

        // Case 3-1. take clipboard owership
        if (!SetSelectionOwner(window, GetAtom("CLIPBOARD"))) {
            return false;
        }
        printf("\n * xcb_clipboard_owner              : 0x%08X\n", window);
        return true;
    }

    bool ProcSelectionClear(xcb_selection_clear_event_t *event)
    {
        // Case 4
        printf("   - XCB_SELECTION_CLEAR            : seq %4u, time %8u, owner 0x%08X, selection '%s'\n",
            event->sequence, event->time, event->owner, GetAtomName(event->selection));

        if (event->owner != window) {
            // we don't lost selection owenership
            return true;
        }

        if (GetSelectionOwner(event->selection) == XCB_WINDOW_NONE) {
            // no one has selection owenership
            return true;
        }

        // we lost selection owenership, so we can request mime_types
        if (!ConvertSelection(window, event->selection, GetAtom("TARGETS"))) {
            return false;
        }
        return true;
    }

    bool ProcSelectionRequest(xcb_selection_request_event_t *event)
    {
        printf("   - XCB_SELECTION_REQUEST          : seq: %4u, time %8u, owner 0x%08X, requestor 0x%08X, selection '%s', target '%s', property '%s'\n",
            event->sequence, event->time, event->owner, event->requestor, GetAtomName(event->selection), GetAtomName(event->target), event->property ? GetAtomName(event->property) : "(null)");

        if (event->requestor == window) {
            // ignore this request from us
            return true;
        }

        if (!event->property) {
            event->property = event->target;
        }

        if (event->target == GetAtom("TARGETS")) {  // Case 3-2
            // Case 3-3
            std::vector<xcb_atom_t> values = {
                GetAtom("STRING"), GetAtom("UTF8_STRING")
            };
            auto cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                event->requestor, event->property, XCB_ATOM_ATOM, 8 * sizeof(xcb_atom_t), values.size(), values.data());
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_change_property_checked() failed (err: %d)\n", error->error_code);
                free(error);
                return false;
            }
        } else {    // Case 3-5
            // Case 3-6
            std::string data = "This is an xcb selection example";
            auto cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                event->requestor, event->property, event->target, 8, data.size(), data.c_str());
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_change_property_checked() failed (err: %d)\n", error->error_code);
                free(error);
                return false;
            }
        }

        // Case 3-4 and 3-7
        xcb_selection_notify_event_t notify = {
            .response_type = XCB_SELECTION_NOTIFY,
            .pad0          = 0,
            .sequence      = 0,
            .time          = event->time,
            .requestor     = event->requestor,
            .selection     = event->selection,
            .target        = event->target,
            .property      = event->property
        };
        auto cookie = xcb_send_event_checked(connection, 0, event->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char *>(&notify));
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_send_event_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        xcb_flush(connection);
        return true;
    }

    bool ProcSelectionNotify(xcb_selection_notify_event_t *event)
    {
        printf("   - XCB_SELECTION_NOTIFY           : seq: %4u, time: %8u, requestor: 0x%08X, selection: '%s', target: '%s', property: '%s'\n",
            event->sequence, event->time, event->requestor, GetAtomName(event->selection), GetAtomName(event->target), event->property ? GetAtomName(event->property) : "(null)");

        if (event->requestor != window) {
            // requestor is not us
            return true;
        }

        if (!event->property) {
            event->property = event->target;
        }

        if (event->target == GetAtom("TARGETS")) {  // Case 2-2
            // Case 2-3
            auto cookie = xcb_get_property(connection, 0, window, event->property, XCB_ATOM_ATOM, 0, 1024);
            auto reply = xcb_get_property_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "xcb_get_property_reply() failed\n");
                return false;
            }
            auto mime_types = reinterpret_cast<xcb_atom_t *>(xcb_get_property_value(reply));
            printf("     . xcb_mime_types (%u)\n", reply->length);
            for (uint32_t i = 0; i < reply->length; i++) {
                printf("       . xcb_mime_type[%d]           : '%s'\n", i, GetAtomName(mime_types[i]));
            }
            for (uint32_t i = 0; i < reply->length; i++) {
                // Case 2-4. we can request real data specified by mime_type
                if (mime_types[i] == GetAtom("TARGETS")) {
                    continue;
                }

                if (!ConvertSelection(window, event->selection, mime_types[i])) {
                    return false;
                }
            }
            free(reply);
        } else {  // Case 2-5
            // Case 2-6
            auto cookie = xcb_get_property(connection, 0, window, event->property, XCB_GET_PROPERTY_TYPE_ANY, 0, 0);
            auto reply = xcb_get_property_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "xcb_get_property_reply() failed\n");
                return false;
            }
            auto type = reply->type;
            auto bytes_after = reply->bytes_after;
            free(reply);

            if (type == GetAtom("INCR")) {
                printf("Data too large and INCR mechanism not implemented\n");
            } else {
                auto cookie = xcb_get_property(connection, 0, window, event->property, XCB_GET_PROPERTY_TYPE_ANY, 0, bytes_after);
                auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                if (!reply) {
                    printf("Failed to read property: %u\n", bytes_after);
                } else {
                    printf("     . xcb_property                 : data %p, len %d, '%s'\n",
                        xcb_get_property_value(reply), xcb_get_property_value_length(reply), GetAtomName(event->property));

                    if (type == GetAtom("STRING") || type == GetAtom("UTF8_STRING")) {
                        std::string data = "";
                        data.assign(reinterpret_cast<char *>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
                        printf("       . '%s'\n", data.c_str());
                    }
                    free(reply);
                }
            }
        }

        xcb_delete_property(connection, window, event->property);
        return true;
    }

    bool CreateWindow(void)
    {
        uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
        std::vector<uint32_t> values = { screen->black_pixel, XCB_EVENT_MASK_BUTTON_PRESS };

        xcb_window_t window = xcb_generate_id(connection);
        auto cookie = xcb_create_window_checked(connection, screen->root_depth, window, screen->root,
            0, 0, 400, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_create_window_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        this->window = window;
        return true;
    }

    bool MapWindow(void)
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
            case XCB_BUTTON_PRESS:
                if (!ProcButtonPress(reinterpret_cast<xcb_button_press_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_SELECTION_CLEAR:
                if (!ProcSelectionClear(reinterpret_cast<xcb_selection_clear_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_SELECTION_REQUEST:
                if (!ProcSelectionRequest(reinterpret_cast<xcb_selection_request_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_SELECTION_NOTIFY:
                if (!ProcSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t *>(event))) {
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
            fprintf(stderr, "xcb_intern_atom_reply() failed '%s'", name);
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
    int                                 screen_num              = 0;
    xcb_connection_t                   *connection              = nullptr;
    const xcb_setup_t                  *setup                   = nullptr;
    xcb_screen_t                       *screen                  = nullptr;
    xcb_window_t                        window                  = XCB_WINDOW_NONE;
    uint8_t                             cut_buffer_idx          = 0;
    std::map<std::string, xcb_atom_t>   atoms                   = {};
    std::map<xcb_atom_t, std::string>   atom_names              = {};
};

int main(int argc, char **argv)
{
    printf("Example xcb_selection\n");

    auto obj = Selection();
    if (!obj.Init() || !obj.ShowCase()) {
        printf("\nFailed..\n");
        return EXIT_FAILURE;
    }
    printf("\nSucceed..\n");
    return EXIT_SUCCESS;
}
