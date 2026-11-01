// Injected into Sublime Text: translucent editor background (OpenGL + acrylic). Windows half; the GL logic is in shim.c.
#include "shim.h"
#include <string.h>

static HMODULE opengl(void) {
	return GetModuleHandleA("opengl32.dll");
}

void *gl_next(const char *name) {
	PROC (WINAPI *get_proc)(LPCSTR) = (void *)GetProcAddress(opengl(), "wglGetProcAddress");
	void *proc = (void *)get_proc(name);
	return proc ? proc : (void *)GetProcAddress(opengl(), name);
}

// SetWindowCompositionAttribute is undocumented; this is how acrylic is turned on for any window.
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
#define ACCENT_ENABLE_ACRYLICBLURBEHIND 4

static void enable_acrylic(HWND win) {
	BOOL (WINAPI *set)(HWND, CompositionAttribute *) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowCompositionAttribute");
	AccentPolicy accent = {ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, 0x99000000, 0};
	CompositionAttribute attr = {WCA_ACCENT_POLICY, &accent, sizeof accent};
	if (set) set(win, &attr);
}

// The window needs an alpha channel in its pixel format and acrylic to show what is behind it.
static int WINAPI hook_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
	PIXELFORMATDESCRIPTOR alpha = *pfd;
	alpha.cAlphaBits = 8;
	alpha.dwFlags |= PFD_SUPPORT_COMPOSITION;
	return ChoosePixelFormat(dc, &alpha);
}

// Extending the frame into the client area makes DWM paint the flat accent colour instead of acrylic
// while the window is active.
static HRESULT WINAPI hook_DwmExtendFrameIntoClientArea(HWND win, const void *margins) {
	return S_OK;
}

static HGLRC WINAPI hook_wglCreateContext(HDC dc) {
	HWND win = WindowFromDC(dc);
	if (win) {
		HWND root = GetAncestor(win, GA_ROOT);
		if (root) win = root;
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
	for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
		if (!strcmp(name, hooks[i].name)) return hooks[i].hook;
	return NULL;
}

static PROC WINAPI hook_wglGetProcAddress(LPCSTR name) {
	PROC proc = wglGetProcAddress(name);
	void *hook = proc ? find_hook(name) : NULL;
	return hook ? (PROC)hook : proc;
}

// Sublime may look GL functions up with GetProcAddress as well as through its import table.
static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name) {
	void *hook = module == opengl() && ((ULONG_PTR)name >> 16) ? find_hook(name) : NULL;
	return hook ? (FARPROC)hook : GetProcAddress(module, name);
}

// Sublime imports the pixel format and GL functions directly, so replace them in its import table.
static void patch_imports(void) {
	BYTE *base = (BYTE *)GetModuleHandleW(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
		IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			void *hook = find_hook(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name);
			if (!hook) continue;
			DWORD old;
			VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			slots->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		shim_init();
		patch_imports();
	}
	return TRUE;
}
