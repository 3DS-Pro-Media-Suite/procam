// Looks turn a raw frame into the JPG and the live view. Each look is a recipe file in the looks folder:
// a "name:" and a "short:" line, then one processing step per line, run in the order written.
// The hotpixel step runs on the raw data before demosaicing. The other steps run on RGB.
// RGB is linear until a "tone" step moves it to display values. A look without one gets gamma 2.2 at the end.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define LOOKS_MAX 40
#define LOOK_INDEX_STANDARD 0 // the first look, the built-in Standard one
#define LOOK_OPS_MAX 24
#define LOOK_PARAMS_MAX 16
#define LOOK_NAME_SIZE 32
#define LOOK_SHORT_NAME_SIZE 12
#define LOOK_FILE_NAME_SIZE 64
// A split tone step holds the shadow tint (r, g, b), then the highlight tint.
#define LOOK_SPLIT_TONE_SHADOW_VALUES 3

// The parameters and help text of each step are in LOOK_OP_INFO.
typedef enum {
	LOOK_OP_HOT_PIXEL,
	LOOK_OP_EXPOSURE,
	LOOK_OP_WB_SHIFT,
	LOOK_OP_MATRIX,
	LOOK_OP_SATURATION,
	LOOK_OP_MONO,
	LOOK_OP_TONE,
	LOOK_OP_CURVE,
	LOOK_OP_SPLIT_TONE,
	LOOK_OP_VIGNETTE,
	LOOK_OP_SHARPEN,
	LOOK_OP_GRAIN,
	LOOK_OP_KINDS
} LookOpKind;

// What a step is called and which parameters it takes. README.txt and the recipe editor are built from it.
typedef struct {
	const char *name;
	int paramCount;
	const char *paramNames[LOOK_PARAMS_MAX];
	float defaults[LOOK_PARAMS_MAX];
	float min[LOOK_PARAMS_MAX];
	float max[LOOK_PARAMS_MAX];
	float step[LOOK_PARAMS_MAX];
	const char *help;
} LookOpInfo;

extern const LookOpInfo LOOK_OP_INFO[LOOK_OP_KINDS];

// The live view runs only the per-pixel steps, and without the vignette. The rest runs on the JPG.
bool look_op_runs_in_live_view(LookOpKind kind);

typedef struct {
	LookOpKind kind;
	int count;
	float values[LOOK_PARAMS_MAX];
} LookOp;

typedef struct {
	char name[LOOK_NAME_SIZE];
	char shortName[LOOK_SHORT_NAME_SIZE];
	char file[LOOK_FILE_NAME_SIZE];
	LookOp ops[LOOK_OPS_MAX];
	int opCount;
} Look;

// Loads every *.look file in dir, sorted by file name. An empty dir first gets the built-in looks and a
// README.txt. Returns the number of looks. When the folder cannot be used, the built-ins come from memory.
int look_load_all(const char *dir);
int look_count(void);
// An index out of range is clamped.
const Look *look_get(int index);
// Both lists have look_count() entries.
const char *const *look_names(void);
const char *const *look_short_names(void);

// Parses one recipe text. Returns false when the recipe has no name.
bool look_parse(const char *text, Look *out);
// Writes the recipe text a look is saved as. Returns its length.
int look_serialize(const Look *look, char *out, size_t outSize);
// A step's values as its recipe line writes them: "0.35 0 0.25", "0:0 1:1", "0 0 0.01 / 0.01 0 0". Stops
// adding values once fewer than roomForValue bytes are left. Returns the length.
int look_format_values(const LookOp *step, char *out, size_t outSize, int roomForValue);

// Replaces a look in memory. It keeps its file name.
void look_set(int index, const Look *look);
// Writes a look to its file.
bool look_save(int index);
// Adds a look and saves it as user-NN.look. Returns its index, or -1 on failure.
int look_add(const Look *look);

// Describes a look for a photo's sidecar file: its name, its file and its steps.
int look_describe(int index, char *out, size_t outSize);
