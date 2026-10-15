#include "shot_info.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cam_settings.h"
#include "math_util.h"
#include "merge.h"

// Multi-frame shots live in a folder IMG_nnnn/ whose INFO.TXT starts with one machine-readable line, then
// the sidecar text:
//   procam-shot kind=3d frames=1 stereo=1 hdr=0 evA=0 evB=0 eyes=1 settings=<CamSettings as hex>
// kind is bracket (EV-2.DNG / .JPG ...) or 3d (L / R).
#define SHOT_INFO_PREFIX "procam-shot "
#define SHOT_INFO_PREFIX_LENGTH (sizeof SHOT_INFO_PREFIX - 1)
#define FIELD_SEPARATORS " \r\n"
#define SHOT_INFO_LINE_SIZE 512
#define SHOT_INFO_FIELDS_SIZE 160 // room for the first line without the settings
#define HEX_DIGITS_PER_BYTE 2
#define SHOT_INFO_MAX_FRAMES MERGE_MAX_FRAMES // frames per camera: no shot takes more

char *shot_info_text(const char *kind, int frames, bool stereo, bool hdr, int evA, int evB,
                     const CamSettings *settings, const char *sidecar)
{
	size_t settingsHexSize = sizeof *settings * HEX_DIGITS_PER_BYTE;
	size_t capacity = SHOT_INFO_FIELDS_SIZE + settingsHexSize + (sidecar ? strlen(sidecar) : 0);
	char *text = malloc(capacity);
	if (!text) {
		return NULL;
	}
	int length = snprintf(
	        text, capacity,
	        SHOT_INFO_PREFIX "kind=%s frames=%d stereo=%d hdr=%d evA=%d evB=%d eyes=1 settings=", kind,
	        frames, stereo, hdr, evA, evB);
	const u8 *settingsBytes = (const u8 *)settings;
	for (size_t i = 0; i < sizeof *settings; i++) {
		length += snprintf(text + length, capacity - length, "%02X", settingsBytes[i]);
	}
	snprintf(text + length, capacity - length, "\n%s", sidecar ? sidecar : "");
	return text;
}

static void read_shot_field(ShotInfo *info, const char *key, const char *value)
{
	if (!strcmp(key, "kind")) {
		snprintf(info->kind, sizeof info->kind, "%s", value);
	} else if (!strcmp(key, "frames")) {
		info->frames = atoi(value);
	}
}

bool shot_info_read(const char *path, ShotInfo *info)
{
	memset(info, 0, sizeof *info);
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	char line[SHOT_INFO_LINE_SIZE];
	bool isShotInfo =
	        fgets(line, sizeof line, file) && !strncmp(line, SHOT_INFO_PREFIX, SHOT_INFO_PREFIX_LENGTH);
	fclose(file);
	if (!isShotInfo) {
		return false;
	}
	char *fields = line + SHOT_INFO_PREFIX_LENGTH;
	for (char *field = strtok(fields, FIELD_SEPARATORS); field; field = strtok(NULL, FIELD_SEPARATORS)) {
		char *equals = strchr(field, '=');
		if (!equals) {
			continue;
		}
		*equals = '\0';
		read_shot_field(info, field, equals + 1);
	}
	info->frames = math_clamp_int(info->frames, 0, SHOT_INFO_MAX_FRAMES);
	return true;
}
