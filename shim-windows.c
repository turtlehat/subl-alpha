// Translucent Sublime Text: the Windows half (injected DLL, acrylic).
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE opengl(void) {
	return GetModuleHandleA("opengl32.dll");
}

#define PREFS_FILE "\\Sublime Text\\Packages\\User\\Preferences.sublime-settings"

int prefs_path(char *buf, size_t size) {
	const char *appdata = getenv("APPDATA");
	return appdata && snprintf(buf, size, "%s" PREFS_FILE, appdata) < (int)size;
}

void *gl_next(const char *name) {
	PROC (WINAPI *get_proc)(LPCSTR) = (void *)GetProcAddress(opengl(), "wglGetProcAddress");
	void *proc = (void *)get_proc(name);
	return proc ? proc : (void *)GetProcAddress(opengl(), name);
}

// ---- WINDOW ----
// Undocumented API that turns on acrylic.
typedef struct {
	int state;
	int flags;
	unsigned tint; // AABBGGRR
	int animation;
} AccentPolicy;

typedef struct {
	int attribute;
	void *data;
	ULONG size;
} CompositionAttribute;

#define WCA_ACCENT_POLICY 19
#define ACRYLIC_TINT 0x99000000
#define ACCENT_ENABLE_ACRYLICBLURBEHIND 4

static void enable_acrylic(HWND win) {
	BOOL (WINAPI *set)(HWND, CompositionAttribute *) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowCompositionAttribute");
	AccentPolicy accent = {ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, ACRYLIC_TINT, 0};
	CompositionAttribute attr = {WCA_ACCENT_POLICY, &accent, sizeof accent};

	if (set)
		set(win, &attr);
}

static int WINAPI hook_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
	PIXELFORMATDESCRIPTOR alpha = *pfd;
	alpha.cAlphaBits = 8;
	alpha.dwFlags |= PFD_SUPPORT_COMPOSITION;
	return ChoosePixelFormat(dc, &alpha);
}

// Extending the frame makes DWM paint a flat accent colour instead of acrylic.
static HRESULT WINAPI hook_DwmExtendFrameIntoClientArea(HWND win, const void *margins) {
	return S_OK;
}

static HGLRC WINAPI hook_wglCreateContext(HDC dc) {
	HWND win = WindowFromDC(dc);

	if (win) {
		HWND root = GetAncestor(win, GA_ROOT);

		if (root)
			win = root;

		enable_acrylic(win);
	}

	return wglCreateContext(dc);
}

static PROC WINAPI hook_wglGetProcAddress(LPCSTR name);
static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name);

static const struct {
	const char *name;
	void *hook;
} hooks[] = {
#define TABLE(ret, name, params, args) {#name, hook_##name},
	SHIM_HOOKS(TABLE)
#undef TABLE
	{"ChoosePixelFormat", hook_ChoosePixelFormat},
	{"wglCreateContext", hook_wglCreateContext},
	{"DwmExtendFrameIntoClientArea", hook_DwmExtendFrameIntoClientArea},
	{"wglGetProcAddress", hook_wglGetProcAddress},
	{"GetProcAddress", hook_GetProcAddress},
};

static void *find_hook(const char *name) {
	for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
		if (!strcmp(name, hooks[i].name))
			return hooks[i].hook;
	}

	return NULL;
}

static PROC WINAPI hook_wglGetProcAddress(LPCSTR name) {
	PROC proc = wglGetProcAddress(name);
	void *hook = proc ? find_hook(name) : NULL;
	return hook ? (PROC)hook : proc;
}

// Sublime also looks GL functions up with GetProcAddress. An ordinal is a number, not a name.
#define IS_ORDINAL(name) (((ULONG_PTR)(name) >> 16) == 0)

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name) {
	void *hook = module == opengl() && !IS_ORDINAL(name) ? find_hook(name) : NULL;
	return hook ? (FARPROC)hook : GetProcAddress(module, name);
}

// Sublime imports these directly, so patch its import table.
static void patch_imports(void) {
	BYTE *base = (BYTE *)GetModuleHandleW(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
		IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);

		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;

			void *hook = find_hook(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name);

			if (!hook)
				continue;

			DWORD old;

			if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
				continue;

			slots->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		patch_imports();
	}

	return TRUE;
}
