// Translucent Sublime Text: the OpenGL half, shared by Linux and Windows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "shim.h"

// The real GL function this hook replaces.
#define NEXT(f) ({ static typeof(&hook_##f) p; p ?: (p = gl_next(#f)); })

// ---- PREFS ----
#define EDITOR_OPACITY_KEY "\"editor_opacity\""
#define CHROME_OPACITY_KEY "\"chrome_opacity\""
#define PATH_SIZE 1024
#define PREFS_FILE_SIZE 65536

static _Atomic float editor_alpha = 1, chrome_alpha = 1;

// DWM drops the acrylic backdrop at alpha of about 1% or less.
#ifdef _WIN32
#define MIN_ALPHA 0.03f
#else
#define MIN_ALPHA 0
#endif

// Not a JSON parser: finds "key": number outside // comments. Anything else is opaque.
static float read_setting(const char *text, const char *key) {
	for (const char *at = strstr(text, key); at; at = strstr(at + 1, key)) {
		const char *line = at;

		while (line > text && line[-1] != '\n')
			line--;

		const char *comment = strstr(line, "//");

		if (comment && comment < at)
			continue;

		at += strlen(key);
		at += strspn(at, " \t");

		if (*at++ != ':')
			continue;

		char *end;
		float a = strtof(at, &end);

		if (end == at)
			return 1;

		return a < MIN_ALPHA ? MIN_ALPHA : a > 1 ? 1 : a;
	}

	return 1;
}

static void refresh_prefs(void) {
	static time_t seen_time;
	static off_t seen_size = -1;
	char path[PATH_SIZE], text[PREFS_FILE_SIZE];
	struct stat st;
	FILE *f = NULL;

	if (prefs_path(path, sizeof path) && !stat(path, &st))
		f = fopen(path, "r");

	if (!f) {
		editor_alpha = chrome_alpha = 1;
		seen_size = -1;
		return;
	}

	if (st.st_mtime == seen_time && st.st_size == seen_size) {
		fclose(f);
		return;
	}

	seen_time = st.st_mtime;
	seen_size = st.st_size;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	text[n] = 0;
	editor_alpha = read_setting(text, EDITOR_OPACITY_KEY);
	chrome_alpha = read_setting(text, CHROME_OPACITY_KEY);
}

// ---- WINDOW ----
// Give the framebuffer an alpha channel; the OS half makes the window itself translucent.
void hook_glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h) {
	NEXT(glRenderbufferStorage)(target, format == GL_RGB8 ? GL_RGBA8 : format, w, h);
}

// ---- TEXT ----
// Alpha needs a single coverage value; green is the closest to per-channel coverage.
// (Forcing frag_color.a too puts solid boxes behind emoji.)
#define TEXT_ALPHA "frag_alpha = alpha;"
#define TEXT_ALPHA_FIXED "frag_alpha = vec4(alpha.rgb, alpha.g);"

static char *fix_text_alpha(const char *src) {
	const char *at = strstr(src, TEXT_ALPHA);

	if (!at)
		return NULL;

	size_t head = at - src, tail = strlen(at + strlen(TEXT_ALPHA));
	char *copy = malloc(head + strlen(TEXT_ALPHA_FIXED) + tail + 1);

	if (!copy)
		return NULL;

	memcpy(copy, src, head);
	strcpy(copy + head, TEXT_ALPHA_FIXED);
	strcpy(copy + head + strlen(TEXT_ALPHA_FIXED), at + strlen(TEXT_ALPHA));
	return copy;
}

// ---- SHADERS ----
// These must match Sublime's shader source; a Sublime update can break them.
#define SOLID_SHADER_MARKER "tint = texelFetch(instances, gl_InstanceID * 2 + 0)"
#define TEXTURED_SHADER_MARKER "#define TEXTURED"
#define GRADIENT_SHADER_MARKER "mix(color1, color2, t)"

// Each window's GL context reuses the same ids, so programs are ref-counted.
#define ID_COUNT 0x10000
#define ID(i) ((i) & (ID_COUNT - 1))
static unsigned char solid_shader[ID_COUNT], solid_program[ID_COUNT];
static unsigned char textured_shader[ID_COUNT], textured_program[ID_COUNT];
static unsigned char gradient_shader[ID_COUNT], gradient_program[ID_COUNT];

void hook_glShaderSource(GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len) {
	const GLchar *patched[n];
	memcpy(patched, src, sizeof patched);
	int tint = 0, textured = 0, gradient = 0, changed = 0;

	for (int i = 0; i < n; i++) {
		tint |= !!strstr(src[i], SOLID_SHADER_MARKER);
		textured |= !!strstr(src[i], TEXTURED_SHADER_MARKER);
		gradient |= !!strstr(src[i], GRADIENT_SHADER_MARKER);
		char *copy = fix_text_alpha(src[i]);

		if (copy) {
			patched[i] = copy;
			changed = 1;
		}
	}

	solid_shader[ID(shader)] = tint && !textured;
	textured_shader[ID(shader)] = tint && textured;
	gradient_shader[ID(shader)] = gradient;
	NEXT(glShaderSource)(shader, n, patched, changed ? NULL : len);

	for (int i = 0; i < n; i++) {
		if (patched[i] != src[i])
			free((void *)patched[i]);
	}
}

void hook_glAttachShader(GLuint program, GLuint shader) {
	solid_program[ID(program)] += solid_shader[ID(shader)];
	textured_program[ID(program)] += textured_shader[ID(shader)];
	gradient_program[ID(program)] += gradient_shader[ID(shader)];
	NEXT(glAttachShader)(program, shader);
}

void hook_glDeleteProgram(GLuint program) {
	if (solid_program[ID(program)])
		solid_program[ID(program)]--;

	if (textured_program[ID(program)])
		textured_program[ID(program)]--;

	if (gradient_program[ID(program)])
		gradient_program[ID(program)]--;

	NEXT(glDeleteProgram)(program);
}

// ---- EDITOR vs CHROME ----
// Sublime uploads a draw's instance data right before drawing it; remember the latest per thread.
#define SNOOP_FLOATS 8
#define TINT_FLOATS 4
static __thread float last_upload[SNOOP_FLOATS];

static int tint_is_white(void) {
	for (int i = 0; i < TINT_FLOATS; i++) {
		if (last_upload[i] != 1)
			return 0;
	}

	return 1;
}

static void snoop(const void *data, GLsizeiptr size) {
	if (data && size >= (GLsizeiptr)sizeof last_upload)
		memcpy(last_upload, data, sizeof last_upload);
}

void hook_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
	snoop(data, size);
	NEXT(glBufferData)(target, size, data, usage);
}

void hook_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
	snoop(data, size);
	NEXT(glBufferSubData)(target, offset, size, data);
}

// Each editor group starts with a white backing fill. Things in that group's column are editor,
// things touching no group are chrome, and the rest are overlays (popups, panels) that stay solid.
#define MAX_GROUPS 16
static __thread GLint groups[MAX_GROUPS][4];
static __thread int ngroups;

static void register_group(const GLint sc[4]) {
	if (ngroups < MAX_GROUPS)
		memcpy(groups[ngroups++], sc, sizeof groups[0]);
}

static int rects_overlap(const GLint a[4], const GLint b[4]) {
	return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}

static float region_alpha(const GLint sc[4]) {
	int overlaps = 0;

	for (int i = 0; i < ngroups; i++) {
		const GLint *g = groups[i];

		if (sc[1] == g[1] && sc[3] == g[3] && sc[0] >= g[0] && sc[0] + sc[2] <= g[0] + g[2])
			return editor_alpha;

		overlaps |= rects_overlap(sc, g);
	}

	return overlaps ? 1 : chrome_alpha;
}

// Called once per frame, so it also picks up preference changes.
void hook_glClear(GLbitfield mask) {
	ngroups = 0;
	refresh_prefs();
	NEXT(glClear)(mask);
}

// ---- DRAW ----
#define NINE_SLICE_INSTANCES 9

static void (*get_integerv)(GLenum, GLint *);
static void (*get_floatv)(GLenum, GLfloat *);
static void (*blend_color)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*blend_func)(GLenum, GLenum);
static void (*blend_func_separate)(GLenum, GLenum, GLenum, GLenum);

static void resolve_gl(void) {
	if (blend_func_separate)
		return;

	get_integerv = gl_next("glGetIntegerv");
	get_floatv = gl_next("glGetFloatv");
	blend_color = gl_next("glBlendColor");
	blend_func = gl_next("glBlendFunc");
	blend_func_separate = gl_next("glBlendFuncSeparate");
}

typedef struct {
	GLint blend[4];
	GLfloat color[4];
} BlendState;

static void fade_begin(float a, BlendState *saved) {
	get_integerv(GL_BLEND_SRC_RGB, &saved->blend[0]);
	get_integerv(GL_BLEND_DST_RGB, &saved->blend[1]);
	get_integerv(GL_BLEND_SRC_ALPHA, &saved->blend[2]);
	get_integerv(GL_BLEND_DST_ALPHA, &saved->blend[3]);
	get_floatv(GL_BLEND_COLOR, saved->color);
	blend_color(a, a, a, a);
	blend_func(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
}

static void fade_end(const BlendState *saved) {
	blend_func_separate(saved->blend[0], saved->blend[1], saved->blend[2], saved->blend[3]);
	blend_color(saved->color[0], saved->color[1], saved->color[2], saved->color[3]);
}

void hook_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
	GLint prog, sc[4];
	BlendState saved;
	float a = 1;
	resolve_gl();
	get_integerv(GL_CURRENT_PROGRAM, &prog);

	if (solid_program[ID(prog)]) {
		get_integerv(GL_SCISSOR_BOX, sc);

		if (tint_is_white()) {
			register_group(sc);
			a = editor_alpha;
		} else {
			a = region_alpha(sc);
		}
	}

	// Tabs and tab bar buttons are 9-slices (single textured draws are icons); the buttons are
	// small and square, and stay opaque.
	if (textured_program[ID(prog)] && instances == NINE_SLICE_INSTANCES) {
		get_integerv(GL_SCISSOR_BOX, sc);

		if (sc[2] > sc[3])
			a = region_alpha(sc);
	}

	if (a < 1)
		fade_begin(a, &saved);

	NEXT(glDrawArraysInstanced)(mode, first, count, instances);

	if (a < 1)
		fade_end(&saved);
}

// Drop shadows are gradients drawn without instancing.
void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
	GLint prog, sc[4];
	BlendState saved;
	float a = 1;
	resolve_gl();
	get_integerv(GL_CURRENT_PROGRAM, &prog);

	if (gradient_program[ID(prog)]) {
		get_integerv(GL_SCISSOR_BOX, sc);
		a = region_alpha(sc);
	}

	if (a < 1)
		fade_begin(a, &saved);

	NEXT(glDrawArrays)(mode, first, count);

	if (a < 1)
		fade_end(&saved);
}
