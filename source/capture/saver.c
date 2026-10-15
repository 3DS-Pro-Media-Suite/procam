#include "saver.h"
#include <3ds.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "app_threads.h"
#include "camera_control.h"
#include "photo_writer.h"
#include "shot_names.h"
#include "time_units.h"

#define SAVE_QUEUE_SIZE 8
#define SAVER_STACK_SIZE (64 * 1024)
#define SAVER_PRIORITY_BELOW_CALLER 1 // a larger number is a lower priority
#define QUEUE_POLL_NS (10 * NANOSECONDS_PER_MILLISECOND) // how often a waiting caller checks the queue
#define PNG_SUFFIX ".PNG" // the writer swaps the extension for each file it writes

typedef struct {
	SaveJob jobs[SAVE_QUEUE_SIZE];
	int head;
	_Atomic int count; // queued or being written; changed under lock
	LightLock lock;
	LightEvent wake;
	_Atomic bool quit;
	Thread thread;
	_Atomic int failedCount; // jobs that could not be written; the main loop reports them
} SaveQueue;

_Atomic int g_shotsSaved;
_Atomic u64 g_lastSavedAt;

static SaveQueue g_saveQueue;
static _Atomic int g_shotsLeft = -1;

// A number has been given out: the saver thread writes the counter on its next pass, well before the
// shot's files are queued. Without a saver thread every save runs on the caller's thread anyway.
static void request_counter_save(void)
{
	if (g_saveQueue.thread) {
		LightEvent_Signal(&g_saveQueue.wake);
	} else {
		shot_names_save_counter();
	}
}

void saver_next_shot_path(char *path, size_t size, bool isFolder)
{
	int number = shot_names_take_number();
	request_counter_save();
	shot_names_path(number, isFolder ? "" : PNG_SUFFIX, path, size);
}

static void free_job_buffers(SaveJob *job)
{
	free(job->frameA);
	free(job->frameB);
	free(job->note);
	free(job->mergeLog);
}

// A written photo takes room on the card.
static void record_job_result(const SaveJob *job, bool ok)
{
	if (!ok) {
		g_saveQueue.failedCount++;
		return;
	}
	if (job->kind == SAVE_TEXT) {
		return;
	}
	g_shotsSaved++;
	if (g_shotsLeft > 0) {
		g_shotsLeft--;
	}
}

// Writes jobs until the queue is empty.
static void write_queued_jobs(void)
{
	for (;;) {
		// The counter first: a few ms.
		shot_names_save_counter();
		LightLock_Lock(&g_saveQueue.lock);
		bool hasJob = g_saveQueue.count > 0;
		SaveJob *job = hasJob ? &g_saveQueue.jobs[g_saveQueue.head] : NULL;
		LightLock_Unlock(&g_saveQueue.lock);
		if (!hasJob) {
			return;
		}
		bool ok = photo_writer_write(job);
		free_job_buffers(job);
		record_job_result(job, ok);
		LightLock_Lock(&g_saveQueue.lock);
		g_saveQueue.head = (g_saveQueue.head + 1) % SAVE_QUEUE_SIZE;
		// Only now: wait_until_saved waits for the count to reach 0, and the slot is free again.
		g_saveQueue.count--;
		LightLock_Unlock(&g_saveQueue.lock);
		g_lastSavedAt = osGetTime();
	}
}

// Encoding and writing a photo can take seconds, so this thread does it below the UI's priority.
static void saver_thread(void *arg)
{
	(void)arg;
	shot_names_scan();
	for (;;) {
		LightEvent_Wait(&g_saveQueue.wake);
		write_queued_jobs();
		if (g_saveQueue.quit) {
			break;
		}
	}
}

void saver_init(void)
{
	LightLock_Init(&g_saveQueue.lock);
	shot_names_init();
	LightEvent_Init(&g_saveQueue.wake, RESET_ONESHOT);
	s32 priority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	s32 saverPriority = priority + SAVER_PRIORITY_BELOW_CALLER;
	g_saveQueue.thread =
	        threadCreate(saver_thread, NULL, SAVER_STACK_SIZE, saverPriority, THREAD_CORE_APP, false);
	// Without a saver thread saves run on the calling thread.
	if (!g_saveQueue.thread) {
		shot_names_scan();
	}
}

static void wait_until_saved(void)
{
	while (g_saveQueue.thread && g_saveQueue.count > 0) {
		svcSleepThread(QUEUE_POLL_NS);
	}
}

// The shot may still get files: a save job of it is queued or being written, or it is being taken.
bool saver_shot_saving(int shotNumber)
{
	bool saving = camera_control_shooting_since() && shotNumber == shot_names_last_taken();
	LightLock_Lock(&g_saveQueue.lock);
	for (int i = 0; i < g_saveQueue.count && !saving; i++) {
		int slot = (g_saveQueue.head + i) % SAVE_QUEUE_SIZE;
		saving = shot_names_number_of_path(g_saveQueue.jobs[slot].path) == shotNumber;
	}
	LightLock_Unlock(&g_saveQueue.lock);
	return saving;
}

void saver_exit(void)
{
	if (!g_saveQueue.thread) {
		return;
	}
	wait_until_saved();
	g_saveQueue.quit = true;
	LightEvent_Signal(&g_saveQueue.wake);
	threadJoin(g_saveQueue.thread, U64_MAX);
	threadFree(g_saveQueue.thread);
	g_saveQueue.thread = NULL;
}

// Puts the job into a free slot. False when the queue is full.
static bool try_enqueue(const SaveJob *job)
{
	LightLock_Lock(&g_saveQueue.lock);
	bool hasRoom = g_saveQueue.count < SAVE_QUEUE_SIZE;
	if (hasRoom) {
		int slot = (g_saveQueue.head + g_saveQueue.count) % SAVE_QUEUE_SIZE;
		g_saveQueue.jobs[slot] = *job;
		g_saveQueue.count++;
	}
	LightLock_Unlock(&g_saveQueue.lock);
	return hasRoom;
}

// Takes ownership of the job's buffers. The queue only fills up when the card is slower than the shutter:
// then this waits.
void saver_queue(SaveJob *job)
{
	if (!job->when) {
		job->when = time(NULL);
	}
	if (!g_saveQueue.thread) {
		bool ok = photo_writer_write(job);
		free_job_buffers(job);
		// Without the saver thread the shots-left count waits for the next free-space check.
		if (!ok) {
			g_saveQueue.failedCount++;
		} else if (job->kind != SAVE_TEXT) {
			g_shotsSaved++;
		}
		return;
	}
	// Two producers, the camera thread's shots and the main thread's merges, so the check and the insert
	// happen under one lock. Otherwise both could take the last free slot.
	while (!try_enqueue(job)) {
		svcSleepThread(QUEUE_POLL_NS);
	}
	LightEvent_Signal(&g_saveQueue.wake);
}

// Nothing queued or being written.
bool saver_idle(void)
{
	return g_saveQueue.count == 0;
}

// A new job would wait for a free slot. Never without a saver thread: saves run at once then.
bool saver_full(void)
{
	return g_saveQueue.thread && g_saveQueue.count >= SAVE_QUEUE_SIZE;
}

// Main loop: the jobs that could not be written since the last call.
int saver_take_failures(void)
{
	return atomic_exchange(&g_saveQueue.failedCount, 0);
}

int saver_shots_left(void)
{
	return g_shotsLeft;
}

void saver_set_shots_left(int shotsLeft)
{
	g_shotsLeft = shotsLeft;
}
