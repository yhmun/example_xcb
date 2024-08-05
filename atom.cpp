#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <xcb/xcb.h>

class Atom
{
public:
    Atom(void)
    {    
    }

    ~Atom(void)
    {
        if (connection) {
            xcb_disconnect(connection);
        }
    }

    bool ShowCase(void)
    {
        connection = xcb_connect(nullptr, &screen_num);
        if (!connection) {
            fprintf(stderr, "xcb_connect() failed\n");
            return false;
        }

        if (!PreCache()) {
            return false;
        }

        printf("\n* Runtime list\n");
        printf("  - xcb_atom (%3u): '%s'\n", XCB_ATOM_WM_TRANSIENT_FOR, GetName(XCB_ATOM_WM_TRANSIENT_FOR));
        printf("  - xcb_atom (%3u): '%s'\n", 294, GetName(static_cast<xcb_atom_t>(294)));
        printf("  - xcb_atom (%3u): '%s'\n", Get("_NET_WM_WINDOW_TYPE_NORMAL"), "_NET_WM_WINDOW_TYPE_NORMAL");
        printf("  - xcb_atom (%3u): '%s'\n", Get("_NET_WM_WINDOW_TYPE_MENU"), "_NET_WM_WINDOW_TYPE_MENU");

        return true;
    }

    bool PreCache(void)
    {
        struct item_t {
            const char                     *name    = nullptr;
            xcb_atom_t                      atom    = XCB_ATOM_NONE;
            xcb_intern_atom_cookie_t        cookie  = {};
        };

        std::vector<item_t> pre_assign_item = {
            #define PRE_ASSIGN(S)           {#S, XCB_ATOM_##S}
            PRE_ASSIGN( PRIMARY             ),
            PRE_ASSIGN( SECONDARY           ),
            PRE_ASSIGN( ARC                 ),
            PRE_ASSIGN( ATOM                ),
            PRE_ASSIGN( BITMAP              ),
            PRE_ASSIGN( CARDINAL            ),
            PRE_ASSIGN( COLORMAP            ),
            PRE_ASSIGN( CURSOR              ),
            PRE_ASSIGN( CUT_BUFFER0         ),
            PRE_ASSIGN( CUT_BUFFER1         ),
            PRE_ASSIGN( CUT_BUFFER2         ),
            PRE_ASSIGN( CUT_BUFFER3         ),
            PRE_ASSIGN( CUT_BUFFER4         ),
            PRE_ASSIGN( CUT_BUFFER5         ),
            PRE_ASSIGN( CUT_BUFFER6         ),
            PRE_ASSIGN( CUT_BUFFER7         ),
            PRE_ASSIGN( DRAWABLE            ),
            PRE_ASSIGN( FONT                ),
            PRE_ASSIGN( INTEGER             ),
            PRE_ASSIGN( PIXMAP              ),
            PRE_ASSIGN( POINT               ),
            PRE_ASSIGN( RECTANGLE           ),
            PRE_ASSIGN( RESOURCE_MANAGER    ),
            PRE_ASSIGN( RGB_COLOR_MAP       ),
            PRE_ASSIGN( RGB_BEST_MAP        ),
            PRE_ASSIGN( RGB_BLUE_MAP        ),
            PRE_ASSIGN( RGB_DEFAULT_MAP     ),
            PRE_ASSIGN( RGB_GRAY_MAP        ),
            PRE_ASSIGN( RGB_GREEN_MAP       ),
            PRE_ASSIGN( RGB_RED_MAP         ),
            PRE_ASSIGN( STRING              ),
            PRE_ASSIGN( VISUALID            ),
            PRE_ASSIGN( WINDOW              ),
            PRE_ASSIGN( WM_COMMAND          ),
            PRE_ASSIGN( WM_HINTS            ),
            PRE_ASSIGN( WM_CLIENT_MACHINE   ),
            PRE_ASSIGN( WM_ICON_NAME        ),
            PRE_ASSIGN( WM_ICON_SIZE        ),
            PRE_ASSIGN( WM_NAME             ),
            PRE_ASSIGN( WM_NORMAL_HINTS     ),
            PRE_ASSIGN( WM_SIZE_HINTS       ),
            PRE_ASSIGN( WM_ZOOM_HINTS       ),
            PRE_ASSIGN( MIN_SPACE           ),
            PRE_ASSIGN( NORM_SPACE          ),
            PRE_ASSIGN( MAX_SPACE           ),
            PRE_ASSIGN( END_SPACE           ),
            PRE_ASSIGN( SUPERSCRIPT_X       ),
            PRE_ASSIGN( SUPERSCRIPT_Y       ),
            PRE_ASSIGN( SUBSCRIPT_X         ),
            PRE_ASSIGN( SUBSCRIPT_Y         ),
            PRE_ASSIGN( UNDERLINE_POSITION  ),
            PRE_ASSIGN( UNDERLINE_THICKNESS ),
            PRE_ASSIGN( STRIKEOUT_ASCENT    ),
            PRE_ASSIGN( STRIKEOUT_DESCENT   ),
            PRE_ASSIGN( ITALIC_ANGLE        ),
            PRE_ASSIGN( X_HEIGHT            ),
            PRE_ASSIGN( QUAD_WIDTH          ),
            PRE_ASSIGN( WEIGHT              ),
            PRE_ASSIGN( POINT_SIZE          ),
            PRE_ASSIGN( RESOLUTION          ),
            PRE_ASSIGN( COPYRIGHT           ),
            PRE_ASSIGN( NOTICE              ),
            PRE_ASSIGN( FONT_NAME           ),
            PRE_ASSIGN( FAMILY_NAME         ),
            PRE_ASSIGN( FULL_NAME           ),
            PRE_ASSIGN( CAP_HEIGHT          ),
            PRE_ASSIGN( WM_CLASS            ),
            PRE_ASSIGN( WM_TRANSIENT_FOR    ),
            #undef PRE_ASSIGN
        };

        std::vector<item_t> items = {
            { "WM_CHANGE_STATE"                 },
            { "WM_DELETE_WINDOW"                },
            { "WM_PROTOCOLS"                    },
            { "_GTK_WORKAREAS_D0"               },
            { "_MOTIF_WM_HINTS"                 },
            { "_NET_ACTIVE_WINDOW"              },
            { "_NET_CLOSE_WINDOW"               },
            { "_NET_DESKTOP_GEOMETRY"           },
            { "_NET_REQUEST_FRAME_EXTENTS"      },
            { "_NET_SUPPORTED"                  },
            { "_NET_SUPPORTING_WM_CHECK"        },
            { "_NET_WM_ICON"                    },
            { "_NET_WM_MOVERESIZE"              },
            { "_NET_WM_NAME"                    },
            { "_NET_WM_PID"                     },
            { "_NET_WM_STATE"                   },
            { "_NET_WM_STATE_ABOVE"             },
            { "_NET_WM_STATE_BELOW"             },
            { "_NET_WM_STATE_DEMANDS_ATTENTION" },
            { "_NET_WM_STATE_HIDDEN"            },
            { "_NET_WM_STATE_FULLSCREEN"        },
            { "_NET_WM_STATE_MAXIMIZED_HORZ"    },
            { "_NET_WM_STATE_MAXIMIZED_VERT"    },
            { "_NET_WM_USER_TIME"               },
            { "_NET_WM_WINDOW_TYPE"             },
            { "_NET_WM_WINDOW_TYPE_DESKTOP"     },
            { "_NET_WM_WINDOW_TYPE_DOCK"        },
            { "_NET_WM_WINDOW_TYPE_NORMAL"      },
            { "_NET_WM_WINDOW_TYPE_SPLASH"      },
            { "_NET_WM_WINDOW_TYPE_DIALOG"      },
            { "_NET_WM_WINDOW_TYPE_UTILITY"     },
            { "_NET_WORKAREA"                   },
            { "_XKB_RULES_NAMES"                },
            { "CLIPBOARD"                       },
            { "INCR"                            },
            { "TARGETS"                         },
            { "TEXT"                            },
            { "utf8"                            },
            { "UTF-8"                           },
            { "UTF8_STRING"                     },
            { "ISO8859-1"                       },
            { "ISO8859-2"                       },
            { "ISO8859-3"                       },
            { "ISO8859-4"                       },
            { "ISO8859-5"                       },
            { "ISO8859-6"                       },
            { "ISO8859-7"                       },
            { "ISO8859-8"                       },
            { "ISO8859-9"                       },
            { "ISO8859-10"                      },
            { "ISO8859-11"                      },
            { "ISO8859-12"                      },
            { "ISO8859-13"                      },
            { "ISO8859-14"                      },
            { "ISO8859-15"                      },
            { "ISO8859-16"                      },
            { "image/bmp"                       },
            { "image/gif"                       },
            { "image/png"                       },
            { "image/jpeg"                      },
            { "image/tiff"                      },
            { "text/html"                       },
            { "text/plain"                      },
            { "text/plain;charset=iso8859-1"    },
            { "text/plain;charset=utf-8"        },
            { "x-special/gnome-copied-files"    },
        };

        for (auto &item : items) {
            item.cookie = xcb_intern_atom(connection, 0, strlen(item.name), item.name);
        }

        for (auto iter = items.begin(); iter != items.end(); iter++) {
            auto reply = xcb_intern_atom_reply(connection, iter->cookie, nullptr);
            if (!reply) {
                fprintf(stderr, "xcb_intern_atom_reply() failed '%s'", iter->name);
                for (++iter; iter != items.end(); iter++) {
                    xcb_discard_reply(connection, iter->cookie.sequence);
                }
                return false;
            }
            iter->atom = reply->atom;
            free(reply);
        }

        items.insert(items.begin(), pre_assign_item.begin(), pre_assign_item.end());

        printf("\n* Pre-cached list\n");
        for (auto &item : items) {
            printf("  - xcb_atom (%3u): '%s'\n", item.atom, item.name);
            atoms[item.name] = item.atom;
            atom_names[item.atom] = item.name;
        }
        return true;
    }

    xcb_atom_t Get(const char *name)
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

    const char *GetName(xcb_atom_t atom)
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

private:
    int                                   screen_num  = 0;
    xcb_connection_t                     *connection  = nullptr;
    std::map<std::string, xcb_atom_t>     atoms       = {};
    std::map<xcb_atom_t, std::string>     atom_names  = {};
};

int main(int argc, char **argv)
{ 
    printf("Example xcb_atom\n");

    auto obj = Atom();
    if (!obj.ShowCase()) {
        printf("\nFailed..\n");
        return EXIT_FAILURE;
    }
    printf("\nSucceed..\n");
    return EXIT_SUCCESS;
}
