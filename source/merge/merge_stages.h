// Runs the merge's stages. Each stage is a list of independent items, such as a frame or a row of tiles. An
// atomic counter hands them out to the caller and its workers. No item depends on the thread that runs it, so
// the output is the same for any thread count.
#pragma once
#include "merge_common.h"

// One progress stage fills this many percent of the progress.
#define MERGE_STAGE_PERCENT 50

// Runs items 0..itemCount-1 of one stage on all threads and returns when every item is done. A worker that
// cannot be started leaves its share to the others.
void merge_stages_run(MergeContext *context, int itemCount, MergeItemFn runItem);
// The next itemCount items move the progress from startPercent on.
void merge_stages_start_progress(MergeContext *context, int itemCount, int startPercent);
// One item of the progress stage is done.
void merge_stages_item_done(MergeContext *context);
