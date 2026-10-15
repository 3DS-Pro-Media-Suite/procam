// libctru's thread defaults, shared by the app and its libraries.
#pragma once

#define MAIN_THREAD_PRIORITY 0x30 // libctru's main thread: used when its priority cannot be read
#define THREAD_PRIORITY_LOWEST 0x3F // the largest priority value an application thread may have
#define THREAD_CORE_APP (-2) // libctru: the application's default core
