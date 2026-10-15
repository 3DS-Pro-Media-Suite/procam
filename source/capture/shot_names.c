#include "shot_names.h"
#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"

// Shot names are PHOTO_DIR/IMG_nnnn with an extension, or PHOTO_DIR/IMG_nnnn for the folder of a multi-frame
// shot or a merged photo's base name. The first number is the larger of the counter file and one past the
// highest number in the photo folder, both read once on the saver thread. After that numbers come from
// memory, so taking a shot makes no file-system call: any such call can wait seconds behind a write.
#define DCIM_DIR "sdmc:/DCIM"

// The counter keeps names unique when the photo folder has been emptied. It has two copies, written in turn:
// opening a file for writing empties it, and a write cut short there would lose the number. Loading takes
// the larger copy.
#define COUNTER_PATH APP_DIR "/counter.txt"
#define COUNTER_BACKUP_PATH APP_DIR "/counter.bak"

char g_lastShot[SHOT_NAME_SIZE];

// The camera thread takes shot numbers, and the main thread when there is no camera thread.
static LightLock g_shotNumberLock;
static int g_nextShotNumber = 1;
static LightEvent g_shotNumbersReady; // sticky: the photo folder has been scanned
static int g_savedCounter; // the number the counter files hold (0: none read or written yet)
static bool g_writeBackupNext; // the backup copy holds the older number, so it is written next

// The number one counter copy holds, or 0 when it is missing or holds nonsense.
static int read_counter_copy(const char *path)
{
	FILE *file = fopen(path, "r");
	int number = 0;
	if (file) {
		if (fscanf(file, "%d", &number) != 1 || number < 1 || number > SHOT_NUMBER_MAX) {
			number = 0;
		}
		fclose(file);
	}
	return number;
}

// The larger of the two copies, or 0 when neither is readable.
static int load_counter(void)
{
	int mainCopy = read_counter_copy(COUNTER_PATH);
	int backupCopy = read_counter_copy(COUNTER_BACKUP_PATH);
	if (mainCopy >= backupCopy) {
		g_writeBackupNext = true;
	} else {
		g_writeBackupNext = false;
	}
	return mainCopy > backupCopy ? mainCopy : backupCopy;
}

// The number goes into the copy with the older number, so a write cut short leaves the newer one.
void shot_names_save_counter(void)
{
	LightLock_Lock(&g_shotNumberLock);
	int next = g_nextShotNumber;
	LightLock_Unlock(&g_shotNumberLock);
	if (next == g_savedCounter || !LightEvent_TryWait(&g_shotNumbersReady)) {
		return;
	}
	static bool foldersMade;
	if (!foldersMade) {
		foldersMade = true;
		mkdir(HOMEBREW_DIR, FOLDER_MODE);
		mkdir(APP_DIR, FOLDER_MODE);
	}
	const char *path = g_writeBackupNext ? COUNTER_BACKUP_PATH : COUNTER_PATH;
	FILE *file = fopen(path, "w");
	bool ok = file && fprintf(file, "%d\n", next) > 0;
	if (file) {
		ok = fclose(file) == 0 && ok;
	}
	if (ok) {
		g_savedCounter = next;
		g_writeBackupNext = !g_writeBackupNext;
	}
}

// One past the highest shot number in the photo folder: any IMG_nnnn file or folder counts.
static int scan_photo_folder(void)
{
	int next = 1;
	DIR *folder = opendir(PHOTO_DIR);
	if (!folder) {
		return next;
	}
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL) {
		int number;
		bool isShot = sscanf(entry->d_name, SHOT_NAME_PREFIX "%d", &number) == 1;
		if (isShot && number >= next && number <= SHOT_NUMBER_MAX) {
			next = number + 1;
		}
	}
	closedir(folder);
	return next;
}

void shot_names_init(void)
{
	LightLock_Init(&g_shotNumberLock);
	LightEvent_Init(&g_shotNumbersReady, RESET_STICKY);
}

// Saver thread, first thing, or saver_init when there is no saver thread.
void shot_names_scan(void)
{
	mkdir(DCIM_DIR, FOLDER_MODE);
	mkdir(PHOTO_DIR, FOLDER_MODE);
	int next = scan_photo_folder();
	int stored = load_counter();
	g_savedCounter = stored;
	if (stored > next) {
		next = stored;
	}
	LightLock_Lock(&g_shotNumberLock);
	if (next > g_nextShotNumber) {
		g_nextShotNumber = next;
	}
	LightLock_Unlock(&g_shotNumberLock);
	LightEvent_Signal(&g_shotNumbersReady);
}

int shot_names_take_number(void)
{
	// A shot right after the start waits for the folder scan.
	if (!LightEvent_TryWait(&g_shotNumbersReady)) {
		LightEvent_Wait(&g_shotNumbersReady);
	}
	LightLock_Lock(&g_shotNumberLock);
	int number = g_nextShotNumber;
	g_nextShotNumber++;
	LightLock_Unlock(&g_shotNumberLock);
	snprintf(g_lastShot, sizeof g_lastShot, SHOT_NAME_FORMAT, number);
	return number;
}

int shot_names_last_taken(void)
{
	LightLock_Lock(&g_shotNumberLock);
	int number = g_nextShotNumber - 1;
	LightLock_Unlock(&g_shotNumberLock);
	return number;
}

void shot_names_path(int number, const char *suffix, char *path, size_t size)
{
	snprintf(path, size, PHOTO_DIR "/" SHOT_NAME_FORMAT "%s", number, suffix);
}

int shot_names_number_of_path(const char *path)
{
	static const char prefix[] = PHOTO_DIR "/" SHOT_NAME_PREFIX;
	size_t prefixLength = sizeof prefix - 1;
	int number;
	if (strncmp(path, prefix, prefixLength) || sscanf(path + prefixLength, "%d", &number) != 1) {
		return -1;
	}
	return number;
}
