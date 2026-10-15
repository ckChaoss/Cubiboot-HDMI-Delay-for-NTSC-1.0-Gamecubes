#include <math.h>
#include <stddef.h>

#include "picolibc.h"
#include "structs.h"
#include "attr.h"
#include "util.h"
#include "os.h"

#include "usbgecko.h"
#include "state.h"
#include "time.h"

#include "reloc.h"
#include "menu.h"
#include "theme.h"

#include "dolphin_arq.h"
#include "flippy_sync.h"
#include "gc_dvd.h"
#include "games.h"
#include "prompt.h"
#include "emu/drive_probe.h"

#include "video.h"
#include "dol.h"
#include "boot.h"
#include "gameid.h"

#define CUBE_TEX_WIDTH 84
#define CUBE_TEX_HEIGHT 84

#define STATE_WAIT_LOAD  0x0f // delay after animation
#define STATE_START_GAME 0x10 // play full animation and start game
#define STATE_NO_DISC    0x12 // play full animation before menu
#define STATE_COVER_OPEN 0x13 // force direct to menu

// __attribute_data__ u32 prog_entrypoint;
// __attribute_data__ u32 prog_dst;
// __attribute_data__ u32 prog_src;
// __attribute_data__ u32 prog_len;

__attribute_data__ u32 cube_color = 0;
__attribute_data__ u32 start_passthrough_game = 0;

__attribute_data__ u8 *cube_text_tex = NULL; // set by gm_load_cube_logo (games.c)
__attribute_data__ char cube_logo_path[MAX_FILE_NAME] = {0};
__attribute_data__ char default_folder[MAX_FILE_NAME] = {0};
__attribute_data__ u32 remember_last_game = 0;
// On by default: the root swiss-gc.dol gets its own tile above the grid (games.c, menu.c).
__attribute_data__ u32 pin_swiss = 1;
// On by default: games show the icon from their save in Swiss's emulated memory card (games.c).
__attribute_data__ u32 save_icons = 1;
// Set by the loader on a return from In-Game Reset (igr_skip_intro): no boot animation.
__attribute_data__ u32 skip_intro = 0;
__attribute_data__ u32 force_progressive = 0;
__attribute_data__ u32 force_widescreen = 0;
// On by default: a physical disc boots through Swiss unless config.ini turns it off.
__attribute_data__ u32 swiss_on_dvd_boot = 1;

// used if we are switching to 60Hz on a PAL IPL
__attribute_data__ static int fix_pal_ntsc = 0;

// used for optional delays
__attribute_data__ u32 preboot_delay_ms = 0;
// Set by the loader on IPL revisions that have the black-frame hold (pre_unblank_delay).
__attribute_data__ u32 preboot_black_hold = 0;
__attribute_data__ u32 postboot_delay_ms = 0;
__attribute_data__ u64 completed_time = 0;

// used to start game
__attribute_reloc__ u32 (*PADSync)();
__attribute_reloc__ void (*__OSStopAudioSystem)();
// __attribute_reloc__ void (*run)(register void* entry_point, register u32 clear_start, register u32 clear_size);

// for setup
__attribute_reloc__ void (*orig_thread_init)();
__attribute_reloc__ void (*menu_init)(int unk);
__attribute_reloc__ void (*main)();

__attribute_reloc__ model *bg_outer_model;
__attribute_reloc__ model *bg_inner_model;
__attribute_reloc__ model *gc_text_model;
__attribute_reloc__ model *logo_model;
__attribute_reloc__ model *cube_model;

// locals
__attribute_data__ static GXColorS10 color_cube;
__attribute_data__ static GXColorS10 color_cube_low;
__attribute_data__ static GXColorS10 color_bg_inner;
__attribute_data__ static GXColorS10 color_bg_outer_0;
__attribute_data__ static GXColorS10 color_bg_outer_1;

// These are technically separate floating-point constants, but they are laid out
// contiguously in this order in all IPL versions
typedef struct {
    struct {
        float aspect_ratio;
    } perspective;

    struct {
        float top;
        float bottom;
        float left;
        float right;
    } orthographic;
} projection_parameters_t;

__attribute_reloc__ projection_parameters_t *projection_parameters;

// start
__attribute_data__ gm_file_entry_t boot_entry;
__attribute_data__ gm_file_entry_t second_boot_entry;

__attribute_used__ void mod_cube_colors() {
    u32 logo_color = theme_get_boot_cube_color();
    if (logo_color == 0) {
        OSReport("Using default colors\n");
        return;
    }

    rgb_color target_color;
    target_color.color = (logo_color << 8) | 0xFF;

    // TODO: the HSL calculations do not render good results for darker inputs, I still need to tune SAT/LUM scaling
    // tough colors: 252850 A18594 763C28

    u32 target_hsl = GRRLIB_RGBToHSL(target_color.color);
    u32 target_hue = H(target_hsl);
    u32 target_sat = S(target_hsl);
    u32 target_lum = L(target_hsl);
    float sat_mult = (float)target_sat / 40.0; //* 1.5;
    if (sat_mult > 2.0) sat_mult = sat_mult * 0.5;
    if (sat_mult > 1.5) sat_mult = sat_mult * 0.5;
    // sat_mult = 0.35; // temp for light colors
    // sat_mult = 1.0;
    float lum_mult = (float)target_lum / 135.0; //* 0.75;
    if (lum_mult < 0.75) lum_mult = lum_mult * 1.5;
    OSReport("SAT MULT = %f\n", sat_mult);
    OSReport("LUM MULT = %f\n", lum_mult);
    OSReport("TARGET_HI: %02x%02x%02x = (%d, %d, %d)\n", target_color.parts.r, target_color.parts.g, target_color.parts.b, H(target_hsl), S(target_hsl), L(target_hsl));

    u8 low_hue = (u8)round((float)H(target_hsl) * 1.02857143);
    u8 low_sat = (u8)round((float)S(target_hsl) * 1.09482759);
    u8 low_lum = (u8)round((float)L(target_hsl) * 0.296296296);
    u32 low_hsl = HSLA(low_hue, low_sat, low_lum, A(target_hsl));
    rgb_color target_low;
    target_low.color = GRRLIB_HSLToRGB(low_hsl);
    OSReport("TARGET_LO: %02x%02x%02x = (%d, %d, %d)\n", target_low.parts.r, target_low.parts.g, target_low.parts.b, H(low_hsl), S(low_hsl), L(low_hsl));

    u8 bg_inner_hue = (u8)round((float)H(target_hsl) * 1.00574712);
    u8 bg_inner_sat = (u8)round((float)S(target_hsl) * 0.95867768);
    u8 bg_inner_lum = (u8)round((float)L(target_hsl) * 0.9);
    u32 bg_inner_hsl = HSLA(bg_inner_hue, bg_inner_sat, bg_inner_lum, bg_inner_model->data->mat[0].tev_color[0]->a);
    rgb_color bg_inner;
    bg_inner.color = GRRLIB_HSLToRGB(bg_inner_hsl);

    u8 bg_outer_hue_0 = (u8)round((float)H(target_hsl) * 1.02941176);
    u8 bg_outer_sat_0 = 0xFF;
    u8 bg_outer_lum_0 = (u8)round((float)L(target_hsl) * 1.31111111);
    u32 bg_outer_hsl_0 = HSLA(bg_outer_hue_0, bg_outer_sat_0, bg_outer_lum_0, bg_outer_model->data->mat[0].tev_color[0]->a);
    rgb_color bg_outer_0;
    bg_outer_0.color = GRRLIB_HSLToRGB(bg_outer_hsl_0);

    u8 bg_outer_hue_1 = (u8)round((float)H(target_hsl) * 1.07428571);
    u8 bg_outer_sat_1 = (u8)round((float)S(target_hsl) * 0.61206896);
    u8 bg_outer_lum_1 = (u8)round((float)L(target_hsl) * 0.92592592);
    u32 bg_outer_hsl_1 = HSLA(bg_outer_hue_1, bg_outer_sat_1, bg_outer_lum_1, bg_outer_model->data->mat[1].tev_color[0]->a);
    rgb_color bg_outer_1;
    bg_outer_1.color = GRRLIB_HSLToRGB(bg_outer_hsl_1);

    // logo

    DUMP_COLOR(logo_model->data->mat[1].tev_color[0]);
    DUMP_COLOR(logo_model->data->mat[1].tev_color[1]);
    DUMP_COLOR(logo_model->data->mat[2].tev_color[2]);

    tex_data *base = logo_model->data->tex->dat;
    for (int i = 0; i < 8; i++) {
        tex_data *p = base + i;
        if (p->width != 84) break; // early exit
        u16 wd = p->width;
        u16 ht = p->height;
        void *img_ptr = (void*)((u8*)base + p->offset + (i * 0x20));
        OSReport("FOUND TEX: %dx%d @ %p\n", wd, ht, img_ptr);

        // change hue of cube textures
        for (int y = 0; y < ht; y++) {
            for (int x = 0; x < wd; x++) {
                u32 color = GRRLIB_GetPixelFromtexImg(x, y, img_ptr, wd);

                // hsl
                {
                    u32 hsl = GRRLIB_RGBToHSL(color);
                    u32 sat = round((float)L(hsl) * sat_mult);
                    if (sat > 0xFF) sat = 0xFF;
                    u32 lum = round((float)L(hsl) * lum_mult);
                    if (lum > 0xFF) lum = 0xFF;
                    color = GRRLIB_HSLToRGB(HSLA(target_hue, (u8)sat, (u8)lum, A(hsl)));
                }

                GRRLIB_SetPixelTotexImg(x, y, img_ptr, wd, color);
            }
        }

        uint32_t buffer_size = (wd * ht) << 2;
        DCFlushRange(img_ptr, buffer_size);
    }

    // OSReport("Original Colors:\n");
    // color_cube
    // color_cube_low
    // color_bg_inner
    // color_bg_outer_0
    // color_bg_outer_1

    // DUMP_COLOR(cube_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(cube_model->data->mat[0].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[0].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[1].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[1].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[2].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[2].tev_color[1]);
    // DUMP_COLOR(bg_inner_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(bg_inner_model->data->mat[1].tev_color[0]);
    // DUMP_COLOR(bg_outer_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(bg_outer_model->data->mat[1].tev_color[0]);

    // while(1);


    // copy over colors
    copy_color(target_color, &cube_state->color);
    copy_color10(target_color, &color_cube);
    copy_color10(target_low, &color_cube_low);

    copy_color10(bg_inner, &color_bg_inner);
    copy_color10(bg_outer_0, &color_bg_outer_0);
    copy_color10(bg_outer_1, &color_bg_outer_1);

    cube_model->data->mat[0].tev_color[0] = &color_cube;
    cube_model->data->mat[0].tev_color[1] = &color_cube_low;

    logo_model->data->mat[0].tev_color[0] = &color_cube_low; // TODO: use different shades
    logo_model->data->mat[0].tev_color[1] = &color_cube_low; // TODO: <-
    logo_model->data->mat[1].tev_color[0] = &color_cube_low; // TODO: <-
    logo_model->data->mat[1].tev_color[1] = &color_cube_low; // TODO: <-
    logo_model->data->mat[2].tev_color[0] = &color_cube_low; // TODO: <-
    logo_model->data->mat[2].tev_color[1] = &color_cube_low; // TODO: <-

    bg_inner_model->data->mat[0].tev_color[0] = &color_bg_inner;
    bg_inner_model->data->mat[1].tev_color[0] = &color_bg_inner;

    bg_outer_model->data->mat[0].tev_color[0] = &color_bg_outer_0;
    bg_outer_model->data->mat[1].tev_color[0] = &color_bg_outer_1;

    return;
}

// The texture swap itself. cube_text_tex is filled by gm_load_cube_logo (games.c) on the
// enum thread; until then this is a no-op and the stock "GAMECUBE" text stays. Idempotent:
// the offset is recomputed from absolute addresses, so a second call writes the same values.
__attribute_used__ void mod_cube_text() {
        tex_data *gc_text_tex = gc_text_model->data->tex->dat;

        u16 wd = gc_text_tex->width;
        u16 ht = gc_text_tex->height;
        void *img_ptr = (void*)((u8*)gc_text_tex + gc_text_tex->offset);
        u32 img_size = wd * ht;

#ifndef DEBUG
        (void)img_ptr;
        (void)img_size;
#endif

        OSReport("CUBE TEXT TEX: %dx%d[%d] (type=%d) @ %p\n", wd, ht, img_size, gc_text_tex->format, img_ptr);
        OSReport("PTR = %08x\n", (u32)cube_text_tex);
        OSReport("ORIG_PTR_PARTS = %08x, %08x\n", (u32)gc_text_tex, gc_text_tex->offset);

        if (cube_text_tex != NULL) {
            s32 desired_offset = gc_text_tex->offset;
            if ((u32)gc_text_tex > (u32)cube_text_tex) {
                desired_offset = -1 * (s32)((u32)gc_text_tex - (u32)cube_text_tex);
            } else {
                desired_offset = (s32)((u32)cube_text_tex - (u32)gc_text_tex);
            }

            OSReport("DESIRED = %d\n", desired_offset);

            // change the texture format
            gc_text_tex->format = GX_TF_RGBA8;
            gc_text_tex->offset = desired_offset;
        }
}


__attribute_used__ void mod_cube_anim() {
    if (fix_pal_ntsc) {
        cube_state->cube_side_frames = 10;
        cube_state->cube_corner_frames = 16;
        cube_state->fall_anim_frames = 5;
        cube_state->fall_delay_frames = 16;
        cube_state->up_anim_frames = 18;
        cube_state->up_delay_frames = 7;
        cube_state->move_anim_frames = 10;
        cube_state->move_delay_frames = 20;
        cube_state->done_delay_frames = 40;
        cube_state->logo_hold_frames = 60;
        cube_state->logo_delay_frames = 5;
        cube_state->audio_cue_frames_b = 7;
        cube_state->audio_cue_frames_c = 6;
    }
}

// Resolve the configured default folder into a path the menu can open.
// Falls back to the SD root ("/") when unset or when the folder can't be opened.
__attribute_used__ char* resolve_default_folder() {
    if (default_folder[0] == '\0') {
        OSReport("No default folder set, opening root\n");
        return "/";
    }

    const char *path = default_folder;
    static char path_buf[MAX_FILE_NAME];
    if (default_folder[0] != '/') {
        snprintf(path_buf, sizeof(path_buf), "/%s", default_folder);
        path = path_buf;
    }

    if (dvd_custom_open(path, FILE_ENTRY_TYPE_DIR, 0) != 0) {
        OSReport("Could not open default folder: %s, opening root\n", path);
        return "/";
    }
    dvd_custom_close(dvd_custom_status()->fd);

    OSReport("Using default folder: %s\n", path);
    return (char*)path;
}

__attribute_used__ void pre_thread_init() {
    dolphin_ARAMInit();
    orig_thread_init();

    gm_init_heap();
    gm_init_thread();

    // Ask once, here, who is on the drive connector: the disc screen needs the answer, and
    // this is the last moment the drive interface is idle. On an ODE or a FlippyDrive the
    // enumeration thread below reads files over that same interface, so an inquiry raised
    // later could land in the middle of one of its transfers. The result is cached, so the
    // disc screen's check costs nothing and never touches the bus.
    drive_id_t drive = drive_probe();

    // A disc left spinning by the game we came back from would otherwise never stop. An
    // In-Game Reset through Swiss jumps back here without resetting the drive, and nothing
    // on the menu side sends it a command: the IPL's own disc machine is not run (see
    // bs2tick), and a game booted from the card goes through Swiss, which leaves the real
    // drive alone. Only entering the disc screen (its exit sends this same command) or
    // opening the lid stopped it.
    //
    // Only a stock optical drive gets it -- a GC Loader or a FlippyDrive on its file API
    // is left alone -- and never with a passthrough boot already pending, so that path is
    // exactly as before. Every disc boot starts from a stopped motor anyway: the disc
    // screen stops it on exit before START, and the boot paths reset the drive and wait
    // out the spin-up. On a drive that is already stopped this does nothing.
    if (drive == DRIVE_ID_UNKNOWN && !start_passthrough_game)
        dvd_stop_motor();

    // Correct the IPL's own bottom prompt bar. Ahead of menu_init on purpose: the IPL takes
    // its own copy of the label strings in there, so a rename applied later moved the labels
    // but left them reading Cancel and Menu Selection.
    prompt_bar_init();
    prompt_bar_layout();

    if (!start_passthrough_game) {
        // NULL = cold boot. The starting folder (last-played when remember_last_game is
        // on, else default_folder) is resolved on the enum thread, NOT here: this hook
        // runs too early for the memory card slot readers, whose first SD access fails
        // while the BIOS still owns EXI channels 0/1 -- see gm_thread_worker.
        gm_start_thread(NULL);
    }
}

__attribute_used__ void pre_menu_init(int unk) {
    menu_init(unk);

    // change default menu
    *prev_menu_id = MENU_GAMESELECT_TRANSITION_ID;
    *cur_menu_id = MENU_GAMESELECT_ID;

    custom_gameselect_init();

    mod_cube_colors();
    mod_cube_text();
    mod_cube_anim();

    // Delay before the boot animation, so a TV / GCVideo has time to lock onto the signal.
    // This wait runs while the IPL still has the picture blanked (no active video), so an
    // HDMI adapter cannot lock during it. Revisions with the black-frame hold below do their
    // waiting there instead, once the picture is on.
    if (!preboot_black_hold) {
        const u32 fields_per_sec = rmode->viTVMode >> 2 == VI_NTSC ? 60 : 50;
        const u32 total_frames = ((u64)preboot_delay_ms * fields_per_sec) / 1000;
        for (u32 i = 0; i < total_frames; i++) {
            VIWaitForRetrace();
        }
    }
}

/*
 * Preboot delay, black-frame hold (NTSC 1.0 hook at 0x813019bc).
 *
 * BS2's frame-swap routine (0x8130191c, run once per frame) un-blanks the picture exactly
 * once, on the first frame, right after queuing frame 0 of the cube animation:
 *
 *     813019a0: lwz r3, cur_xfb
 *     813019ac: bl VISetNextFrameBuffer
 *     813019bc: li r3, 0
 *     813019c0: bl VISetBlack          ; VISetBlack(FALSE) -> picture appears
 *
 * While the VI is blanked it sends sync only (zero active lines), so an HDMI adapter or
 * capture card sees "no signal" and cannot lock. We patch the `li r3, 0` into
 * `bl pre_unblank_delay`, which returns 0 in r3 so the stock `bl VISetBlack` still gets
 * its FALSE argument.
 *
 * In the hook we paint the spare XFB black, point the VI at it, turn the picture on and
 * wait. The adapter locks onto a real (black) picture while the animation, which only
 * advances when we return, stays untouched. Then frame 0 is restored and BS2 carries on,
 * so what is seen is: black -> full animation from its first frame, with the audio cues
 * (frame counted) landing after the link is up.
 *
 * Runs once only (the IPL clears its first-frame flag right after). Uses VIWaitForRetrace
 * (the same call BS2 makes a few instructions later, so it is safe in this context), but
 * bounds the wait by the time base so it is correct for 50/60 Hz and 480p alike.
 */
__attribute_used__ u32 pre_unblank_delay(void) {
    if (preboot_delay_ms) {
        // NTSC 1.0 addresses in BS2 (only reached through the NTSC 1.0 hook).
        void (*bs2_VISetNextFrameBuffer)(void *) = (void (*)(void *))0x8133ee1c;
        void (*bs2_VISetBlack)(u32)              = (void (*)(u32))0x8133ee88;
        void (*bs2_VIFlush)(void)                = (void (*)(void))0x8133ed18;

        // BS2 small-data variables (r13 = 0x81465320): the two XFBs, the one that holds
        // the frame we are about to show, and the video mode they were sized from.
        void **xfb_a_ptr = (void **)0x8145d590;   // r13-32144
        void **xfb_b_ptr = (void **)0x8145d594;   // r13-32140
        void **cur_ptr   = (void **)0x8145d598;   // r13-32136
        u8   **mode_ptr  = (u8 **)0x8145d59c;     // r13-32132 (GXRModeObj*)

        void *cur   = *cur_ptr;                   // frame 0 of the cube animation
        void *other = (cur == *xfb_a_ptr) ? *xfb_b_ptr : *xfb_a_ptr;

        // Same size BS2 allocated each XFB with: ((fbWidth + 15) & ~15) * xfbHeight * 2.
        u32 fb_width   = *(u16 *)(*mode_ptr + 4);
        u32 xfb_height = *(u16 *)(*mode_ptr + 8);
        u32 bytes      = ((fb_width + 15) & ~15u) * xfb_height * 2;

        // Paint the spare XFB black (YUY2 black = Y 0x10, Cb/Cr 0x80, which is exactly what
        // the animation's own black background converts to) and push it out of the cache
        // so the VI DMA sees it. The loop has an asm statement in it so GCC cannot turn it
        // into a memset() call (there is no libc here).
        for (u32 off = 0; off + 32 <= bytes; off += 32) {
            u32 *line = (u32 *)((u8 *)other + off);
            line[0] = 0x10801080; line[1] = 0x10801080;
            line[2] = 0x10801080; line[3] = 0x10801080;
            line[4] = 0x10801080; line[5] = 0x10801080;
            line[6] = 0x10801080; line[7] = 0x10801080;
            __asm__ volatile("dcbf 0,%0" : : "r"(line) : "memory");
        }
        __asm__ volatile("sync" : : : "memory");

        // Switch the picture ON, but showing the black buffer. The adapter / capture card
        // now sees a real active-video signal (it ignores the blanked one) and can lock
        // while nothing visible happens. The animation has not advanced: it only moves
        // when we return.
        bs2_VISetNextFrameBuffer(other);
        bs2_VISetBlack(0);
        bs2_VIFlush();

        u64 start = gettime();
        while (diff_msec(start, gettime()) < preboot_delay_ms) {
            VIWaitForRetrace();
        }

        // Hand frame 0 back; BS2's own VIFlush right after this hook displays it.
        bs2_VISetNextFrameBuffer(cur);
    }
    return 0;
}

__attribute_used__ void pre_main() {
    OSReport("RUNNING BEFORE MAIN\n");

    OSReport("efbHeight = %u\n", rmode->efbHeight);
    OSReport("xfbHeight = %u\n", rmode->xfbHeight);

    if (force_progressive) {
        OSReport("Patching video mode to Progressive Scan\n");
        fix_pal_ntsc = rmode->viTVMode >> 2 != VI_NTSC;
        if (fix_pal_ntsc) {
            rmode->fbWidth = 592;
            rmode->efbHeight = 226;
            rmode->xfbHeight = 448;
            rmode->viXOrigin = 40;
            rmode->viYOrigin = 16;
            rmode->viWidth = 640;
            rmode->viHeight = 448;
        }

        // sample points arranged in increasing Y order
        u8  sample_pattern[12][2] = {
            {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
            {6,6},{6,6},{6,6},  // pix 1
            {6,6},{6,6},{6,6},  // pix 2
            {6,6},{6,6},{6,6}   // pix 3
        };
        memcpy(&rmode->sample_pattern[0][0], &sample_pattern[0][0], (12*2));

        rmode->viTVMode = VI_TVMODE_NTSC_PROG;
        rmode->xfbMode = VI_XFBMODE_SF;
        // rmode->aa = FALSE; // breaks the IPL??
        // rmode->field_rendering = TRUE;

        rmode->vfilter[0] = 0;
        rmode->vfilter[1] = 0;
        rmode->vfilter[2] = 21;
        rmode->vfilter[3] = 22;
        rmode->vfilter[4] = 21;
        rmode->vfilter[5] = 0;
        rmode->vfilter[6] = 0;
    }

    if (force_widescreen) {
        // In all IPL versions, the default projection matrices use:
        // - a perspective aspect ratio of 1.3214285
        // - an orthographic screen size of (t: 224, b: -224, l: -296, r: 292)
        // (Note that the perspective and orthographic aspect ratios don't seem to match up,
        // and neither are actually 4:3)
        // Stretch these by the same ratio as 4:3 -> 16:9. The left/right values are the
        // originals times 4/3, floored to integers (floorf is unavailable here).
        projection_parameters->perspective.aspect_ratio = 1.3214285f * (4.0f / 3.0f);
        projection_parameters->orthographic.left = -395.0f;
        projection_parameters->orthographic.right = 389.0f;
    }

    main();

    __builtin_unreachable();
}

__attribute_used__ u32 get_tvmode() {
    return rmode->viTVMode;
}

__attribute_data__ int frame_count = 0;
__attribute_used__ u32 bs2tick() {
    extern u32 stock_disc_mode;

    frame_count++;
    if (!completed_time && cube_state->cube_anim_done) {
        OSReport("FINISHED (%d frames)\n", frame_count);
        completed_time = gettime();
    }

    // postboot_delay_ms is not handled here: this hook only runs while the IPL's own state
    // machine is driving, and by the time a game is picked from the menu the animation has
    // long since finished. The wait lives in bs2start() instead, where every boot path
    // converges.
    // While the disc screen is up, Cubiboot drives the read itself and reports the state
    // the screen renders from. The IPL's own machine is deliberately not run: its route to
    // a banner goes through the apploader, which loads the game over the memory Cubiboot
    // occupies and takes the console down mid-read.
    if (stock_disc_mode) {
        extern u32 stock_disc_tick(void);
        return stock_disc_tick();
    }

    if (start_passthrough_game) {
        return STATE_START_GAME;
    }

    // this helps the start menu show correctly
    if (*main_menu_id >= 3) {
        return STATE_START_GAME;
    }

#ifdef TEST_SKIP_ANIMATION
    return STATE_COVER_OPEN;
#endif

    // Back from In-Game Reset (config.ini igr_skip_intro): straight to the game list.
    if (skip_intro) {
        return STATE_COVER_OPEN;
    }

    // TODO: allow the user to decide if they want to logo to play
    return STATE_NO_DISC;
}

__attribute_used__ void bs2start() {
    OSReport("DONE\n");

    // Optional dwell before the game takes over, to recover the feeling of waiting for a
    // disc to spin up. The IPL's frame loop has already exited, so the last frame it drew
    // stays on screen for the whole wait. Placed here, ahead of the device and audio
    // teardown below, so it applies to every boot path: game, program and DVD passthrough.
    if (postboot_delay_ms) {
        u64 delay_start = gettime();
        while (diff_msec(delay_start, gettime()) < postboot_delay_ms);
    }

    // read boot info into lowmem
    struct dolphin_lowmem *lowmem = (struct dolphin_lowmem*)0x80000000;

    if (!start_passthrough_game || swiss_on_dvd_boot) {
        gm_deinit_thread();
    } else {
        dvd_custom_bypass_enter();
        udelay(10 * 1000);

        // Entering bypass resets the drive, so a disc that was spinning a moment ago is
        // spinning down again here. A single read lands inside that window and reports the
        // disc as unreadable, dropping a perfectly good boot into the loader stub below --
        // the same spin-up window the disc screen has to wait out before it can read a
        // banner. Retry rather than treating the first answer as final.
        int ret = 1;
        int err = 1;
        for (int i = 0; i < 8; i++) {
            ret = dvd_read_id();
            err = dvd_get_error();
            if (ret == 0 && err == 0) break;
            udelay(250 * 1000);
        }

        if (ret != 0 || err != 0) {
            custom_OSReport("Failed to read disc ID\n");
            dvd_custom_bypass_exit();
            udelay(10 * 1000);

            load_stub(); // exit to loader again
            u32 *sig = (u32*)0x80001804;
            if ((*sig++ == 0x53545542 || *sig++ == 0x53545542) && *sig == 0x48415858) {
                static void (*reload)(void) = (void(*)(void))0x80001800;
                run(reload);
            }
        }

        custom_OSReport("Game ID: %c%c%c%c\n", lowmem->b_disk_info.game_code[0], lowmem->b_disk_info.game_code[1], lowmem->b_disk_info.game_code[2], lowmem->b_disk_info.game_code[3]);
        dvd_audio_config(lowmem->b_disk_info.audio_streaming, lowmem->b_disk_info.stream_buffer_size);

        char diskName[64] = "DISC GAME\0";
        setup_gameid_commands(&lowmem->b_disk_info, diskName);
    }

    // no IPL code should be running after this point

    extern void tsd_set_native(bool native);
    tsd_set_native(true);

    while (!PADSync());
    OSDisableInterrupts();
    __OSStopAudioSystem();

    u32 start_addr = 0x80100000;
    u32 end_addr = 0x81600000;
    u32 len = end_addr - start_addr;

    // The sector cache keeps its pages inside the range about to be cleared, and its
    // bookkeeping would go on claiming those pages are valid -- so every read after this
    // point would be served zeros. That is not a subtle failure: the files read below are
    // the game and the apploader, so nothing would boot on any device. Disable it here,
    // where the wipe is, rather than trusting the magic it keeps alongside the pages to
    // catch it.
    extern void sector_cache_disable(void);
    sector_cache_disable();

    memset((void*)start_addr, 0, len); // cleanup
    DCFlushRange((void*)start_addr, len);
    ICInvalidateRange((void*)start_addr, len);

    // Passthrough mode
    if (start_passthrough_game) {
        chainload_boot_game(NULL, true);
    }

    char *boot_path = boot_entry.path;
    // An app entry already points at its default.dol, so it boots exactly like any other
    // homebrew program from here on.
    if (boot_entry.type == GM_FILE_TYPE_PROGRAM || boot_entry.type == GM_FILE_TYPE_APP) {
        custom_OSReport("Booting DOL\n");
        load_stub();

        chainload_swiss_game(boot_path, false); // use swiss as normal dol load is partially broken
        //dol_info_t info = load_dol_file(boot_path, false);
        //run(info.entrypoint);
    } else {
        custom_OSReport("Booting ISO\n");

        if (!swiss_on_dvd_boot || is_swiss_image(boot_path)) {
            // Swiss disc images always take the native apploader path (see chainload_boot_game),
            // even when swiss_on_dvd_boot is on -- routing them through Swiss resets to the IPL.
            custom_OSReport("Booting ISO (custom apploader)\n");
            chainload_boot_game(&boot_entry, false);
        } else {
            custom_OSReport("Booting ISO (swiss chainload)\n");
            chainload_swiss_game(boot_path, false);
        }
    }

    __builtin_unreachable();
}

void mega_trap(u32 r3, u32 r4, u32 r5, u32 r6) {
    u32 caller = (u32)__builtin_return_address(0);
    OSReport("[%08x] (r3=%08x r4=%08x r5=%08x, r6=%08x) You hit the mega trap dog\n", caller, r3, r4, r5, r6);
    while(1);
}

// unused
void _start() {}
