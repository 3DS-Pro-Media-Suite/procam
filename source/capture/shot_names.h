// Shot names and numbers: IMG_nnnn, counted on from the photo folder and the counter files.
#pragma once
#include <3ds.h>

#define SHOT_NAME_PREFIX "IMG_"
#define SHOT_NAME_FORMAT SHOT_NAME_PREFIX "%04d" // grows to five digits from IMG_10000 on
#define SHOT_NUMBER_MAX 99999
#define SHOT_NAME_SIZE 32

extern char g_lastShot[SHOT_NAME_SIZE]; // the name shot_names_take_number gave out last: "IMG_0042"

void shot_names_init(void);
// Finds the first free number: the larger of the counter files' and one past the highest in the photo folder.
// Called once, on the saver thread.
void shot_names_scan(void);
// Reserves the next number. Waits for shot_names_scan on a shot right after the start.
int shot_names_take_number(void);
// The number given out last, 0 before any.
int shot_names_last_taken(void);
// Writes the counter when it changed since the last write. Saver thread.
void shot_names_save_counter(void);

// A shot's path in the photo folder followed by suffix: "" for its folder, ".JPG" for a file.
void shot_names_path(int number, const char *suffix, char *path, size_t size);
// The shot number of a path in the photo folder ("sdmc:/DCIM/procam/IMG_0042.JPG", ".../IMG_0042/L.DNG"),
// else -1.
int shot_names_number_of_path(const char *path);
