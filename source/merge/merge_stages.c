#include "merge_stages.h"
#include <stdatomic.h>
#include <stddef.h>

typedef struct {
	MergeContext *context;
	int workerIndex;
} WorkerArg;

// Two threads may finish items at once, so the progress only moves forwards.
static void raise_progress(MergeJob *job, int percent)
{
	if (!job->progress) {
		return;
	}
	int current = *job->progress;
	while (percent > current && !atomic_compare_exchange_weak(job->progress, &current, percent)) {
	}
}

static void run_items(MergeContext *context, MergeWorker *worker)
{
	for (;;) {
		int item = __atomic_fetch_add(&context->next, 1, __ATOMIC_RELAXED);
		if (item >= context->count) {
			break;
		}
		context->item(context, worker, item);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST); // this thread's results are visible before its join returns
}

static void worker_main(void *arg)
{
	WorkerArg *workerArg = arg;
	run_items(workerArg->context, &workerArg->context->workers[workerArg->workerIndex]);
}

void merge_stages_run(MergeContext *context, int itemCount, MergeItemFn runItem)
{
	if (itemCount <= 0) {
		return;
	}
	context->item = runItem;
	context->count = itemCount;
	context->next = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST); // the stage's inputs are visible to the workers it starts
	WorkerArg workerArgs[MERGE_MAX_THREADS];
	void *handles[MERGE_MAX_THREADS] = {0};
	int threadCount = context->threadCount < itemCount ? context->threadCount : itemCount;
	int started = 1; // the caller
	for (int k = 1; k < threadCount; k++) {
		workerArgs[k].context = context;
		workerArgs[k].workerIndex = k;
		handles[k] = context->job->spawn(worker_main, &workerArgs[k], k);
		started += handles[k] != NULL;
	}
	run_items(context, &context->workers[0]);
	for (int k = 1; k < threadCount; k++) {
		if (handles[k]) {
			context->job->join(handles[k]);
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (started > context->threadsUsed) {
		context->threadsUsed = started;
	}
}

void merge_stages_start_progress(MergeContext *context, int itemCount, int startPercent)
{
	context->done = 0;
	context->total = itemCount;
	context->base = startPercent;
}

void merge_stages_item_done(MergeContext *context)
{
	int done = __atomic_add_fetch(&context->done, 1, __ATOMIC_RELAXED);
	raise_progress(context->job, context->base + MERGE_STAGE_PERCENT * done / context->total);
}
