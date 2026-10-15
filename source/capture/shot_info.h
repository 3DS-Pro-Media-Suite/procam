// INFO.TXT of a multi-frame shot's folder: one machine-readable line, then the sidecar text.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

#define SHOT_KIND_SIZE 12

// The first line of a multi-frame shot's INFO.TXT.
typedef struct {
	char kind[SHOT_KIND_SIZE];
	int frames; // per camera
	bool dng; // a raw 3D pair: L.DNG and R.DNG
} ShotInfo;

// INFO.TXT for a multi-frame shot: the machine-readable line, then the sidecar text. malloc'd.
char *shot_info_text(const char *kind, int frames, bool stereo, bool hdr, int evA, int evB,
                     const CamSettings *settings, const char *sidecar);
bool shot_info_read(const char *path, ShotInfo *info);
