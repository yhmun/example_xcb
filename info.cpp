#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <xcb/xcb.h>

class Info
{
public:
    Info(void)
    {    
    }

    ~Info(void)
    {
        if (connection) {
            xcb_disconnect(connection);
        }
    }

    bool Dump(void)
    {
        connection = xcb_connect(nullptr, &screen_num);
        if (!connection) {
            fprintf(stderr, "xcb_connect() failed\n");
            return false;
        }
        setup = xcb_get_setup(connection);

        if (!DumpConnection()) {
            return false;
        }
        if (!DumpSetup()) {
            return false;
        }
        return true;
    }

    bool DumpConnection(void)
    {
        printf("\n* xcb_connection\n");
        printf(" - xcb_get_file_descriptor              : %d\n", xcb_get_file_descriptor(connection));
        printf(" - xcb_total_read                       : %lu\n", xcb_total_read(connection));
        printf(" - xcb_total_written                    : %lu\n", xcb_total_written(connection));
        printf(" - xcb_get_maximum_request_length       : %u\n", xcb_get_maximum_request_length(connection));
        return true;
    }

    bool DumpSetup(void)
    {
        printf("\n* xcb_setup\n");

        printf(" - status                               : %u\n", setup->status);
        printf(" - protocol_version                     : %u.%u\n", setup->protocol_major_version, setup->protocol_minor_version);
        printf(" - length                               : %u\n", setup->length);
        printf(" - release_number                       : %u\n", setup->release_number);
        printf(" - resource_id_base                     : 0x%08X\n", setup->resource_id_base);
        printf(" - resource_id_mask                     : 0x%08X\n", setup->resource_id_mask);
        printf(" - motion_buffer_size                   : %u\n", setup->motion_buffer_size);
        printf(" - maximum_request_length               : %u\n", setup->maximum_request_length);
        printf(" - image_byte_order                     : %u\n", setup->image_byte_order);
        printf(" - bitmap_format_bit_order              : %u\n", setup->bitmap_format_bit_order);
        printf(" - bitmap_format_scanline_unit          : %u\n", setup->bitmap_format_scanline_unit);
        printf(" - bitmap_format_scanline_pad           : %u\n", setup->bitmap_format_scanline_pad);
        printf(" - keycode_range                        : %u ~ %u\n", setup->min_keycode, setup->max_keycode);
        printf(" - xcb_setup_sizeof                     : %d\n", xcb_setup_sizeof(setup));

        printf(" - vendor_len                           : %u\n", setup->vendor_len);
        if (setup->vendor_len) {
            printf("   . xcb_setup_vendor                   : %s\n", xcb_setup_vendor(setup));
            printf("   . xcb_setup_vendor_length            : %d\n", xcb_setup_vendor_length(setup));
        }

        printf(" - roots_len                            : %u\n", setup->roots_len);
        if (setup->roots_len && !DumpSetupRoots()) {
            return false;
        }

        printf(" - pixmap_formats_len                   : %u\n", setup->pixmap_formats_len);
        if (setup->pixmap_formats_len && !DumpSetupPixmapFormats()) {
            return false;
        }
        return true;
    }

    bool DumpSetupRoots(void)
    {
        printf("   . xcb_setup_roots (%d)\n", xcb_setup_roots_length(setup));
 
        auto iter = xcb_setup_roots_iterator(setup);
        for (auto i = 0; iter.rem; xcb_screen_next(&iter), i++) {
            auto screen = iter.data;
            printf("   . xcb_screen[%d]\n", i);
            printf("     . root                             : 0x%08X\n", screen->root);
            printf("     . default_colormap                 : 0x%08X\n", screen->default_colormap);
            printf("     . white_pixel                      : 0x%08X\n", screen->white_pixel);
            printf("     . black_pixel                      : 0x%08X\n", screen->black_pixel);
            printf("     . current_input_masks              : 0x%08X\n", screen->current_input_masks);
            printf("     . size_in_pixels                   : %4u x %4u\n", screen->width_in_pixels, screen->height_in_pixels);
            printf("     . size_in_millimeters              : %4u x %4u\n", screen->width_in_millimeters, screen->height_in_millimeters);
            printf("     . installed_maps_range             : %u ~ %u\n", screen->min_installed_maps, screen->max_installed_maps);
            printf("     . root_visual                      : 0x%08X\n", screen->root_visual);
            printf("     . backing_stores                   : %u\n", screen->backing_stores);
            printf("     . save_unders                      : %u\n", screen->save_unders);
            printf("     . root_depth                       : %u\n", screen->root_depth);
            printf("     . allowed_depths_len               : %u\n", screen->allowed_depths_len);

            if (screen->allowed_depths_len && !DumpScreenAllowedDepths(screen)) {
                return false;
            }
        }
        return true;
    }

    bool DumpScreenAllowedDepths(xcb_screen_t *screen)
    {
        printf("       . xcb_screen_allowed_depths (%d)\n", xcb_screen_allowed_depths_length(screen));

        auto iter = xcb_screen_allowed_depths_iterator(screen);
        for (auto i = 0; iter.rem; xcb_depth_next(&iter), i++) {
            auto depth = iter.data;
            printf("         . xcb_depth[%d]\n", i);
            printf("           . depth                      : %u\n", depth->depth);
            printf("           . visuals_len                : %u\n", depth->visuals_len);

            if (depth->visuals_len && !DumpDepthVisuals(depth)) {
                return false;
            }
        }
        return true;
    }

    bool DumpDepthVisuals(xcb_depth_t *depth)
    {
        printf("             . xcb_depth_visuals (%d)\n", xcb_depth_visuals_length(depth));

        auto iter = xcb_depth_visuals_iterator(depth);
        for (auto i = 0; iter.rem; xcb_visualtype_next(&iter), i++) {
            auto visualtype = iter.data;
            printf("               . xcb_visualtype[%d]\n", i);
            printf("                 . _class               : %u\n", visualtype->_class);
            printf("                 . bits_per_rgb_value   : %u\n", visualtype->bits_per_rgb_value);
            printf("                 . colormap_entries     : %u\n", visualtype->colormap_entries);
            printf("                 . color_mask           : 0x%08X\n", visualtype->red_mask | visualtype->green_mask | visualtype->blue_mask);
        }
        return true;
    }

    bool DumpSetupPixmapFormats(void)
    {
        printf("   . xcb_setup_pixmap_formats (%d)\n", xcb_setup_pixmap_formats_length(setup));
 
        auto iter = xcb_setup_pixmap_formats_iterator(setup);
        for (auto i = 0; iter.rem; xcb_format_next(&iter), i++) {
            auto format = iter.data;
            printf("     . xcb_format[%d]\n", i);
            printf("       . depth                          : %u\n", format->depth);
            printf("       . bits_per_pixel                 : %u\n", format->bits_per_pixel);
            printf("       . scanline_pad                   : %u\n", format->scanline_pad);
        }
        return true;
    }

private:
    int                 screen_num  = 0;
    xcb_connection_t   *connection  = nullptr;
    const xcb_setup_t  *setup       = nullptr;
};

int main(int argc, char **argv)
{ 
    printf("Example xcb_info\n");

    auto obj = Info();
    if (!obj.Dump()) {
        printf("\nFailed..\n");
        return EXIT_FAILURE;
    }
    printf("\nSucceed..\n");
    return EXIT_SUCCESS;
}
