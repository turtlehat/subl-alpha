// Translucent editor background for Sublime Text 4: the OpenGL logic shared by the Linux and Windows shims.
#include <stdlib.h>
#include <string.h>
#include "shim.h"

// Next definition of the function we are currently inside.
#define NEXT(f) ({ static typeof(&hook_##f) p; p ?: (p = gl_next(#f)); })

// Opacities from the launcher (--editor, --chrome); chrome stays opaque unless asked.
static float editor_alpha, chrome_alpha;

// DWM skips the acrylic backdrop for pixels with alpha at or below about 1%, so on Windows opacity never reaches 0.
#ifdef _WIN32
#define MIN_ALPHA 0.03f
#else
#define MIN_ALPHA 0
#endif

static float opacity(const char *var, float fallback) {
	const char *env = getenv(var);
	float a = env ? atof(env) : fallback;
	return a < MIN_ALPHA ? MIN_ALPHA : a > 1 ? 1 : a;
}

void shim_init(void) {
	editor_alpha = opacity("SUBL_EDITOR", 0.5f);
	chrome_alpha = opacity("SUBL_CHROME", 1);
}

// Each window has its own GL context reusing the same ids, so the programs we track are ref-counted.
#define ID(i) ((i) & 0xFFFF)
static unsigned char solid_shader[0x10000], solid_program[0x10000];
static unsigned char textured_shader[0x10000], textured_program[0x10000];

void hook_glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h) {
	NEXT(glRenderbufferStorage)(target, format == GL_RGB8 ? GL_RGBA8 : format, w, h);
}

// The dual-source blend also writes the destination alpha, which needs a single coverage value; green
// is the closest match to the per-channel coverage. (Don't also force frag_color.a: it puts solid boxes
// behind emoji.)
#define TEXT_ALPHA "frag_alpha = alpha;"
#define TEXT_ALPHA_FIXED "frag_alpha = vec4(alpha.rgb, alpha.g);"

// Copy of src with the text alpha fixed, or NULL if src has nothing to fix.
static char *fix_text_alpha(const char *src) {
	const char *at = strstr(src, TEXT_ALPHA);
	if (!at) return NULL;
	size_t head = at - src, tail = strlen(at + strlen(TEXT_ALPHA));
	char *copy = malloc(head + strlen(TEXT_ALPHA_FIXED) + tail + 1);
	if (!copy) return NULL;
	memcpy(copy, src, head);
	strcpy(copy + head, TEXT_ALPHA_FIXED);
	strcpy(copy + head + strlen(TEXT_ALPHA_FIXED), at + strlen(TEXT_ALPHA));
	return copy;
}

void hook_glShaderSource(GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len) {
	const GLchar *patched[n];
	memcpy(patched, src, sizeof patched);
	int tint = 0, textured = 0, changed = 0;
	for (int i = 0; i < n; i++) {
		tint |= !!strstr(src[i], "tint = texelFetch(instances, gl_InstanceID * 2 + 0)");
		textured |= !!strstr(src[i], "#define TEXTURED");
		char *copy = fix_text_alpha(src[i]);
		if (copy) {
			patched[i] = copy;
			changed = 1;
		}
	}
	solid_shader[ID(shader)] = tint && !textured;
	textured_shader[ID(shader)] = tint && textured;
	NEXT(glShaderSource)(shader, n, patched, changed ? NULL : len);
	for (int i = 0; i < n; i++)
		if (patched[i] != src[i]) free((void *)patched[i]);
}

void hook_glAttachShader(GLuint program, GLuint shader) {
	solid_program[ID(program)] += solid_shader[ID(shader)];
	textured_program[ID(program)] += textured_shader[ID(shader)];
	NEXT(glAttachShader)(program, shader);
}

void hook_glDeleteProgram(GLuint program) {
	if (solid_program[ID(program)]) solid_program[ID(program)]--;
	if (textured_program[ID(program)]) textured_program[ID(program)]--;
	NEXT(glDeleteProgram)(program);
}

// Sublime uploads a draw's per-instance data (tint, then position and size) right before the draw.
// Remember the latest upload per thread (one GL context each) instead of reading the buffer back.
static __thread float last_upload[8];

static void snoop(const void *data, GLsizeiptr size) {
	if (data && size >= (GLsizeiptr)sizeof last_upload) memcpy(last_upload, data, sizeof last_upload);
}

void hook_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
	snoop(data, size);
	NEXT(glBufferData)(target, size, data, usage);
}

void hook_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
	snoop(data, size);
	NEXT(glBufferSubData)(target, offset, size, data);
}

// Each editor group starts with an opaque white backing fill covering its text area. The editor's own
// fills (minimap, background, line highlights, selections) are scissored to a full-height column of
// that area. Fills touching no group are chrome (tab bar, sidebar, status bar, dividers); fills that
// overlap a group without being part of it are overlays (autocomplete, palette, panels) and stay opaque.
#define MAX_GROUPS 16
static __thread GLint groups[MAX_GROUPS][4];
static __thread int ngroups;

// Opacity for the solid fill about to be drawn inside scissor box sc.
static float fill_alpha(const GLint sc[4]) {
	if (last_upload[0] == 1 && last_upload[1] == 1 && last_upload[2] == 1 && last_upload[3] == 1) {
		if (ngroups < MAX_GROUPS) memcpy(groups[ngroups++], sc, sizeof groups[0]);
		return editor_alpha;
	}
	int overlaps = 0;
	for (int i = 0; i < ngroups; i++) {
		const GLint *g = groups[i];
		if (sc[1] == g[1] && sc[3] == g[3] && sc[0] >= g[0] && sc[0] + sc[2] <= g[0] + g[2]) return editor_alpha;
		overlaps |= sc[0] < g[0] + g[2] && g[0] < sc[0] + sc[2] && sc[1] < g[1] + g[3] && g[1] < sc[1] + sc[3];
	}
	return overlaps ? 1 : chrome_alpha;
}

// GL functions we call but don't replace.
static void (*get_integerv)(GLenum, GLint *);
static void (*get_floatv)(GLenum, GLfloat *);
static void (*blend_color)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*blend_func)(GLenum, GLenum);
static void (*blend_func_separate)(GLenum, GLenum, GLenum, GLenum);

static void resolve_gl(void) {
	if (blend_func_separate) return;
	get_integerv = gl_next("glGetIntegerv");
	get_floatv = gl_next("glGetFloatv");
	blend_color = gl_next("glBlendColor");
	blend_func = gl_next("glBlendFunc");
	blend_func_separate = gl_next("glBlendFuncSeparate");
}

void hook_glClear(GLbitfield mask) {
	ngroups = 0;
	NEXT(glClear)(mask);
}

void hook_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
	GLint prog, sc[4], blend[4];
	GLfloat color[4];
	float a = 1;
	resolve_gl();
	get_integerv(GL_CURRENT_PROGRAM, &prog);
	// Tab backgrounds and the tab bar buttons are 9-slice textures; single textured draws are icons.
	// Popup shadows and scrollbar thumbs are 9-slices too, but overlap an editor group, so fill_alpha keeps them solid.
	if (solid_program[ID(prog)] || (textured_program[ID(prog)] && instances == 9)) {
		get_integerv(GL_SCISSOR_BOX, sc);
		a = fill_alpha(sc);
	}
	if (a < 1) {
		get_integerv(GL_BLEND_SRC_RGB, &blend[0]);
		get_integerv(GL_BLEND_DST_RGB, &blend[1]);
		get_integerv(GL_BLEND_SRC_ALPHA, &blend[2]);
		get_integerv(GL_BLEND_DST_ALPHA, &blend[3]);
		get_floatv(GL_BLEND_COLOR, color);
		blend_color(a, a, a, a);
		blend_func(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
	}
	NEXT(glDrawArraysInstanced)(mode, first, count, instances);
	if (a < 1) {
		blend_func_separate(blend[0], blend[1], blend[2], blend[3]);
		blend_color(color[0], color[1], color[2], color[3]);
	}
}
