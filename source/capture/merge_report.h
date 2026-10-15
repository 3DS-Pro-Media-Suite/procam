// What a finished merge tells the user: the sidecar summary, the merge log and the toast.
#pragma once
#include <3ds.h>
#include "background_merge.h"
#include "merge.h"

// What a finished merge produced, for the save job, the sidecar text and the toast. Times are in lines with
// the fine part included.
typedef struct {
	bool merged; // false: the photo is frame A_00 alone
	bool hdrUsed; // an HDR merge with camera B in it
	bool usedB; // camera B's frames are in the photo
	int white; // the mosaic's white level
	float black; // the mosaic's mean black level
	float meteredTime;
	float timeA;
	float timeB;
	float overMetered; // camera A's time over the metered one
	float range; // HDR: the mosaic's white over the metered exposure's white, both above black
	float exposureSeconds; // the time the photo reports. HDR: the metered one
} MergeOutcome;

// Where and how long the merge ran, for the log.
typedef struct {
	const char *mergeLog; // merge_run's own log
	int core;
	int helperCore; // when more than one thread ran
	u64 elapsedMs;
} MergeRun;

// merged: merge_run made a mosaic. HDR counts as used only when merge_run says so.
MergeOutcome merge_report_outcome(const MergeRequest *request, const MergeResult *result, bool merged);
// The summary for the sidecar text, which the DNG's ImageDescription also gets. malloc'd.
char *merge_report_summary(const MergeRequest *request, const MergeResult *result,
                           const MergeOutcome *outcome);
// The whole log, for the TXT only: the capture, the merge's own log and its times. NULL when out of memory.
char *merge_report_log(const MergeRequest *request, const MergeResult *result, const MergeOutcome *outcome,
                       const MergeRun *run);
void merge_report_toast(const char *name, const MergeRequest *request, const MergeResult *result,
                        const MergeOutcome *outcome);
