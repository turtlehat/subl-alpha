// Starts Sublime Text with shim.dll injected: subl-alpha-windows [subl arguments...]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DEFAULT_SUBL L"C:\\Program Files\\Sublime Text\\sublime_text.exe"

#define CMD_SIZE 32768

int wmain(void) {
	if (FindWindowW(L"PX_WINDOW_CLASS", NULL)) {
		fprintf(stderr, "subl-alpha: Sublime Text is already running; close it first\n");
		return 1;
	}

	const wchar_t *subl = _wgetenv(L"SUBL") ?: DEFAULT_SUBL;
	wchar_t dll[MAX_PATH], cmd[CMD_SIZE];
	GetModuleFileNameW(NULL, dll, MAX_PATH);
	wcsrchr(dll, L'\\')[1] = 0;
	wcscat(dll, L"shim.dll");

	// Forward our own arguments untouched: the command line minus the first token.
	const wchar_t *args = GetCommandLineW();

	if (*args == L'"')
		args = wcschr(args + 1, L'"') ?: args + wcslen(args);
	else
		args += wcscspn(args, L" ");

	if (*args)
		args++;

	_snwprintf(cmd, CMD_SIZE, L"\"%ls\" %ls", subl, args);

	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;

	if (!CreateProcessW(subl, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
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
