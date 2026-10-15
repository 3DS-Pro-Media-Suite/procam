// The screen shown at exit while the waiting MERGE and HDR shots are merged and saved.
#pragma once
#include "screens.h"

// No merge thread may still read its buffers when main() returns and the heap goes away. The frames of a
// waiting MERGE or HDR shot exist only in memory, so every one is merged and saved first, with the progress
// on screen.
void exit_screen_finish_merges(const Screens *screens);
