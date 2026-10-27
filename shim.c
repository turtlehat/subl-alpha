// LD_PRELOAD shim: translucent editor background for Sublime Text (GTK3 + GL, Wayland).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

// Sublime resolves GTK with dlsym, so we interpose dlsym itself (bottom of file).
static void *real_dlsym(void *h, const char *name) {
	static void *(*fn)(void *, const char *);
	if (!fn) fn = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34") ?: dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
	return fn(h, name);
}

// Next definition of the function we are currently inside.
#define NEXT(f) ({ static typeof(&f) p; p ?: (p = real_dlsym(RTLD_NEXT, #f)); })

// Opacities from the launcher (--alpha, --chrome); chrome stays opaque unless asked.
static float editor_alpha, chrome_alpha;

static float opacity(const char *var, float fallback) {
	const char *env = getenv(var);
	float a = env ? atof(env) : fallback;
	return a < 0 ? 0 : a > 1 ? 1 : a;
}

// Each window has its own GL context reusing the same ids, so solid programs are ref-counted.
#define ID(i) ((i) & 0xFFFF)
static unsigned char solid_shader[0x10000], solid_program[0x10000];

void glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h) {
	NEXT(glRenderbufferStorage)(target, format == GL_RGB8 ? GL_RGBA8 : format, w, h);
}

// The dual-source blend also writes the destination alpha, which needs a single coverage value; green
// is the closest match to the per-channel coverage. (Don't also force frag_color.a: it puts solid boxes
// behind emoji.)
#define TEXT_ALPHA "frag_alpha = alpha;"
#define TEXT_ALPHA_FIXED "frag_alpha = vec4(alpha.rgb, alpha.g);"

void glShaderSource(GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len) {
	const GLchar *patched[n];
	memcpy(patched, src, sizeof patched);
	int tint = 0, textured = 0, changed = 0;
	for (int i = 0; i < n; i++) {
		tint |= !!strstr(src[i], "tint = texelFetch(instances, gl_InstanceID * 2 + 0)");
		textured |= !!strstr(src[i], "#define TEXTURED");
		const char *at = strstr(src[i], TEXT_ALPHA);
		char *copy;
		if (at && asprintf(&copy, "%.*s%s%s", (int)(at - src[i]), src[i], TEXT_ALPHA_FIXED, at + strlen(TEXT_ALPHA)) >= 0) {
			patched[i] = copy;
			changed = 1;
		}
	}
	solid_shader[ID(shader)] = tint && !textured;
	NEXT(glShaderSource)(shader, n, patched, changed ? NULL : len);
	for (int i = 0; i < n; i++)
		if (patched[i] != src[i]) free((void *)patched[i]);
}

void glAttachShader(GLuint program, GLuint shader) {
	solid_program[ID(program)] += solid_shader[ID(shader)];
	NEXT(glAttachShader)(program, shader);
}

void glDeleteProgram(GLuint program) {
	if (solid_program[ID(program)]) solid_program[ID(program)]--;
	NEXT(glDeleteProgram)(program);
}

// Sublime uploads a draw's per-instance data (tint, then position and size) right before the draw.
// Remember the latest upload per thread (one GL context each) instead of reading the buffer back.
static __thread float last_upload[8];

static void snoop(const void *data, GLsizeiptr size) {
	if (data && size >= (GLsizeiptr)sizeof last_upload) memcpy(last_upload, data, sizeof last_upload);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
	snoop(data, size);
	NEXT(glBufferData)(target, size, data, usage);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
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

void glClear(GLbitfield mask) {
	ngroups = 0;
	NEXT(glClear)(mask);
}

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

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
	GLint prog, sc[4], blend[4];
	GLfloat color[4];
	float a = 1;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	if (solid_program[ID(prog)]) {
		glGetIntegerv(GL_SCISSOR_BOX, sc);
		a = fill_alpha(sc);
	}
	if (a < 1) {
		glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
		glGetFloatv(GL_BLEND_COLOR, color);
		glBlendColor(a, a, a, a);
		glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
	}
	NEXT(glDrawArraysInstanced)(mode, first, count, instances);
	if (a < 1) {
		glBlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
		glBlendColor(color[0], color[1], color[2], color[3]);
	}
}

static void *gtk, *(*real_window_new)(int);
#define GTK(name) real_dlsym(gtk, name)

static void *window_new(int type) {
	void *win = real_window_new(type);
	static int first = 1;
	if (first) {
		first = 0;
		editor_alpha = opacity("SUBL_ALPHA", 0.5f);
		chrome_alpha = opacity("SUBL_CHROME", 1);
		// Not at load time: Sublime re-execs itself at startup and that child needs the shim.
		unsetenv("LD_PRELOAD");
		// An unpainted window leaves the focused menu bar transparent.
		void *(*new_provider)(void) = GTK("gtk_css_provider_new");
		int (*load)(void *, const char *, long, void **) = GTK("gtk_css_provider_load_from_data");
		void (*add)(void *, void *, unsigned) = GTK("gtk_style_context_add_provider_for_screen");
		void *(*screen)(void) = GTK("gdk_screen_get_default");
		void *css = new_provider();
		load(css, "menubar { background-color: @theme_bg_color; }", -1, NULL);
		add(screen(), css, 600); // GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
	}
	void *(*screen)(void) = GTK("gdk_screen_get_default");
	void *(*rgba_visual)(void *) = GTK("gdk_screen_get_rgba_visual");
	void (*set_visual)(void *, void *) = GTK("gtk_widget_set_visual");
	void (*set_paintable)(void *, int) = GTK("gtk_widget_set_app_paintable");
	set_visual(win, rgba_visual(screen()));
	set_paintable(win, 1);
	return win;
}

void *dlsym(void *handle, const char *name) {
	void *sym = real_dlsym(handle, name);
	if (sym && !strcmp(name, "gtk_window_new")) {
		real_window_new = sym;
		gtk = handle;
		return window_new;
	}
	return sym;
}
