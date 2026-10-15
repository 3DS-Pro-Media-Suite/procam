// The looks Pro Cam ships: their file names and recipes.
#pragma once

#define BUILTIN_LOOK_COUNT 19

typedef struct {
	const char *fileName;
	const char *recipe;
} BuiltinLook;

extern const BuiltinLook BUILTIN_LOOKS[];
