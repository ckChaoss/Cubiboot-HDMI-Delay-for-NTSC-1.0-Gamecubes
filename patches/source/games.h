#pragma once

#include <gctypes.h>

#include "dolphin_os.h"
#include "icon.h"
#include "bnr.h"

#include "decomp_ar.h"

// Backing
typedef enum {
    GM_LOAD_STATE_NONE,
    GM_LOAD_STATE_SETUP, // valid for use
    GM_LOAD_STATE_UNLOADED,
    GM_LOAD_STATE_UNLOADING,
    GM_LOAD_STATE_LOADING,
    GM_LOAD_STATE_LOADED, // valid for use
} gm_load_state_t;

typedef enum {
    GM_FILE_TYPE_UNKNOWN,
    GM_FILE_TYPE_DIRECTORY,
    GM_FILE_TYPE_PROGRAM,
    GM_FILE_TYPE_GAME,
    // A folder holding default.dol next to opening.bnr. Selecting it boots the .dol
    // instead of entering the folder, and it draws with the banner like a game does.
    GM_FILE_TYPE_APP
} gm_file_type_t;

#pragma pack(push,1)
typedef struct {
    u32 used;
    u32 padding[7];
    u8 data[ICON_PIXELDATA_LEN];
} gm_icon_buf_t;

typedef struct {
    u32 used;
    u32 padding[7];
    u8 data[BNR_PIXELDATA_LEN];
} gm_banner_buf_t __attribute__((aligned(32)));
#pragma pack(pop)

typedef struct {
    ARQRequest req;
    u32 aram_offset;
    bool schedule_free;
    gm_load_state_t state;
    gm_icon_buf_t *buf;
} gm_icon_t;

typedef struct {
    ARQRequest req;
    u32 aram_offset;
    bool schedule_free;
    gm_load_state_t state;
    gm_banner_buf_t *buf;
} gm_banner_t;

typedef struct {
    bool use_banner;
    gm_icon_t icon;
    gm_banner_t banner;
} gm_asset_t;

// typedef struct gm_disc_header {
//     char game_code[4];
// 	char maker_code[2];
// 	char disk_id;
// 	char version;
// } gm_disc_header_t;

typedef struct {
    u8 game_id[6];
    u8 disc_num;
    u8 disc_ver;
    // The banner is a file of its own (an app's opening.bnr) rather than a region inside a
    // disc image, so it reads from offset 0 and dvd_bnr_offset stops being a valid "has a
    // banner" test -- 0 is a real offset here.
    bool standalone_bnr;
    u8 dvd_bnr_type;
    u32 dvd_bnr_offset;
    u32 dvd_dol_offset;
    u32 dvd_fst_offset;
    u32 dvd_fst_size;
    u32 dvd_max_fst_size;
} gm_extra_t;

typedef struct {
    char path[128];
    gm_file_type_t type;
} gm_path_entry_t;

typedef struct gm_file_entry_struct gm_file_entry_t;

struct gm_file_entry_struct {
    char path[128];
    BNRDesc desc;
    gm_extra_t extra;
    gm_asset_t asset;
    gm_file_type_t type;
    gm_file_entry_t *second;
};

extern int number_of_lines;
extern int game_backing_count;
extern OSMutex *game_enum_mutex;
extern bool game_enum_running;
extern char game_enum_path[];

// for boot but defined in main (???)
extern gm_file_entry_t boot_entry;
extern gm_file_entry_t second_boot_entry;

// last-played: remember_last_game is patched in by the loader (cubeboot/main.c) from
// config.ini; gm_pending_last_played_slot is the entry index the enum thread wants the
// menu thread to select once enumeration finishes (-1 = nothing pending).
extern u32 remember_last_game;
extern int gm_pending_last_played_slot;
void gm_apply_pending_last_played();              // menu thread: jump to pending slot
char *gm_last_played_folder(void);                // startup: folder containing last game, or NULL

// Pinned Swiss tile (config.ini pin_swiss): a reserved slot index that gm_get_game_entry
// answers with the root swiss-gc.dol. selected_slot holds it while the tile is selected.
#define GM_SWISS_PIN_SLOT (-1)
extern u32 pin_swiss;
bool gm_swiss_pin_present(void);

// Save icons: the frame of an icon to draw at `tick` (60 Hz), animated or still.
const u8 *gm_icon_frame_data(gm_icon_buf_t *first, u32 tick);
void gm_icons_mark_dirty(void);   // a scroll/jump/scan changed which rows are on screen
void gm_icons_tick(void);         // menu thread, once per frame: load/free icons for those rows

void gm_init_heap();
void gm_init_thread();
void gm_deinit_thread();
void gm_start_thread(const char *target);
void gm_line_changed(int delta);
bool gm_can_move();
gm_file_entry_t *gm_get_game_entry(int index);
