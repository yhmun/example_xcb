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

static constexpr int32_t    INVALID_FD      = -1;
static constexpr ssize_t    INCR_CHUNK_SIZE = 64 * 1024;
static int                  signal_pipe[2]  = {INVALID_FD, INVALID_FD};

class Selection
{
public:
    Selection(void)
    {
    }

    ~Selection(void)
    {
        if (read_fd != INVALID_FD) {
            close(read_fd);
        }

        if (write_fd != INVALID_FD) {
            close(write_fd);
        }

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

        if (!SetWindowAttribute(screen->root)) {
            return false;
        }

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
        printf("\n");
        printf(" * xcb_screen_root                  : 0x%08X\n", screen->root);
        printf(" * xcb_window                       : 0x%08X\n", window);

        /**
         * Case 1. Who is the selection owner?
         *         - xcb_get_selection_owner()
         *
         * Case 2. Transfer data from the other application
         *         1) xcb_convert_selection_checked() with 'TARGETS' and a user property
         *         2) XCB_SELECTION_NOTIFY says that target (mime_type) list are stored in the user property
         *         3) xcb_convert_selection_checked() with one of targets and a user property
         *         4) XCB_SELECTION_NOTIFY says that data is ready
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
        if (!GetSelectionOwner(XCB_ATOM_PRIMARY) ||
            !GetSelectionOwner(XCB_ATOM_SECONDARY) ||
            !GetSelectionOwner(GetAtom("CLIPBOARD"))) {
            return false;
        }

        // Case 2-1. request available targets aka 'mime_types' from the selection owner
        for (auto iter : selections) {
            auto &data = iter.second;
            if (data.owner != window && !ConvertSelection(data.atom, GetAtom("TARGETS"))) {
                return false;
            }
        }

        return RunEventLoop();
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

    bool SetSelectionOwner(xcb_atom_t selection)
    {
        xcb_window_t owner = window;
        auto cookie = xcb_set_selection_owner_checked(connection, owner, selection, XCB_CURRENT_TIME);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_set_selection_owner_checked() failed\n");
            free(error);
            return false;
        }
        auto &data = selections[selection] = {};
        data.atom  = selection;
        data.owner = owner;
        printf(" * xcb_selection_owner              : 0x%08X '%s'\n", owner, GetAtomName(selection));
        xcb_flush(connection);
        return true;
    }

    bool GetSelectionOwner(xcb_atom_t selection)
    {
        auto cookie = xcb_get_selection_owner(connection, selection);
        auto reply = xcb_get_selection_owner_reply(connection, cookie, nullptr);
        if (!reply) {
            fprintf(stderr, "xcb_get_selection_owner_reply() failed\n");
            return false;
        }
        if (!reply->owner) {
            selections.erase(selection);
        } else {
            auto iter = selections.find(selection);
            if (iter == selections.end()) {
                auto &data = selections[selection] = {};
                data.atom  = selection;
                data.owner = reply->owner;
            } else {
                auto &data = selections[selection];
                data.owner = reply->owner;
            }
        }
        printf(" * xcb_selection_owner              : 0x%08X '%s'\n", reply->owner, GetAtomName(selection));
        free(reply);
        return true;
    }

    bool GetNextSelectionTarget(void)
    {
        if (pending_target || incr_property) {
            return true;
        }

        // Case 2-4. we can request real data specified by mime_type
        for (auto &iter : selections) {
            auto &data = iter.second;
            if (!data.targets.empty()) {
                auto target = data.targets.front();
                data.targets.pop();
                pending_target = target;
                return ConvertSelection(data.atom, target);
            }
        }
        return true;
    }

    bool ConvertSelection(xcb_atom_t selection, xcb_atom_t target)
    {
        auto iter = selections.find(selection);
        if (iter == selections.end()) {
            return true;
        }

        auto &data = iter->second;
        if (target == GetAtom("TARGETS")) {
            data.targets = {};
        }

        xcb_window_t requestor = window;
        xcb_atom_t property = XCB_ATOM_CUT_BUFFER0 + (cut_buffer_idx++ % 8);

        auto cookie = xcb_convert_selection_checked(connection, requestor, selection, target, property, XCB_CURRENT_TIME);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_convert_selection_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        printf(" * xcb_convert_selection_checked()  : requestor 0x%08X, selection '%s', target '%s', property '%s'\n",
            requestor, GetAtomName(selection), GetAtomName(target), GetAtomName(property));
        return true;
    }

    bool ProcButtonPress(xcb_button_press_event_t *event)
    {
        printf("   - XCB_BUTTON_PRESS               : seq: %4u, time: %10u, root: 0x%08X, event: 0x%08X, child: 0x%08X, event_x: %d, event_y: %d, state: %u, same_screen: %u\n",
            event->sequence, event->time, event->root, event->event, event->child, event->event_x, event->event_y, event->state, event->same_screen);

        auto selection = GetAtom("CLIPBOARD");
        if (!GetSelectionOwner(selection)) {
            return false;
        }

        auto iter = selections.find(selection);
        if (iter == selections.end() || iter->second.owner != window) {
            // Case 3-1. take clipboard owership
            if (!SetSelectionOwner(selection)) {
                return false;
            }
        }
        return true;
    }

    bool ProcPropertyNotify(xcb_property_notify_event_t *event)
    {
        printf("   - XCB_PROPERTY_NOTIFY            : seq: %4u, time: %10u, window: 0x%08X, state: '%s', atom: '%s'\n",
            event->sequence, event->time, event->window, event->state == XCB_PROPERTY_NEW_VALUE ? "new" : "del", GetAtomName(event->atom));

        if (event->atom != incr_property) {
            return true;
        }

        if (event->state == XCB_PROPERTY_NEW_VALUE) {
            if (event->window == window) {
                auto cookie = xcb_get_property(connection, 1, window, event->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, INT32_MAX / 4);
                auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                if (!reply) {
                    printf("Failed to read property\n");
                } else {
                    auto len = xcb_get_property_value_length(reply);
                    auto val = xcb_get_property_value(reply);
                    printf("       . length: %d\n", len);

                    if (len) {
                        if (write_fd != INVALID_FD) {
                            write(write_fd, val, len);
                        }
                    } else {
                        if (write_fd != INVALID_FD) {
                            close(write_fd);
                            write_fd = INVALID_FD;
                        }
                        incr_property = XCB_ATOM_NONE;
                        GetNextSelectionTarget();
                    }
                    free(reply);
                }
            }
        } else {
            if (event->window != window) {
                auto bytes = read(read_fd, read_buf, INCR_CHUNK_SIZE);
                if (bytes < 0) {
                    fprintf(stderr, "read() failed (err: '%s')\n", strerror(errno));
                    return false;
                }
                incr_bytes += bytes;
                printf("       . bytes : %u\n", incr_bytes);
                printf("       . chunk : %lu\n", bytes);

                auto cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                    event->window, event->atom, incr_target, 8, bytes, &read_buf);
                auto error = xcb_request_check(connection, cookie);
                if (error) {
                    fprintf(stderr, "xcb_change_property_checked() failed (err: %d)\n", error->error_code);
                    free(error);
                    return true;
                }
                if (!bytes) {
                    incr_property = XCB_ATOM_NONE;
                    incr_target = XCB_ATOM_NONE;
                    incr_bytes = 0;
                }
            }
        }
        return true;
    }

    bool ProcSelectionClear(xcb_selection_clear_event_t *event)
    {
        // Case 4
        printf("   - XCB_SELECTION_CLEAR            : seq: %4u, time: %10u, owner: 0x%08X, selection: '%s'\n",
            event->sequence, event->time, event->owner, GetAtomName(event->selection));

        if (event->owner != window) {
            return true;
        }

        // retrive who has ownership
        if (!GetSelectionOwner(event->selection)) {
            return false;
        }

        // request mime_types because we lost ownership
        if (!ConvertSelection(event->selection, GetAtom("TARGETS"))) {
            return false;
        }
        return true;
    }

    bool SendSelectionResponse(xcb_selection_request_event_t *event)
    {
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

        union {
            xcb_selection_notify_event_t    event;
            char                            data[32];
        } response = {
            .event = notify
        };

        auto cookie = xcb_send_event_checked(connection, 0, event->requestor, XCB_EVENT_MASK_NO_EVENT, response.data);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_send_event_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        printf("       . responsed\n");
        xcb_flush(connection);
        return true;
    }

    bool ProcSelectionRequest(xcb_selection_request_event_t *event)
    {
        printf("   - XCB_SELECTION_REQUEST          : seq: %4u, time: %10u, owner: 0x%08X, requestor: 0x%08X, selection: '%s', target: '%s', property: '%s'\n",
            event->sequence, event->time, event->owner, event->requestor, GetAtomName(event->selection), GetAtomName(event->target), GetAtomName(event->property));

        if (event->requestor == window) {
            return true;
        }

        xcb_atom_t image_atom = XCB_ATOM_NONE;
        image_atom = GetAtom("image/png");
        //image_atom = GetAtom("image/jpeg");

        xcb_void_cookie_t cookie = {};
        if (event->target == GetAtom("TARGETS")) {
            std::vector<xcb_atom_t> targets = {};

            if (image_atom == GetAtom("image/png")) {
                read_fd = open("test.png", O_RDONLY | O_CREAT, S_IRUSR | S_IWUSR);
            } else if (image_atom == GetAtom("image/jpeg")) {
                read_fd = open("test.jpg", O_RDONLY | O_CREAT, S_IRUSR | S_IWUSR);
            } else {
                read_fd = INVALID_FD;
            }
            if (read_fd != INVALID_FD) {
                read_fd_len = lseek(read_fd, 0, SEEK_END);
                targets.push_back(image_atom);
            } else {
                targets.push_back(XCB_ATOM_STRING);
                targets.push_back(GetAtom("UTF8_STRING"));
            }
            targets.push_back(event->target);
            targets.push_back(GetAtom("TIMESTAMP"));

            for (auto target : targets) {
                printf("       . target: '%s'\n", GetAtomName(target));
            }

            cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                event->requestor, event->property, XCB_ATOM_ATOM, 8 * sizeof(xcb_atom_t), targets.size(), targets.data());
        } else if (event->target == GetAtom("TIMESTAMP")) {
            xcb_timestamp_t cur = XCB_CURRENT_TIME;
            cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                event->requestor, event->property, XCB_ATOM_INTEGER, 8 * sizeof(xcb_timestamp_t), 1, &cur);
        } else if (event->target == XCB_ATOM_STRING || event->target == GetAtom("UTF8_STRING")) {
            static char text[] = "Copy & Paste test";
            cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                event->requestor, event->property, event->target, 8, strlen(text), text);
        } else if (image_atom && image_atom == event->target) {
            if (read_fd == INVALID_FD) {
                event->property = XCB_ATOM_NONE;
            } else {
                lseek(read_fd, 0, SEEK_SET);
                if (read_fd_len < INCR_CHUNK_SIZE) {
                    auto bytes = read(read_fd, read_buf, INCR_CHUNK_SIZE);
                    if (bytes < 0) {
                        event->property = XCB_ATOM_NONE;
                        fprintf(stderr, "read() failed (err: '%s')\n", strerror(errno));
                    } else {
                        cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                            event->requestor, event->property, event->target, 8, bytes, &read_buf);
                    }
                } else if (!SetWindowAttribute(event->requestor)) {
                    event->property = XCB_ATOM_NONE;
                } else {
                    incr_property = event->property;
                    incr_target = event->target;
                    incr_bytes = 0;
                    cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                        event->requestor, event->property, GetAtom("INCR"), 32, 1, &read_fd_len);
                    printf("       . 'INCR': %u\n", read_fd_len);
                }
            }
        }

        if (event->property) {
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                event->property = XCB_ATOM_NONE;
                fprintf(stderr, "xcb_change_property_checked() failed (err: %d)\n", error->error_code);
                free(error);
            }
        }
        return SendSelectionResponse(event);
    }

    bool ProcSelectionNotify(xcb_selection_notify_event_t *event)
    {
        printf("   - XCB_SELECTION_NOTIFY           : seq: %4u, time: %10u, requestor: 0x%08X, selection: '%s', target: '%s', property: '%s'\n",
            event->sequence, event->time, event->requestor, GetAtomName(event->selection), GetAtomName(event->target), event->property ? GetAtomName(event->property) : "(null)");

        if (event->requestor != window) {
            return true;
        }

        auto iter = selections.find(event->selection);
        if (iter == selections.end()) {
            return true;
        }

        if (event->target == pending_target) {
            pending_target = XCB_ATOM_NONE;
        }

        if (event->property) {
            auto cookie = xcb_get_property(connection, 1, event->requestor, event->property, XCB_GET_PROPERTY_TYPE_ANY, 0, INT32_MAX / 4);
            auto reply = xcb_get_property_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "Failed to read property\n");
                return false;
            }

            auto &data = iter->second;
            auto value = xcb_get_property_value(reply);
            if (event->target == GetAtom("TARGETS")) {
                // Case 2-2
                auto atoms = reinterpret_cast<xcb_atom_t *>(value);
                for (uint32_t i = 0; i < reply->length; i++) {
                    auto atom = atoms[i];
                    printf("       . target: '%s'\n", GetAtomName(atom));
                    if (event->target != atom) {
                        data.targets.push(atom);
                    }
                }
            } else {
                // Case 2-4
                auto len = xcb_get_property_value_length(reply);
                printf("       . type  : '%s'\n", GetAtomName(reply->type));
                printf("       . length: %d\n", len);

                if (write_fd == INVALID_FD) {
                    if (event->target == GetAtom("image/png")) {
                        write_fd = open("test.png", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
                    } else if (event->target == GetAtom("image/bmp")) {
                        write_fd = open("test.bmp", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
                    } else if (event->target == GetAtom("image/jpeg")) {
                        write_fd = open("test.jpg", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
                    }
                }

                if (reply->type == GetAtom("INCR")) {
                    if (len == 4) {
                        auto bytes = *reinterpret_cast<uint32_t *>(value);
                        printf("       . 'INCR': %u\n", bytes);
                        incr_property = event->property;
                    }
                } else {
                    if (reply->type == XCB_ATOM_INTEGER) {
                        uint32_t num = *reinterpret_cast<uint32_t *>(value);
                        printf("       . number: %u\n", num);
                    } else if (reply->type == XCB_ATOM_STRING ||
                               reply->type == GetAtom("TEXT") ||
                               reply->type == GetAtom("UTF8_STRING") ||
                               reply->type == GetAtom("text/plain") ||
                               reply->type == GetAtom("text/html")) {
                        std::string str = "";
                        str.assign(reinterpret_cast<char *>(value), std::min(len, 1024));
                        printf("       . string: '%s'\n", str.c_str());
                    }
                    if (write_fd != INVALID_FD) {
                        write(write_fd, value, len);
                        close(write_fd);
                        write_fd = INVALID_FD;
                    }
                }
            }
            free(reply);
        }

        GetNextSelectionTarget();
        return true;
    }

    bool CreateWindow(void)
    {
        uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
        std::vector<uint32_t> values = { screen->black_pixel, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS};

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
            case XCB_PROPERTY_NOTIFY:
                if (!ProcPropertyNotify(reinterpret_cast<xcb_property_notify_event_t *>(event))) {
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
    xcb_window_t                                window                      = XCB_WINDOW_NONE;
    uint8_t                                     cut_buffer_idx              = 0;

    struct selection_t
    {
        xcb_atom_t                              atom                        = XCB_ATOM_NONE;
        xcb_window_t                            owner                       = XCB_WINDOW_NONE;
        std::queue<xcb_atom_t>                  targets                     = {};
    };
    std::map<xcb_atom_t, selection_t>           selections                  = {};
    xcb_atom_t                                  pending_target              = XCB_ATOM_NONE;
    xcb_atom_t                                  incr_property               = XCB_ATOM_NONE;
    xcb_atom_t                                  incr_target                 = XCB_ATOM_NONE;
    uint32_t                                    incr_bytes                  = 0;
    int32_t                                     write_fd                    = INVALID_FD;
    int32_t                                     read_fd                     = INVALID_FD;
    uint32_t                                    read_fd_len                 = 0;
    uint8_t                                     read_buf[INCR_CHUNK_SIZE]   = {};

    std::map<std::string, xcb_atom_t>           atoms                       = {};
    std::map<xcb_atom_t, std::string>           atom_names                  = {};
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
