// Starts Sublime Text with shim.dll injected: subl-alpha-windows [--editor OPACITY] [--chrome [OPACITY]] [subl arguments...]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SUBL L"C:\\Program Files\\Sublime Text\\sublime_text.exe"

int wmain(int argc, wchar_t **argv) {
	const wchar_t *editor = NULL, *chrome = NULL;
	int i = 1;
	for (; i < argc; i++) {
		if (!wcscmp(argv[i], L"--editor") && i + 1 < argc) editor = argv[++i];
		else if (!wcscmp(argv[i], L"--chrome")) {
			// the value is optional, so only a number counts
			if (i + 1 < argc && (iswdigit(argv[i + 1][0]) || argv[i + 1][0] == L'.')) chrome = argv[++i];
			else chrome = L"";
		} else break;
	}
	if (editor) SetEnvironmentVariableW(L"SUBL_EDITOR", editor);
	if (chrome) SetEnvironmentVariableW(L"SUBL_CHROME", *chrome ? chrome : editor ? editor : L"0.5");

	if (FindWindowW(L"PX_WINDOW_CLASS", NULL)) {
		fprintf(stderr, "subl-alpha: Sublime Text is already running; close it first\n");
		return 1;
	}

	wchar_t dll[MAX_PATH], cmd[32768] = L"\"" SUBL L"\"";
	GetModuleFileNameW(NULL, dll, MAX_PATH);
	wcsrchr(dll, L'\\')[1] = 0;
	wcscat(dll, L"shim.dll");
	for (; i < argc; i++) {
		wcscat(cmd, L" \"");
		wcscat(cmd, argv[i]);
		wcscat(cmd, L"\"");
	}

	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	if (!CreateProcessW(SUBL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
		fprintf(stderr, "subl-alpha: cannot start Sublime Text (error %lu)\n", GetLastError());
		return 1;
	}
	// The queued load runs first when the main thread starts, before Sublime's own code.
	SIZE_T bytes = (wcslen(dll) + 1) * sizeof *dll;
	void *remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, bytes, NULL) ||
	    !QueueUserAPC((PAPCFUNC)LoadLibraryW, pi.hThread, (ULONG_PTR)remote)) {
		fprintf(stderr, "subl-alpha: cannot inject shim.dll (error %lu)\n", GetLastError());
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	ResumeThread(pi.hThread);
	return 0;
}
