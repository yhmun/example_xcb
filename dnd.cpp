#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <list>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <stack>
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
    struct window_t
    {
        xcb_window_t                id              = XCB_WINDOW_NONE;
        xcb_rectangle_t             rect            = {};
        xcb_query_tree_cookie_t     tree_cookie     = {};
        xcb_get_geometry_cookie_t   geometry_cookie = {};
        std::list<window_t *>       children        = {};
    };

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

        if (gc != XCB_NONE) {
            auto cookie = xcb_free_gc_checked(connection, gc);
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_free_gc_checked() failed\n");
                free(error);
            }
        }

        if (win != XCB_WINDOW_NONE) {
            auto cookie = xcb_destroy_window_checked(connection, win);
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_destroy_window_checked() failed\n");
                free(error);
            }
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
        win = CreateWindow(400, 400);
        if (win == XCB_WINDOW_NONE) {
            return false;
        }

        gc = CreateGContext();
        if (gc == XCB_NONE) {
            return false;
        }

        if (!MapWindow(win)) {
            return false;
        }

        if (!ChangeProperty(win, GetAtom("XdndAware"), XCB_ATOM_ATOM, 1, &XDND_PROTOCOL_VERSION)) {
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
        std::vector<uint32_t> values = {XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE, 0};
        auto cookie = xcb_change_window_attributes_checked(connection, window, XCB_CW_EVENT_MASK, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_change_window_attributes() failed\n");
            return false;
        }
        printf(" * xcb_change_window_attributes     : 0x%08X\n", window);
        return true;
    }

    bool ContainPosition(const xcb_rectangle_t &rect, int16_t x, int16_t y)
    {
        return x >= rect.x && x <= rect.x + rect.width &&
               y >= rect.y && y <= rect.y + rect.height;
    }

    bool ClearWindowGeometryTree(window_t *&root)
    {
        if (!root) {
            return true;
        }

        std::queue<window_t *> que = {};
        que.push(root);
        while (!que.empty()) {
            auto node = que.front();
            que.pop();

            for (auto child : node->children) {
                que.push(child);
            }
            delete node;
        }
        root = nullptr;
        return true;
    }

    bool QueryWindowGeometryTree(window_t *&root)
    {
        if (!ClearWindowGeometryTree(root)) {
            return false;
        }

        root = new window_t;
        root->id = screen->root;

        std::list<window_t *> windows = { root };
        std::list<window_t *> next_windows = {};

        bool rc = true;
        while (rc && !windows.empty()) {
            for (auto win : windows) {
                win->tree_cookie = xcb_query_tree(connection, win->id);
            }

            for (auto win : windows) {
                if (rc) {
                    auto reply = xcb_query_tree_reply(connection, win->tree_cookie, nullptr);
                    if (reply) {
                        auto children = xcb_query_tree_children(reply);
                        if (reply->children_len) {
                            for (auto i = reply->children_len - 1; i >= 0; i--) {
                                auto child = new window_t;
                                child->id = children[i];
                                win->children.push_back(child);
                                next_windows.push_back(child);
                            }
                        }
                        free(reply);
                    } else {
                        fprintf(stderr, "xcb_query_tree_reply() failed\n");
                        rc = false;
                    }
                } else {
                    xcb_discard_reply(connection, win->tree_cookie.sequence);
                }
            }

            if (rc) {
                windows = std::move(next_windows);
            }
        }

        if (rc) {
            std::queue<window_t *> que = {};
            que.push(root);
            while (!que.empty()) {
                auto win = que.front();
                win->geometry_cookie = xcb_get_geometry(connection, win->id);
                que.pop();

                for (auto child : win->children) {
                    que.push(child);
                }
            }

            que.push(root);
            while (!que.empty()) {
                auto win = que.front();
                if (rc) {
                    auto reply = xcb_get_geometry_reply(connection, win->geometry_cookie, nullptr);
                    if (reply) {
                        win->rect.x += reply->x;
                        win->rect.y += reply->y;
                        win->rect.width = reply->width;
                        win->rect.height = reply->height;
                        printf(" - win: 0x%08x, %d, %d, %u, %u\n", win->id, win->rect.x, win->rect.y, win->rect.width, win->rect.height);
                        free(reply);
                    } else {
                        fprintf(stderr, "xcb_get_geometry_reply() failed\n");
                        rc = false;
                    }
                } else {
                    xcb_discard_reply(connection, win->geometry_cookie.sequence);
                }
                que.pop();

                for (auto child : win->children) {
                    child->rect.x = win->rect.x;
                    child->rect.y = win->rect.y;
                    que.push(child);
                }
            }
        }
        
        if (!rc) {
            ClearWindowGeometryTree(root);
        }
        return rc;
    }

    xcb_window_t FindWindowOverPointer(window_t *window, int16_t pointer_x, int16_t pointer_y)
    {
        xcb_window_t win = XCB_WINDOW_NONE;
        if (!window) {
            return win;
        }

        for (auto child : window->children) {
            win = FindWindowOverPointer(child, pointer_x, pointer_y);
            if (win != XCB_WINDOW_NONE) {
                return win;
            }
        }

        if (ContainPosition(window->rect, pointer_x, pointer_y)) {
            win = window->id;
        }
        return win;
    }

    bool UnGrabPointer(void)
    {
        if (send.grabbed) {
            auto cookie = xcb_ungrab_pointer(connection, XCB_CURRENT_TIME);
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_ungrab_pointer() failed (err: %d)", error->error_code);
                free(error);
                return false;
            }
            send.grabbed = false;
        }
        return true;
    }

    void ClearReceive(void)
    {
        receive.dst_win     = XCB_WINDOW_NONE;
        receive.src_win     = XCB_WINDOW_NONE;
        receive.version     = 0;
        receive.flags       = 0;
        receive.timestamp   = 0;
        receive.dst_x       = 0;
        receive.dst_y       = 0;
        receive.root_x      = 0;
        receive.root_y      = 0;
        receive.action      = XCB_ATOM_NONE;
        receive.types.clear();
    }

    bool SendReceiveStatus(bool accept, bool want_position = true)
    {
        xcb_client_message_event_t event = {
            .response_type  = XCB_CLIENT_MESSAGE,
            .format         = 32,
            .sequence       = 0,
            .window         = receive.src_win,
            .type           = GetAtom("XdndStatus"),
            .data           = { .data32 = { receive.dst_win, 0, 0, 0, 0 }},
        };

        // Bit 0 is set if the current target will accept the drop
        if (accept) {
            event.data.data32[1] |= 0x1;
        }

        // Bit 1 is set if the target wants XdndPosition messages while the mouse moves inside the rectangle in data.l[2,3]
        if (want_position) {
            event.data.data32[1] |= 0x2;
        }

        // a rectangle in root coordinates that means "don't send another XdndPosition message until the mouse moves out of here"
        // an empty rectangle means "send another message when the mouse moves"
        event.data.data32[2] = (static_cast<uint32_t>(receive.root_x) << 16) | receive.root_y;
        event.data.data32[3] = (static_cast<uint32_t>(win_w) << 16) | win_h;

        // the action accepted by the target
        if (accept && receive.version >= 2) {
            event.data.data32[4] = receive.action;
        }

        auto cookie = xcb_send_event_checked(connection, 0, receive.src_win, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&event));
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_send_event_checked() failed 'XdndStatus' (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        printf("   - XdndStatus                     : window: 0x%08x, accepted: %s, want_position: %s, rect_x: %u, rect_y: %u, rect_w: %u, rect_h: %u",
            receive.src_win, accept ? "yes" : "no", want_position ? "yes" : "no", event.data.data32[2] >> 16, event.data.data32[2] & 0xffff, event.data.data32[3] >> 16, event.data.data32[3] & 0xffff);
        if (event.data.data32[4]) {
            printf(", action: %s", GetAtomName(event.data.data32[4]));
        }
        printf("\n");
        return true;
    }

    bool SendReceiveFinish(bool accept)
    {
        if (receive.version >= 2) {
            xcb_client_message_event_t event = {
                .response_type  = XCB_CLIENT_MESSAGE,
                .format         = 32,
                .sequence       = 0,
                .window         = receive.src_win,
                .type           = GetAtom("XdndFinished"),
                .data           = { .data32 = { receive.dst_win, 0, 0, 0, 0 }},
            };

            // Bit 0 is set if the current target accepted the drop and successfully performed the accepted drop action
            if (accept) {
                event.data.data32[1] |= 0x1;
            }

            // the action performed by the target
            if (accept && receive.version >= 2) {
                event.data.data32[2] = receive.action;
            }

            auto cookie = xcb_send_event_checked(connection, 0, receive.src_win, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&event));
            auto error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "xcb_send_event_checked() failed 'XdndFinished' (err: %d)\n", error->error_code);
                free(error);
                ClearReceive();
                return false;
            }
            printf("   - XdndFinished                   : window: 0x%08x, accepted: %s", receive.src_win, accept ? "yes" : "no");
            if (event.data.data32[2]) {
                printf(", action: %s", GetAtomName(event.data.data32[2]));
            }
            printf("\n");
        }
        ClearReceive();
        return true;
    }

    bool ProcClientMessage(xcb_client_message_event_t *event)
    {
        printf("   - XCB_CLIENT_MESSAGE             : seq: %4u, window: 0x%08X, type: %s", event->sequence, event->window, GetAtomName(event->type));

        if (event->type == GetAtom("XdndEnter")) {
            bool has_list = event->data.data32[1] & 1;
            receive.dst_win = event->window;
            receive.src_win = event->data.data32[0];
            receive.version = event->data.data32[1] >> 24;
            if (has_list) {
                auto cookie = xcb_get_property(connection, 0, receive.src_win, GetAtom("XdndTypeList"), XCB_ATOM_ANY, 0, 2048);
                auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                if (!reply) {
                    fprintf(stderr, "xcb_get_property_reply() failed 'XdndTypeList'\n");
                    return false;
                }
                auto types = reinterpret_cast<xcb_atom_t *>(xcb_get_property_value(reply));
                for (uint32_t i = 0; i < reply->length; i++) {
                    receive.types.insert(types[i]);
                }
                free(reply);
            } else {
                for (auto i = 2; i < 5; i++) {
                    auto type = event->data.data32[i];
                    if (type) {
                        receive.types.insert(type);
                    }
                }
            }
            printf(", source: 0x%08X, version: %u, has_list: %s\n", receive.src_win, receive.version, has_list ? "yes" : "no");
            for (auto type : receive.types) {
                printf("     - type: %s\n", GetAtomName(type));
            }
        } else if (event->type == GetAtom("XdndPosition")) {
            receive.src_win = event->data.data32[0];
            receive.flags = event->data.data32[1]; // reserved for future use
            printf(", source: 0x%08X, flags: 0x%08X", receive.src_win, receive.flags);
            if (receive.version >= 1) {
                receive.timestamp = event->data.data32[3];
                printf(", timestamp: %u", receive.timestamp);
            }
            if (receive.version >= 2) {
                receive.action = event->data.data32[4];
                printf(", action: %s", GetAtomName(receive.action));
            }
            receive.root_x = 0xffff & (event->data.data32[2] >> 16);
            receive.root_y = 0xffff & event->data.data32[2];
            printf(", root_x: %u, root_y: %u", receive.root_x, receive.root_y);

            auto cookie = xcb_translate_coordinates(connection, screen->root, event->window, receive.root_x, receive.root_y);
            auto reply = xcb_translate_coordinates_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "\nxcb_translate_coordinates_reply() failed\n");
                SendReceiveStatus(false);
                return false;
            }
            receive.dst_x = reply->dst_x;
            receive.dst_y = reply->dst_y;
            printf(", dst_x: %u, dst_y: %u\n", receive.dst_x, receive.dst_y);
            free(reply);

            bool accept = ContainPosition(rect, receive.dst_x, receive.dst_y);
            SendReceiveStatus(accept);
        } else if (event->type == GetAtom("XdndLeave")) {
            receive.src_win = event->data.data32[0];
            receive.flags = event->data.data32[1]; // reserved for future use
            printf(", source: 0x%08X, flags: 0x%08X\n", receive.src_win, receive.flags);
            ClearReceive();
        } else if (event->type == GetAtom("XdndDrop")) {
            xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
            receive.src_win = event->data.data32[0];
            receive.flags = event->data.data32[1]; // reserved for future use
            printf(", source: 0x%08X, flags: 0x%08X", receive.src_win, receive.flags);
            if (receive.version >= 1) {
                timestamp = event->data.data32[2];
                receive.timestamp = event->data.data32[2];
                printf(", timestamp: %u", receive.timestamp);
            }
            printf("\n");

            for (auto type : receive.types) {
                xcb_atom_t property = XCB_ATOM_CUT_BUFFER0 + (cut_buffer_idx++ % 8);
                auto cookie = xcb_convert_selection_checked(connection, event->window, GetAtom("XdndSelection"), type, property, timestamp);
                auto error = xcb_request_check(connection, cookie);
                if (error) {
                    fprintf(stderr, "xcb_convert_selection_checked() failed (err: %d)\n", error->error_code);
                    free(error);
                    SendReceiveFinish(false);
                    return false;
                }
                receive.targets.insert(type);
            }
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
            if (receive.src_win == XCB_WINDOW_NONE) {
                printf("\n");
                return true;
            }

            auto cookie = xcb_get_property(connection, true, event->requestor, event->property, XCB_ATOM_ANY, 0, 2048);
            auto reply = xcb_get_property_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "\nxcb_get_property_reply() failed\n");
                SendReceiveFinish(false);
                return false;
            } else {
                auto len = xcb_get_property_value_length(reply);
                printf(", len: %d", len);
                if (event->target == GetAtom("text/plain") ||
                    event->target == GetAtom("text/uri-list") ||
                    event->target == GetAtom("application/x-kde4-urilist")) {
                    std::string text = "";
                    text.assign(reinterpret_cast<char *>(xcb_get_property_value(reply)), std::min(len, 128));
                    if (text.back() == '\n') {
                        text.pop_back();
                    }
                    if (text.back() == '\r') {
                        text.pop_back();
                    }
                    printf(", value: '%s'", text.c_str());
                }
                free(reply);
            }

            printf("\n");
            receive.targets.erase(event->target);
            if (receive.targets.empty()) {
                SendReceiveFinish(true);
            }
        } else {
            printf("\n");
        }
        return true;
    }

    bool ProcSelectionClear(xcb_selection_clear_event_t *event)
    {
        printf("   - XCB_SELECTION_CLEAR            : seq: %4u, time: %10u, owner: 0x%08X, selection: '%s'\n", event->sequence, event->time, event->owner, GetAtomName(event->selection));

        if (event->owner == win) {
            send.selection_owned = false;
        }
        return true;
    }

    bool ProcConfigureNotify(xcb_configure_notify_event_t *event)
    {
        printf("   - XCB_CONFIGURE_NOTIFY           : seq: %4u, event: 0x%08X, window: 0x%08X, above_sibling: 0x%08x, x: %d, y: %d, width: %u, height: %u, border_width: %u, override_redirect: %u\n", event->sequence, event->event, event->window, event->above_sibling, event->x, event->y, event->width, event->height, event->border_width, event->override_redirect);

        if (event->window == win) {
            if (event->width != win_w || event->height != win_h) {
                rect.x = event->width / 4;
                rect.y = event->height / 4;
                rect.width = event->width / 2;
                rect.height = event->height / 2;
                if (!ClearArea() || !FillRectangle()) {
                    return false;
                }
            }
            win_x = event->x;
            win_y = event->y;
            win_w = event->width;
            win_h = event->height;
        }
        return true;
    }

    bool ProcDestroyNotify(xcb_destroy_notify_event_t *event)
    {
        printf("   - XCB_DESTROY_NOTIFY             : seq: %4u, event: 0x%08X, root: 0x%08X\n", event->sequence, event->event, event->window);
        return true;
    }

    bool ProcButtonPress(xcb_button_press_event_t *event)
    {
        printf("   - XCB_BUTTON_PRESS               : seq: %4u, time: %10u, root: 0x%08X, event: 0x%08X, child: 0x%08X, root_x: %d, root_y: %d, event_x: %d, event_y: %d, state: %u, same_screen: %u",
            event->sequence, event->time, event->root, event->event, event->child, event->root_x, event->root_y, event->event_x, event->event_y, event->state, event->same_screen);

        if (event->detail == 1 && ContainPosition(rect, event->event_x, event->event_y)) {
            auto cookie = xcb_grab_pointer(connection, 0, win, XCB_EVENT_MASK_BUTTON_1_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE, XCB_CURRENT_TIME);
            auto reply = xcb_grab_pointer_reply(connection, cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "\nxcb_grab_pointer_reply() failed\n");
                return false;
            }
            auto status = reply->status;
            printf(", grab_status: %d", status);
            free(reply);
            if (status == XCB_GRAB_STATUS_SUCCESS || status == XCB_GRAB_STATUS_ALREADY_GRABBED) {
                send.grabbed = true;
                if (!send.selection_owned) {
                    auto cookie = xcb_set_selection_owner_checked(connection, win, GetAtom("XdndSelection"), XCB_CURRENT_TIME);
                    auto error = xcb_request_check(connection, cookie);
                    if (error) {
                        fprintf(stderr, "\nxcb_set_selection_owner_checked() failed 'XdndSelection' (err: %d)\n", error->error_code);
                        free(error);
                        return false;
                    }
                    printf(", XdndSelection");
                    send.selection_owned = true;
                }
            }
        }
        printf("\n");
        return true;
    }

    bool ProcButtonRelease(xcb_button_press_event_t *event)
    {
        printf("   - XCB_BUTTON_RELEASE             : seq: %4u, time: %10u, root: 0x%08X, event: 0x%08X, child: 0x%08X, root_x: %d, root_y: %d, event_x: %d, event_y: %d, state: %u, same_screen: %u",
            event->sequence, event->time, event->root, event->event, event->child, event->root_x, event->root_y, event->event_x, event->event_y, event->state, event->same_screen);

        if (event->detail == 1) {
            if (send.dst_aware) {
                printf(", XdndDrop: 0x%08x, XdndVersion: %u", send.dst_win, send.dst_version);
            } else if (send.dst_win != win) {
                printf(", pointer_target: 0x%08x", send.dst_win);
            } else {
                printf(", pointer_target: same");
            }
            printf("\n");

            if (!UnGrabPointer()) {
                return false;
            }
        } else {
            printf("\n");
        }
        return true;
    }

    bool ProcMotionNotify(xcb_motion_notify_event_t *event)
    {
        printf("   - XCB_MOTION_NOTIFY              : seq: %4u, time: %10u, root: 0x%08X, event: 0x%08X, child: 0x%08X, root_x: %d, root_y: %d, event_x: %d, event_y: %d, detail: %u, state: %u, same_screen: %u",
            event->sequence, event->time, event->root, event->event, event->child, event->root_x, event->root_y, event->event_x, event->event_y, event->detail, event->state, event->same_screen);

        if (event->event == win && send.grabbed) {
            if (send.query_required) {
                if (!QueryWindowGeometryTree(send.root)) {
                    return false;
                }
                send.query_required = false;
            }

            auto dst_win = FindWindowOverPointer(send.root, event->root_x, event->root_y);
            if (dst_win != XCB_WINDOW_NONE && dst_win != screen->root && dst_win != win) {
                if (send.dst_win != dst_win) {
                    auto cookie = xcb_get_property(connection, 0, dst_win, GetAtom("XdndAware"), XCB_ATOM_ANY, 0, 2048);
                    auto reply = xcb_get_property_reply(connection, cookie, nullptr);
                    if (!reply) {
                        fprintf(stderr, "\nxcb_get_property_reply() failed 'XdndAware'\n");
                        return false;
                    }
                    if (xcb_get_property_value_length(reply)) {
                        send.dst_aware = true;
                        send.dst_version = *reinterpret_cast<uint32_t *>(xcb_get_property_value(reply));
                        printf(", XdndEnter: 0x%08x", dst_win);
                    } else {
                        send.dst_aware = false;
                        send.dst_version = 0;
                    }
                    send.dst_win = dst_win;
                    free(reply);
                }
                if (send.dst_aware) {
                    printf(", XdndTarget: 0x%08x, XdndVersion: %u", dst_win, send.dst_version);
                } else {
                    printf(", pointer_target: 0x%08x", dst_win);
                }
            } else {
                if (send.dst_aware) {
                    printf(", XdndLeave: 0x%08x", send.dst_win);
                } else if (dst_win == win) {
                    printf(", pointer_target: same");
                }
                send.dst_win = XCB_WINDOW_NONE;
                send.dst_aware = false;
                send.dst_version = 0;
            }
        }
        printf("\n");
        return true;
    }

    bool ProcFocusOut(xcb_focus_out_event_t *event)
    {
        printf("   - XCB_FOCUS_OUT                  : seq: %4u, event: 0x%08X, detail: %u, mode: %uu\n", event->sequence, event->event, event->detail, event->mode);

        if (win == event->event) {
            send.query_required = true;
        }
        return true;
    }

    bool ProcPropertyNotify(xcb_property_notify_event_t *event)
    {
        printf("   - XCB_PROPERTY_NOTIFY            : seq: %4u, time: %10u, window: 0x%08X, state: '%s', atom: '%s'\n",
            event->sequence, event->time, event->window, event->state == XCB_PROPERTY_NEW_VALUE ? "new" : "del", GetAtomName(event->atom));

        return true;
    }

    xcb_window_t CreateWindow(uint16_t width, uint16_t height)
    {
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        std::vector<uint32_t> values = { screen->black_pixel, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE};

        xcb_window_t window = xcb_generate_id(connection);
        auto cookie = xcb_create_window_checked(connection, screen->root_depth, window, screen->root,
            0, 0, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_create_window_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return XCB_WINDOW_NONE;
        }
        return window;
    }

    xcb_gcontext_t CreateGContext(void)
    {
        uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
        std::vector<uint32_t> values = { screen->white_pixel, screen->black_pixel, 0 };

        xcb_gcontext_t gc = xcb_generate_id(connection);
        auto cookie = xcb_create_gc_checked(connection, gc, screen->root, mask, values.data());
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_create_gc_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return XCB_NONE;
        }
        return gc;
    }

    bool ClearArea(void)
    {
        auto cookie = xcb_clear_area_checked(connection, 1, win, 0, 0, win_w, win_h);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_clear_area_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        return true;
    }

    bool FillRectangle(void)
    {
        auto cookie = xcb_poly_fill_rectangle_checked(connection, win, gc, 1, &rect);
        auto error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "xcb_poly_fill_rectangle_checked() failed (err: %d)\n", error->error_code);
            free(error);
            return false;
        }
        return true;
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
            case XCB_SELECTION_CLEAR:
                if (!ProcSelectionClear(reinterpret_cast<xcb_selection_clear_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_CONFIGURE_NOTIFY:
                if (!ProcConfigureNotify(reinterpret_cast<xcb_configure_notify_event_t *>(event))) {
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
            case XCB_BUTTON_RELEASE:
                if (!ProcButtonRelease(reinterpret_cast<xcb_button_release_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_MOTION_NOTIFY:
                if (!ProcMotionNotify(reinterpret_cast<xcb_motion_notify_event_t *>(event))) {
                    return false;
                }
                break;
            case XCB_FOCUS_OUT:
                if (!ProcFocusOut(reinterpret_cast<xcb_focus_out_event_t *>(event))) {
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
        if (atom == XCB_ATOM_NONE) {
            return "XCB_ATOM_NONE";
        }

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
    xcb_window_t                                win                         = XCB_WINDOW_NONE;
    int16_t                                     win_x                       = 0;
    int16_t                                     win_y                       = 0;
    uint16_t                                    win_w                       = 0;
    uint16_t                                    win_h                       = 0;
    xcb_gcontext_t                              gc                          = XCB_NONE;
    xcb_rectangle_t                             rect                        = {};
    std::map<std::string, xcb_atom_t>           atoms                       = {};
    std::map<xcb_atom_t, std::string>           atom_names                  = {};

    struct
    {
        bool                                    grabbed                     = false;
        bool                                    selection_owned             = false;
        bool                                    query_required              = true;
        bool                                    dst_aware                   = false;
        uint32_t                                dst_version                 = 0;
        xcb_window_t                            dst_win                     = XCB_WINDOW_NONE;
        window_t                               *root                        = nullptr;
    } send;

    struct
    {
        xcb_window_t                            dst_win                     = XCB_WINDOW_NONE;
        xcb_window_t                            src_win                     = XCB_WINDOW_NONE;
        uint32_t                                version                     = 0;
        uint32_t                                flags                       = 0;
        uint32_t                                timestamp                   = 0;
        int16_t                                 dst_x                       = 0;
        int16_t                                 dst_y                       = 0;
        int16_t                                 root_x                      = 0;
        int16_t                                 root_y                      = 0;
        xcb_atom_t                              action                      = XCB_ATOM_NONE;
        std::set<xcb_atom_t>                    types                       = {};
        std::set<xcb_atom_t>                    targets                     = {};
    } receive;
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
