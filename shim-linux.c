// Translucent Sublime Text: the Linux half (LD_PRELOAD, GTK3).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "shim.h"

#define DLSYM_NEW "GLIBC_2.34"
#define DLSYM_OLD "GLIBC_2.2.5"

// Sublime resolves GTK with dlsym, so we interpose dlsym itself (bottom of file).
static void *real_dlsym(void *h, const char *name) {
	static void *(*fn)(void *, const char *);

	if (!fn)
		fn = dlvsym(RTLD_NEXT, "dlsym", DLSYM_NEW) ?: dlvsym(RTLD_NEXT, "dlsym", DLSYM_OLD);

	return fn(h, name);
}

#define PREFS_FILE "sublime-text/Packages/User/Preferences.sublime-settings"
#define MENUBAR_CSS "menubar { background-color: @theme_bg_color; }"
#define GTK_PRIORITY_APPLICATION 600

int prefs_path(char *buf, size_t size) {
	const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");

	if (config && *config)
		return snprintf(buf, size, "%s/" PREFS_FILE, config) < (int)size;

	return home && snprintf(buf, size, "%s/.config/" PREFS_FILE, home) < (int)size;
}

void *gl_next(const char *name) {
	return real_dlsym(RTLD_NEXT, name);
}

// Export each hook under its GL name so it replaces libGL's.
#define EXPORT(ret, name, params, args) ret name params { return hook_##name args; }
SHIM_HOOKS(EXPORT)

// ---- WINDOW ----
static void *gtk, *(*real_window_new)(int);
#define GTK(name) real_dlsym(gtk, name)

// An unpainted window leaves the focused menu bar transparent.
static void style_menubar(void) {
	void *(*new_provider)(void) = GTK("gtk_css_provider_new");
	int (*load)(void *, const char *, long, void **) = GTK("gtk_css_provider_load_from_data");
	void (*add)(void *, void *, unsigned) = GTK("gtk_style_context_add_provider_for_screen");
	void *(*screen)(void) = GTK("gdk_screen_get_default");
	void *css = new_provider();
	load(css, MENUBAR_CSS, -1, NULL);
	add(screen(), css, GTK_PRIORITY_APPLICATION);
}

static void *window_new(int type) {
	void *win = real_window_new(type);
	static int first = 1;

	if (first) {
		first = 0;
		style_menubar();
		// Not at load time: Sublime re-execs itself at startup and the new process needs the shim.
		unsetenv("LD_PRELOAD");
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
