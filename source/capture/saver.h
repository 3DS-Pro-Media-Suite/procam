// The save queue and its thread.
#pragma once
#include <3ds.h>
#include "photo_writer.h"

extern _Atomic int g_shotsSaved;
extern _Atomic u64 g_lastSavedAt; // when the saver last finished a file

// Reserves the next shot's path: its PNG, or its folder.
void saver_next_shot_path(char *path, size_t size, bool isFolder);
void saver_init(void);
bool saver_shot_saving(int shotNumber);
void saver_exit(void);
void saver_queue(SaveJob *job);
bool saver_idle(void);
bool saver_full(void);
int saver_take_failures(void);
// Photos the card still has room for, -1 until known. Each photo saved counts down.
int saver_shots_left(void);
void saver_set_shots_left(int shotsLeft);
