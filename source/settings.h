// The settings: the Settings struct and shot modes, the menu rows of every setting, the app folders and the
// settings file with its presets.
#pragma once
#include <3ds.h>
#include "cam_settings.h"
#include "raw10.h"

// What the shutter takes.
//   PHOTO   one raw frame (DNG + the look's JPG); the only mode with the 60 fps live view
//   MERGE   a burst merged into one cleaner photo
//   HDR     both outer cameras at one moment, A longer (shadows), B shorter (highlights), merged into one DNG
//           with the extra highlight range and a JPG that rolls those highlights off
//   3D      a stereo pair
//   BRACKET an exposure series, every frame its own photo (in a folder)
enum {
	MODE_PHOTO,
	MODE_MERGE,
	MODE_HDR,
	MODE_3D,
	MODE_BRACKET,
	MODE_COUNT
};

// How much information the viewfinder shows (Settings.hud).
enum {
	HUD_OFF,
	HUD_MINIMAL,
	HUD_FULL,
	HUD_LEVEL_COUNT
};

// What a raw shot saves (Settings.photoFormat).
typedef enum {
	PHOTO_FORMAT_RAW_AND_JPG,
	PHOTO_FORMAT_JPG_ONLY,
	PHOTO_FORMAT_RAW_ONLY, // the DNG alone, so no look
	PHOTO_FORMAT_COUNT
} PhotoFormat;

// The size of the JPG or PNG (Settings.photoSize), reduced by nearest neighbour.
typedef enum {
	PHOTO_SIZE_FULL,
	PHOTO_SIZE_HALF,
	PHOTO_SIZE_QUARTER,
	PHOTO_SIZE_COUNT
} PhotoSize;

#define PRESET_SLOT_COUNT 3
#define CAMERA_CHOICE_COUNT 2 // outer (rear) and inner (selfie)
#define OUTPUT_FORMAT_COUNT 2 // CAM_FORMAT_RGB565 and CAM_FORMAT_YUYV
#define BRACKET_SERIES_CHOICE_COUNT 3
#define SELF_TIMER_CHOICE_COUNT 4 // off, 2 s, 5 s, 10 s
#define BURST_FRAME_CHOICE_COUNT 4 // 4, 6, 8 or 12 frames per camera

// HDR's exposure difference between the two cameras.
#define HDR_GAP_MIN_EV 1
#define HDR_GAP_COUNT 3
#define HDR_GAP_MAX_EV (HDR_GAP_MIN_EV + HDR_GAP_COUNT - 1)

typedef struct {
	CamSettings cam;
	s8 timer; // self-timer: index into the self-timer delays (SELF_TIMER_CHOICE_COUNT)
	s8 mode; // MODE_*
	s8 shutterSound;
	s8 histogram;
	s8 grid;
	s8 zebra;
	s8 peaking;
	s8 hud; // HUD_*
	s8 slot; // preset slot, 0 .. PRESET_SLOT_COUNT - 1
	s8 photoSize; // PHOTO_SIZE_*
	s8 review; // show each photo for a moment after taking it
	s8 bracketSeries; // BRACKET: index into BRACKET_SERIES_NAMES
	s8 burstFrames; // MERGE and HDR: index into the burst frame choices (settings_burst_frames_per_camera)
	s8 hdrGap; // HDR: exposure difference between the two cameras, HDR_GAP_MIN_EV .. HDR_GAP_MAX_EV
	s8 photoFormat; // raw mode: PHOTO_FORMAT_*
	s8 lookIndex; // the look of the JPG and the live view
} Settings;

typedef enum {
	ROW_ENUM,
	ROW_INT,
	ROW_BOOL,
	ROW_ACTION
} RowKind;

typedef enum {
	ACTION_NONE,
	ACTION_SAVE_PRESET,
	ACTION_LOAD_PRESET,
	ACTION_RESET,
	ACTION_INFO,
	ACTION_EXIT,
	ACTION_SWAP_SCREENS,
	ACTION_GALLERY,
	ACTION_HOT_PIXEL_FIX,
	ACTION_LOOK_EDITOR,
	ACTION_FAST_LIVE_VIEW
} RowAction;

typedef struct {
	const char *label; // full name (menu rows, Q value bar)
	const char *tag; // short name on a Q tile
	RowKind kind;
	size_t offset; // field in Settings
	int min;
	int max;
	int defaultValue;
	int bias; // shown value = stored value - bias
	const char *const *names; // full value names (ROW_ENUM / ROW_BOOL)
	const char *const *shortNames; // Q tile value names (NULL: use names)
	RowAction action;
} SettingRow;

#define SETTING_OFFSET(field) offsetof(Settings, field)

// The Q menu: 4 x 4 tiles in reading order. The top row sets up the shot: camera, mode, the mode's own option
// and the 60 fps live view. The bottom row is what comes out: format, look, size and timer. The option tile
// follows the mode. The 60 fps tile is a switch row: it shows g_fastLiveView, not a Settings field.
#define QUICK_TILE_COUNT 16
#define QUICK_TILE_MODE 1
#define QUICK_TILE_OPTION 2
#define QUICK_TILE_FAST_LIVE_VIEW 3
#define QUICK_TILE_SHUTTER 4
#define QUICK_TILE_EV 5
#define QUICK_TILE_EXPOSURE_MODE 6
#define QUICK_TILE_ISO 9
#define QUICK_TILE_LOOK 13

typedef struct {
	const char *name;
	const SettingRow *rows;
	int count;
} SettingPage;

// The pages of the setup menu.
enum {
	SETTINGS_PAGE_SHOOTING,
	SETTINGS_PAGE_DISPLAY,
	SETTINGS_PAGE_PRESETS,
	SETTINGS_PAGE_SYSTEM,
	SETTINGS_PAGE_COUNT
};

#define HOMEBREW_DIR "sdmc:/3ds"
#define APP_DIR HOMEBREW_DIR "/procam"
#define FOLDER_MODE 0777

// The settings of one shutter press, copied under the camera control lock. A capture can take seconds while
// the UI stays live, so the shot reads only this copy: a change in the middle of a shot must not reach it.
typedef struct {
	Settings settings;
	// The viewing white balance at the press (the DNG's as-shot neutral, the JPG's).
	float whiteBalance[RGB_CHANNELS];
	bool hotPixelFix;
} ShotSettings;

#define PHOTO_DIR "sdmc:/DCIM/procam"

extern Settings g_settings;
extern bool g_screensSwapped; // viewfinder on the touch screen, controls on the top screen
extern bool g_isNew3ds; // ZL, ZR and the C-stick exist
extern bool g_hotPixelFix; // fix hot pixels in the raw (DNG) too
extern bool g_fastLiveView; // raw, PHOTO mode: the 60 fps live view
extern const char *const CAMERA_SHORT_NAMES[CAMERA_CHOICE_COUNT];
extern const char *const MODE_SHORT_NAMES[MODE_COUNT];
extern const char *const BRACKET_SERIES_NAMES[BRACKET_SERIES_CHOICE_COUNT];
extern const char *const FRAME_RATE_SHORT_NAMES[CAM_FRAME_RATE_COUNT];
extern const char *const OUTPUT_FORMAT_SHORT_NAMES[OUTPUT_FORMAT_COUNT];
extern const char *const METERING_SHORT_NAMES[CAM_METERING_WINDOW_COUNT];
extern const char *const WHITE_BALANCE_SHORT_NAMES[CAM_WHITE_BALANCE_COUNT];
extern SettingRow g_quickRows[QUICK_TILE_COUNT];
extern const SettingPage SETTINGS_PAGES[SETTINGS_PAGE_COUNT];

void settings_defaults(Settings *settings);
// The setup menu row of the auto exposure lock, which L also toggles.
const SettingRow *settings_ae_lock_row(void);
// A row of a CamSettings field: the camera thread applies it.
bool settings_row_is_camera_setting(const SettingRow *row);
// A value row that shows an app switch instead of a Settings field (the 60 fps tile). Its offset means
// nothing: read it with settings_row_get and change it through its action.
bool settings_row_is_switch(const SettingRow *row);
// HDR's option row: hdrGap and burstFrames in one value. Read and write it with settings_row_get / _set.
bool settings_row_is_hdr(const SettingRow *row);
int settings_row_get(const SettingRow *row, Settings *settings);
void settings_row_set(const SettingRow *row, Settings *settings, int value);
void settings_load(void);
void settings_save(void);
// Stores the settings in a preset slot and saves the file.
void settings_save_preset(int slot);
// Loads a preset slot into the settings. False when the slot is empty.
bool settings_load_preset(int slot);
// HDR's option value: the index into the burst frame choices it holds.
int settings_hdr_option_burst_index(int value);
// The frames per camera of a burst frame choice.
int settings_burst_frames_per_camera(int index);
// The short names of the HDR gap and of the bracket series these settings hold.
const char *settings_hdr_gap_name(const Settings *settings);
const char *settings_bracket_series_name(const Settings *settings);
// The name of one stored value of a row. shortForm: the name on a Q tile.
void settings_value_name(const SettingRow *row, int stored, char *out, size_t outSize, bool shortForm);
// The name of the row's current value.
void settings_row_value(const SettingRow *row, char *out, size_t outSize, bool shortForm);
// Puts the current mode's option on the QUICK_TILE_OPTION tile. The main loop calls it before input and
// drawing.
void settings_update_mode_row(void);
// Loads the looks and gives the LOOK tile their names and range. Call it before settings_load.
void settings_load_looks(void);
// Gives the LOOK tile the range of the current looks, after one is added.
void settings_refresh_look_tile(void);
