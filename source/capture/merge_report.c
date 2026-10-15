#include "merge_report.h"
#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raw10.h"
#include "sensor_exposure.h"
#include "settings.h"
#include "still_capture.h"
#include "time_units.h"
#include "toast.h"

#define SUMMARY_SIZE 1024
#define MERGE_LOG_EXTRA 1024 // room for the lines around the merge's own log
#define HELPER_TEXT_SIZE 24
#define PERCENT 100

// Appends formatted text to a fixed buffer, never past its end. Returns the new length.
static int append_text(char *buffer, size_t capacity, int length, const char *format, ...)
{
	if (length < 0 || (size_t)length >= capacity) {
		return length;
	}
	va_list args;
	va_start(args, format);
	int written = vsnprintf(buffer + length, capacity - length, format, args);
	va_end(args);
	if (written < 0) {
		return length;
	}
	if ((size_t)(length + written) >= capacity) {
		return (int)capacity - 1;
	}
	return length + written;
}

static const char *capture_method(const MergeRequest *request)
{
	return request->captureMethod ? request->captureMethod : "?";
}

// Equal times (A's capped at B's) still merge B as HDR.
MergeOutcome merge_report_outcome(const MergeRequest *request, const MergeResult *result, bool merged)
{
	MergeOutcome outcome;
	outcome.merged = merged;
	outcome.hdrUsed = merged && request->hdr && result->hdrUsed;
	outcome.usedB = merged && request->frameCountB > 0 && (result->useB > 0 || outcome.hdrUsed);
	outcome.white = merged && result->white > 0 ? result->white : MERGE_SCALE * RAW10_WHITE;
	float blackSum = 0;
	for (int cell = 0; cell < CFA_CELLS; cell++) {
		blackSum += request->blackLevelsA[cell];
	}
	outcome.black = MERGE_SCALE * blackSum / CFA_CELLS;
	outcome.meteredTime = request->meteredLines + request->fineLines;
	outcome.timeA = request->linesA + request->fineLines;
	outcome.timeB = request->linesB + request->fineLines;
	outcome.overMetered = request->hdr ? outcome.timeA / outcome.meteredTime : 1;
	outcome.range = 1;
	if (outcome.hdrUsed) {
		float meteredWhite = MERGE_SCALE * RAW10_WHITE - outcome.black;
		outcome.range = (outcome.white - outcome.black) / (meteredWhite * outcome.overMetered);
		if (outcome.range < 1) {
			outcome.range = 1;
		}
	}
	outcome.exposureSeconds = request->exposureSeconds;
	if (outcome.hdrUsed) {
		outcome.exposureSeconds =
		        outcome.meteredTime * sensor_exposure_line_microseconds() / (float)MICROSECONDS_PER_SECOND;
	}
	return outcome;
}

// The HDR lines of the summary: both cameras' times and what the merge made of them.
static int append_hdr_summary(char *text, size_t capacity, int length, const MergeRequest *request,
                              const MergeResult *result, const MergeOutcome *outcome)
{
	float evA = log2f(outcome->overMetered);
	float evB = log2f(outcome->timeB / outcome->meteredTime);
	float lineSeconds = sensor_exposure_line_microseconds() / (float)MICROSECONDS_PER_SECOND;
	float gapEv = log2f(outcome->timeA / outcome->timeB);
	length = append_text(
	        text, capacity, length,
	        "HDR (gap setting %d EV): camera A %u lines (%.5f s, %+.2f EV), camera B %u lines "
	        "(%.5f s, %+.2f EV), metered %u lines (times: + %.2f line each), gain %d on both: %.2f "
	        "EV apart\n",
	        request->hdrGapEv, request->linesA, outcome->timeA * lineSeconds, evA, request->linesB,
	        outcome->timeB * lineSeconds, evB, request->meteredLines, request->fineLines, request->gain,
	        gapEv);
	if (outcome->hdrUsed) {
		float rolledOffEv = log2f(outcome->range);
		length =
		        append_text(text, capacity, length,
		                    "HDR: white %d (x%.2f of camera A's clip); %.1f %% of the samples above A's clip "
		                    "(from camera B), %.2f %% left at white; exposure time: the metered one; JPG: "
		                    "rendered for the metered exposure, "
		                    "%.2f EV of highlights above its white rolled off",
		                    outcome->white, result->hdrRatio, PERCENT * result->hdrAbove,
		                    PERCENT * result->hdrWhite, rolledOffEv);
		if (request->photoFormat != PHOTO_FORMAT_JPG_ONLY) {
			length = append_text(text, capacity, length, "; DNG BaselineExposure %+.2f EV", rolledOffEv);
		}
		return append_text(text, capacity, length, "\n");
	}
	if (outcome->merged) {
		return append_text(
		        text, capacity, length,
		        "HDR: camera B not used (its levels did not match the exposure ratio): camera A "
		        "alone, %+.1f EV over the metered exposure, highlights clipped as in a plain photo\n",
		        evA);
	}
	return length;
}

char *merge_report_summary(const MergeRequest *request, const MergeResult *result,
                           const MergeOutcome *outcome)
{
	char text[SUMMARY_SIZE];
	int length = 0;
	if (!outcome->merged) {
		length = append_text(text, sizeof text, length,
		                     "\nmerge FAILED (out of memory?): this is frame A_00 alone, x%d (white %d)\n",
		                     MERGE_SCALE, outcome->white);
	} else if (outcome->usedB) {
		length = append_text(
		        text, sizeof text, length,
		        "\nmerged: %d+%d frames (%s), reference A_%02d, weight A %.2f B %.2f, B gain %.3f %.3f "
		        "%.3f %.3f offset %.1f %.1f %.1f %.1f, %d defects fixed; values x%d (white %d)\n",
		        request->frameCountA, request->frameCountB, capture_method(request), result->referenceIndex,
		        result->useA, result->useB, result->levelGain[0], result->levelGain[1], result->levelGain[2],
		        result->levelGain[3], result->levelOffset[0], result->levelOffset[1], result->levelOffset[2],
		        result->levelOffset[3], result->fixedDefects, MERGE_SCALE, outcome->white);
	} else {
		const char *droppedB = request->frameCountB ? ", camera B dropped (its levels did not match)" : "";
		length = append_text(
		        text, sizeof text, length,
		        "\nmerged: %d frames of camera A (%s)%s, reference A_%02d, weight %.2f, %d defects "
		        "fixed; values x%d (white %d)\n",
		        request->frameCountA, capture_method(request), droppedB, result->referenceIndex, result->useA,
		        result->fixedDefects, MERGE_SCALE, outcome->white);
	}
	if (request->hdr) {
		length = append_hdr_summary(text, sizeof text, length, request, result, outcome);
	}
	if (request->note[0]) {
		length = append_text(text, sizeof text, length, "%s\n", request->note);
	}
	return strdup(text);
}

static const char *camera_b_use(const MergeRequest *request, const MergeOutcome *outcome)
{
	if (outcome->usedB || outcome->hdrUsed) {
		return "used";
	}
	if (request->frameCountB) {
		return "not used";
	}
	return "not captured";
}

char *merge_report_log(const MergeRequest *request, const MergeResult *result, const MergeOutcome *outcome,
                       const MergeRun *run)
{
	size_t capacity = strlen(run->mergeLog) + MERGE_LOG_EXTRA;
	char *text = malloc(capacity);
	if (!text) {
		return NULL;
	}
	const RawShotInfo *info = &request->info;
	int length = append_text(text, capacity, 0, "\nmerge log:\n");
	length = append_text(
	        text, capacity, length,
	        "capture: %d+%d frames via %s in %.2f s; arrived A %d B %d, buffer errors A %d B %d; "
	        "frame-done skew mean %.2f ms, max %.2f ms; %u lines, gain %u\n",
	        request->frameCountA, request->frameCountB, capture_method(request),
	        request->captureMs / (double)MILLISECONDS_PER_SECOND, info->received[RAW_SHOT_CAMERA_A],
	        info->received[RAW_SHOT_CAMERA_B], info->bufferError[RAW_SHOT_CAMERA_A],
	        info->bufferError[RAW_SHOT_CAMERA_B], info->skewMs, info->skewMaxMs, info->lines, info->gain);
	if (request->hdr) {
		// What the sensors ran: the times written and read back, and the frame length.
		length = append_text(
		        text, capacity, length,
		        "HDR burst: lines A %u B %u (read back %u / %u), frame %u / %u lines, pairs from "
		        "one moment %u (claimed again %u)\n",
		        info->cameraLines[RAW_SHOT_CAMERA_A], info->cameraLines[RAW_SHOT_CAMERA_B],
		        info->linesBack[RAW_SHOT_CAMERA_A], info->linesBack[RAW_SHOT_CAMERA_B],
		        info->timing[RAW_SHOT_CAMERA_A][RAW_SHOT_FRAME_LENGTH],
		        info->timing[RAW_SHOT_CAMERA_B][RAW_SHOT_FRAME_LENGTH], info->paired, info->reclaims);
	}
	char helper[HELPER_TEXT_SIZE] = "";
	if (result->threads > 1) {
		snprintf(helper, sizeof helper, " + helper core %d", run->helperCore);
	}
	const char *threadPlural = result->threads == 1 ? "" : "s";
	append_text(text, capacity, length,
	            "camera B: %s\n%stime: %.1f s on core %d%s, %d thread%s (align %lu ms, merge %lu ms)\n",
	            camera_b_use(request, outcome), run->mergeLog,
	            run->elapsedMs / (double)MILLISECONDS_PER_SECOND, run->core, helper, result->threads,
	            threadPlural, (unsigned long)result->alignMs, (unsigned long)result->mergeMs);
	return text;
}

void merge_report_toast(const char *name, const MergeRequest *request, const MergeResult *result,
                        const MergeOutcome *outcome)
{
	if (!outcome->merged) {
		toast_show("Merge failed: saved one frame");
	} else if (outcome->hdrUsed) {
		toast_show("%s HDR merged (%.1f EV wider), saving", name, log2f(result->hdrRatio));
	} else if (request->hdr) {
		toast_show("%s: HDR's camera B did not match, saved camera A", name);
	} else if (request->hdrWanted) {
		toast_show("%s: no HDR possible, merged at one exposure", name);
	} else if (outcome->usedB) {
		toast_show("%s merged (B %d %%), saving", name, (int)(result->useB * PERCENT + 0.5f));
	} else {
		toast_show("%s merged (camera A only), saving", name);
	}
}
