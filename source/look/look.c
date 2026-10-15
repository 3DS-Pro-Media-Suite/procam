#include "look.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "look_builtins.h"

#define LOOKS_DIR_SIZE 64
#define LOOKS_DIR_MODE 0777
#define FILE_PATH_SIZE 128
#define RECIPE_TEXT_SIZE 4096
#define RECIPE_LINE_SIZE 256
#define STEP_NAME_SIZE 24
// A look without a short name shows this many letters of its name on the menu tile.
#define SHORT_NAME_LETTERS 8
// Room kept free in the output before writing another step or value.
#define ROOM_FOR_STEP 64
#define ROOM_FOR_VALUE 16
// The README lists at most this many parameter names per step.
#define README_PARAMS_MAX 9
#define USER_LOOK_NUMBER_MAX 999

const LookOpInfo LOOK_OP_INFO[LOOK_OP_KINDS] = {
        [LOOK_OP_HOT_PIXEL] = {.name = "hotpixel",
                               .paramCount = 1,
                               .paramNames = {"threshold"},
                               .defaults = {48},
                               .min = {0},
                               .max = {400},
                               .step = {4},
                               .help = "replace pixels this far (10-bit) above or below all their "
                                       "same-colour neighbours (raw stage)"},
        [LOOK_OP_EXPOSURE] = {.name = "exposure",
                              .paramCount = 1,
                              .paramNames = {"ev"},
                              .defaults = {0},
                              .min = {-4},
                              .max = {4},
                              .step = {0.1f},
                              .help = "brighten (+) or darken (-) in stops"},
        [LOOK_OP_WB_SHIFT] = {.name = "wb_shift",
                              .paramCount = 2,
                              .paramNames = {"red", "blue"},
                              .defaults = {1, 1},
                              .min = {0.5f, 0.5f},
                              .max = {2, 2},
                              .step = {0.02f, 0.02f},
                              .help = "multiply red and blue on top of the shot's white balance"},
        [LOOK_OP_MATRIX] = {.name = "matrix",
                            .paramCount = 9,
                            .paramNames = {"rr", "rg", "rb", "gr", "gg", "gb", "br", "bg", "bb"},
                            .defaults = {1, 0, 0, 0, 1, 0, 0, 0, 1},
                            .min = {-2, -2, -2, -2, -2, -2, -2, -2, -2},
                            .max = {3, 3, 3, 3, 3, 3, 3, 3, 3},
                            .step = {0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f},
                            .help = "3x3 colour matrix: output red = rr*R + rg*G + rb*B, and so on"},
        [LOOK_OP_SATURATION] = {.name = "saturation",
                                .paramCount = 1,
                                .paramNames = {"amount"},
                                .defaults = {1},
                                .min = {0},
                                .max = {3},
                                .step = {0.02f},
                                .help = "colour intensity (1 = unchanged, 0 = grey)"},
        [LOOK_OP_MONO] = {.name = "mono",
                          .paramCount = 3,
                          .paramNames = {"red", "green", "blue"},
                          .defaults = {0.30f, 0.59f, 0.11f},
                          .min = {0, 0, 0},
                          .max = {1, 1, 1},
                          .step = {0.02f, 0.02f, 0.02f},
                          .help = "black and white from these channel weights (a colour filter)"},
        [LOOK_OP_TONE] = {.name = "tone",
                          .paramCount = 4,
                          .paramNames = {"contrast", "lift", "rolloff", "gamma"},
                          .defaults = {0.35f, 0, 0.25f, 2.2f},
                          .min = {-1, 0, 0, 1},
                          .max = {1.5f, 0.3f, 2, 3},
                          .step = {0.02f, 0.005f, 0.02f, 0.05f},
                          .help = "display tone: S-curve contrast, shadow lift, highlight roll-off, gamma "
                                  "(linear -> display)"},
        [LOOK_OP_CURVE] = {.name = "curve",
                           .paramCount = 16,
                           .paramNames = {"x1", "y1", "x2", "y2", "x3", "y3", "x4", "y4", "x5", "y5", "x6",
                                          "y6", "x7", "y7", "x8", "y8"},
                           .defaults = {0, 0, 0.25f, 0.25f, 0.75f, 0.75f, 1, 1},
                           .min = {0},
                           .max = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                           .step = {0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f,
                                    0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f},
                           .help = "tone curve through x:y points (display values 0..1)"},
        [LOOK_OP_SPLIT_TONE] = {.name = "split",
                                .paramCount = 6,
                                .paramNames = {"shadow r", "shadow g", "shadow b", "high r", "high g",
                                               "high b"},
                                .defaults = {0},
                                .min = {-0.1f, -0.1f, -0.1f, -0.1f, -0.1f, -0.1f},
                                .max = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f},
                                .step = {0.002f, 0.002f, 0.002f, 0.002f, 0.002f, 0.002f},
                                .help = "tints added in the shadows and the highlights"},
        [LOOK_OP_VIGNETTE] = {.name = "vignette",
                              .paramCount = 1,
                              .paramNames = {"amount"},
                              .defaults = {0.3f},
                              .min = {-1},
                              .max = {1},
                              .step = {0.02f},
                              .help = "darken (+) or lighten (-) the corners"},
        [LOOK_OP_SHARPEN] = {.name = "sharpen",
                             .paramCount = 1,
                             .paramNames = {"amount"},
                             .defaults = {0.5f},
                             .min = {0},
                             .max = {3},
                             .step = {0.05f},
                             .help = "unsharp mask on brightness (JPG / test render only)"},
        [LOOK_OP_GRAIN] = {.name = "grain",
                           .paramCount = 1,
                           .paramNames = {"amount"},
                           .defaults = {0.03f},
                           .min = {0},
                           .max = {0.2f},
                           .step = {0.005f},
                           .help = "film grain (JPG / test render only)"},
};

bool look_op_runs_in_live_view(LookOpKind kind)
{
	return kind != LOOK_OP_HOT_PIXEL && kind != LOOK_OP_VIGNETTE && kind != LOOK_OP_SHARPEN &&
	       kind != LOOK_OP_GRAIN;
}

static Look g_looks[LOOKS_MAX];
static int g_lookCount;
static const char *g_lookNames[LOOKS_MAX];
static const char *g_lookShortNames[LOOKS_MAX];
static char g_looksDir[LOOKS_DIR_SIZE];

static bool is_blank(char c)
{
	return c == ' ' || c == '\t';
}

static int find_op_kind(const char *name)
{
	for (int kind = 0; kind < LOOK_OP_KINDS; kind++) {
		if (strcmp(name, LOOK_OP_INFO[kind].name) == 0) {
			return kind;
		}
	}
	return -1;
}

// Copies the next line of text into line, cut to fit, and returns where the following line starts.
static const char *read_line(const char *text, char *line, size_t lineSize)
{
	size_t length = strcspn(text, "\r\n");
	if (length >= lineSize) {
		length = lineSize - 1;
	}
	memcpy(line, text, length);
	line[length] = 0;
	const char *next = text + length;
	while (*next == '\r' || *next == '\n') {
		next++;
	}
	return next;
}

// Cuts off a '#' comment and returns where the text after the indent starts.
static char *strip_comment_and_indent(char *line)
{
	char *comment = strchr(line, '#');
	if (comment) {
		*comment = 0;
	}
	char *text = line;
	while (is_blank(*text)) {
		text++;
	}
	return text;
}

// Returns the text after label when the line starts with it, or NULL.
static char *after_label(char *line, const char *label)
{
	size_t labelLength = strlen(label);
	if (strncmp(line, label, labelLength) != 0) {
		return NULL;
	}
	return line + labelLength;
}

static void copy_trimmed(char *destination, size_t destinationSize, char *value)
{
	while (*value == ' ') {
		value++;
	}
	size_t length = strlen(value);
	while (length > 0 && is_blank(value[length - 1])) {
		length--;
		value[length] = 0;
	}
	snprintf(destination, destinationSize, "%s", value);
}

// Reads a "name:" or "short:" line into the look. Returns false for any other line.
static bool parse_label(char *line, Look *look)
{
	char *name = after_label(line, "name:");
	if (name) {
		copy_trimmed(look->name, sizeof look->name, name);
		return true;
	}
	char *shortName = after_label(line, "short:");
	if (shortName) {
		copy_trimmed(look->shortName, sizeof look->shortName, shortName);
		return true;
	}
	return false;
}

static bool is_value_separator(char c)
{
	return is_blank(c) || c == '/' || c == ':' || c == ',';
}

// Reads the numbers after a step name. "/", ":" and "," separate them like spaces.
static void parse_values(const char *text, LookOp *step)
{
	step->count = 0;
	const char *cursor = text;
	while (*cursor && step->count < LOOK_PARAMS_MAX) {
		while (is_value_separator(*cursor)) {
			cursor++;
		}
		if (!*cursor) {
			return;
		}
		char *numberEnd;
		float value = strtof(cursor, &numberEnd);
		if (numberEnd == cursor) {
			return;
		}
		step->values[step->count] = value;
		step->count++;
		cursor = numberEnd;
	}
}

// Adds one step to the look. Unknown steps are skipped, so saving the look drops them.
static void parse_step(const char *line, Look *look)
{
	char stepName[STEP_NAME_SIZE];
	int nameLength = 0;
	// The scan width is STEP_NAME_SIZE - 1.
	if (sscanf(line, "%23s%n", stepName, &nameLength) != 1) {
		return;
	}
	int kind = find_op_kind(stepName);
	if (kind < 0 || look->opCount >= LOOK_OPS_MAX) {
		return;
	}
	LookOp *step = &look->ops[look->opCount];
	look->opCount++;
	step->kind = kind;
	const LookOpInfo *info = &LOOK_OP_INFO[kind];
	for (int i = 0; i < info->paramCount; i++) {
		step->values[i] = info->defaults[i];
	}
	parse_values(line + nameLength, step);
	// A curve keeps only the points the recipe gives. Other steps always have all their parameters.
	if (kind != LOOK_OP_CURVE) {
		step->count = info->paramCount;
	}
}

bool look_parse(const char *text, Look *out)
{
	memset(out, 0, sizeof *out);
	char line[RECIPE_LINE_SIZE];
	const char *cursor = text;
	while (*cursor) {
		cursor = read_line(cursor, line, sizeof line);
		char *content = strip_comment_and_indent(line);
		if (!*content) {
			continue;
		}
		if (parse_label(content, out)) {
			continue;
		}
		parse_step(content, out);
	}
	if (!out->name[0]) {
		return false;
	}
	if (!out->shortName[0]) {
		snprintf(out->shortName, sizeof out->shortName, "%.*s", SHORT_NAME_LETTERS, out->name);
	}
	return true;
}

// The text written before a value: nothing before the first, ":" inside a curve's x:y pairs and " / " between
// the shadow tint and the highlight tint.
static const char *value_separator(LookOpKind kind, int valueIndex)
{
	if (valueIndex == 0) {
		return "";
	}
	if (kind == LOOK_OP_CURVE) {
		bool isCurveY = valueIndex % 2 == 1;
		return isCurveY ? ":" : " ";
	}
	if (kind == LOOK_OP_SPLIT_TONE && valueIndex == LOOK_SPLIT_TONE_SHADOW_VALUES) {
		return " / ";
	}
	return " ";
}

int look_format_values(const LookOp *step, char *out, size_t outSize, int roomForValue)
{
	int length = 0;
	out[0] = 0;
	for (int j = 0; j < step->count && length < (int)outSize - roomForValue; j++) {
		const char *separator = value_separator(step->kind, j);
		length += snprintf(out + length, outSize - length, "%s%g", separator, step->values[j]);
	}
	return length;
}

int look_serialize(const Look *look, char *out, size_t outSize)
{
	int length = snprintf(out, outSize, "name: %s\nshort: %s\n", look->name, look->shortName);
	for (int i = 0; i < look->opCount && length < (int)outSize - ROOM_FOR_STEP; i++) {
		const LookOp *step = &look->ops[i];
		length += snprintf(out + length, outSize - length, "%s", LOOK_OP_INFO[step->kind].name);
		if (step->count > 0) {
			length += snprintf(out + length, outSize - length, " ");
			length += look_format_values(step, out + length, outSize - length, ROOM_FOR_VALUE);
		}
		length += snprintf(out + length, outSize - length, "\n");
	}
	return length;
}

static bool write_text_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	if (!file) {
		return false;
	}
	bool written = fputs(text, file) >= 0;
	bool closed = fclose(file) == 0;
	return written && closed;
}

static bool read_text_file(const char *path, char *buffer, size_t bufferSize)
{
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	size_t length = fread(buffer, 1, bufferSize - 1, file);
	fclose(file);
	buffer[length] = 0;
	return true;
}

static void write_readme(const char *dir)
{
	char path[FILE_PATH_SIZE];
	snprintf(path, sizeof path, "%s/README.txt", dir);
	FILE *file = fopen(path, "w");
	if (!file) {
		return;
	}
	fputs("Pro Cam looks: one recipe per .look file, loaded at start-up (sorted by file name).\n"
	      "Lines: 'name: ...', 'short: ...' (up to 8 letters, for the menu tile), "
	      "then processing steps, run in order.\n"
	      "'#' starts a comment. Until a 'tone' step colours are linear (1 = white); "
	      "'tone' moves them to display values\n"
	      "(without one, plain gamma 2.2 is applied at the end). "
	      "hotpixel always runs first, on the raw data.\n"
	      "The DNG is never touched: looks only make the JPG and the live view.\n"
	      "\n"
	      "Steps:\n",
	      file);
	for (int kind = 0; kind < LOOK_OP_KINDS; kind++) {
		const LookOpInfo *info = &LOOK_OP_INFO[kind];
		fprintf(file, "  %-10s", info->name);
		for (int i = 0; i < info->paramCount && i < README_PARAMS_MAX; i++) {
			fprintf(file, " <%s>", info->paramNames[i]);
		}
		if (kind == LOOK_OP_CURVE) {
			fprintf(file, " x:y x:y ...");
		}
		fprintf(file, "\n      %s\n", info->help);
	}
	fclose(file);
}

static bool is_look_file(const char *fileName)
{
	const char *extension = ".look";
	size_t extensionLength = strlen(extension);
	size_t length = strlen(fileName);
	return length > extensionLength && strcmp(fileName + length - extensionLength, extension) == 0;
}

static int count_look_files(DIR *folder)
{
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL) {
		if (is_look_file(entry->d_name)) {
			count++;
		}
	}
	return count;
}

static void write_builtin_files(const char *dir)
{
	char path[FILE_PATH_SIZE];
	for (int i = 0; i < BUILTIN_LOOK_COUNT; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, BUILTIN_LOOKS[i].fileName);
		write_text_file(path, BUILTIN_LOOKS[i].recipe);
	}
	write_readme(dir);
}

// Writes the built-in looks into an empty folder. A used folder is left as it is.
static void prepare_looks_dir(const char *dir)
{
	DIR *folder = opendir(dir);
	if (!folder) {
		return;
	}
	int fileCount = count_look_files(folder);
	closedir(folder);
	if (fileCount == 0) {
		write_builtin_files(dir);
	}
}

static void load_look_files(const char *dir)
{
	DIR *folder = opendir(dir);
	if (!folder) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL && g_lookCount < LOOKS_MAX) {
		const char *fileName = entry->d_name;
		if (!is_look_file(fileName) || strlen(fileName) >= LOOK_FILE_NAME_SIZE) {
			continue;
		}
		char path[FILE_PATH_SIZE];
		snprintf(path, sizeof path, "%s/%s", dir, fileName);
		static char text[RECIPE_TEXT_SIZE];
		if (!read_text_file(path, text, sizeof text)) {
			continue;
		}
		Look *look = &g_looks[g_lookCount];
		if (look_parse(text, look)) {
			snprintf(look->file, sizeof look->file, "%s", fileName);
			g_lookCount++;
		}
	}
	closedir(folder);
}

static void load_builtins_from_memory(void)
{
	for (int i = 0; i < BUILTIN_LOOK_COUNT && g_lookCount < LOOKS_MAX; i++) {
		Look *look = &g_looks[g_lookCount];
		if (look_parse(BUILTIN_LOOKS[i].recipe, look)) {
			snprintf(look->file, sizeof look->file, "%s", BUILTIN_LOOKS[i].fileName);
			g_lookCount++;
		}
	}
}

static int compare_file_names(const void *left, const void *right)
{
	return strcmp(((const Look *)left)->file, ((const Look *)right)->file);
}

static void refresh_names(int index)
{
	g_lookNames[index] = g_looks[index].name;
	g_lookShortNames[index] = g_looks[index].shortName;
}

int look_load_all(const char *dir)
{
	snprintf(g_looksDir, sizeof g_looksDir, "%s", dir);
	g_lookCount = 0;
	mkdir(dir, LOOKS_DIR_MODE);
	prepare_looks_dir(dir);
	load_look_files(dir);
	if (g_lookCount == 0) {
		load_builtins_from_memory();
	}
	qsort(g_looks, g_lookCount, sizeof g_looks[0], compare_file_names);
	for (int i = 0; i < g_lookCount; i++) {
		refresh_names(i);
	}
	return g_lookCount;
}

int look_count(void)
{
	return g_lookCount;
}

const Look *look_get(int index)
{
	if (index < 0) {
		return &g_looks[0];
	}
	if (index >= g_lookCount) {
		return &g_looks[g_lookCount - 1];
	}
	return &g_looks[index];
}

const char *const *look_names(void)
{
	return g_lookNames;
}

const char *const *look_short_names(void)
{
	return g_lookShortNames;
}

void look_set(int index, const Look *look)
{
	if (index < 0 || index >= g_lookCount) {
		return;
	}
	char fileName[sizeof look->file];
	snprintf(fileName, sizeof fileName, "%s", g_looks[index].file);
	g_looks[index] = *look;
	snprintf(g_looks[index].file, sizeof fileName, "%s", fileName);
	refresh_names(index);
}

bool look_save(int index)
{
	if (index < 0 || index >= g_lookCount || !g_looksDir[0]) {
		return false;
	}
	static char text[RECIPE_TEXT_SIZE];
	look_serialize(&g_looks[index], text, sizeof text);
	char path[FILE_PATH_SIZE];
	snprintf(path, sizeof path, "%s/%s", g_looksDir, g_looks[index].file);
	return write_text_file(path, text);
}

static bool file_name_taken(const char *fileName)
{
	for (int i = 0; i < g_lookCount; i++) {
		if (strcmp(g_looks[i].file, fileName) == 0) {
			return true;
		}
	}
	return false;
}

// User looks are called user-01.look, user-02.look and so on, so they sort after the built-ins.
static void choose_user_file_name(Look *look)
{
	for (int number = 1; number <= USER_LOOK_NUMBER_MAX; number++) {
		snprintf(look->file, sizeof look->file, "user-%02d.look", number);
		if (!file_name_taken(look->file)) {
			return;
		}
	}
}

int look_add(const Look *look)
{
	if (g_lookCount >= LOOKS_MAX) {
		return -1;
	}
	int index = g_lookCount;
	g_looks[index] = *look;
	choose_user_file_name(&g_looks[index]);
	refresh_names(index);
	g_lookCount++;
	if (!look_save(index)) {
		g_lookCount--;
		return -1;
	}
	return index;
}

int look_describe(int index, char *out, size_t outSize)
{
	const Look *look = look_get(index);
	int length = snprintf(out, outSize, "look: %s (%s)\n", look->name, look->file);
	for (int i = 0; i < look->opCount && length < (int)outSize - ROOM_FOR_STEP; i++) {
		const LookOp *step = &look->ops[i];
		length += snprintf(out + length, outSize - length, "  %s", LOOK_OP_INFO[step->kind].name);
		for (int j = 0; j < step->count && length < (int)outSize - ROOM_FOR_VALUE; j++) {
			length += snprintf(out + length, outSize - length, " %g", step->values[j]);
		}
		length += snprintf(out + length, outSize - length, "\n");
	}
	return length;
}
