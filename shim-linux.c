// LD_PRELOAD shim: translucent editor background for Sublime Text (GTK3 + GL, Wayland). Linux half; the GL logic is in shim.c.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include "shim.h"

// Sublime resolves GTK with dlsym, so we interpose dlsym itself (bottom of file).
static void *real_dlsym(void *h, const char *name) {
	static void *(*fn)(void *, const char *);
	if (!fn) fn = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34") ?: dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
	return fn(h, name);
}

void *gl_next(const char *name) {
	return real_dlsym(RTLD_NEXT, name);
}

// Export each hook under its real GL name so it takes over from libGL.
#define EXPORT(ret, name, params, args) ret name params { return hook_##name args; }
SHIM_HOOKS(EXPORT)

static void *gtk, *(*real_window_new)(int);
#define GTK(name) real_dlsym(gtk, name)

static void *window_new(int type) {
	void *win = real_window_new(type);
	static int first = 1;
	if (first) {
		first = 0;
		shim_init();
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
