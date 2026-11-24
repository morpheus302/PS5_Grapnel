/* Grapnel.exe: the Windows builder's start. SPDX-License-Identifier: GPL-3.0-or-later

   It starts python\pythonw.exe on app\scripts\grapnel-builder\grapnel_gui.py, both in its own folder, passes its arguments
   on, and ends: no console, no installer, nothing written outside the folder but the window's saved choices
   (%LOCALAPPDATA%\Grapnel). When the folder is not whole (the .exe started from inside the zip, or a part taken away by an
   antivirus) it says so, in French or English after Windows' language.

   The idea is heydemoura's launcher.c (Longshot PS5 Builder, heydemoura/PS5_Longshot ps5/builder/launcher.c, GPL-3.0);
   written again here for Grapnel. Built by scripts/grapnel-builder/make_windows_package.py with llvm-mingw:
   -municode -mwindows -static, with window/launcher.rc (the icon, the manifest, the version information). */
#include <windows.h>
#include <wchar.h>

static int french(void) { return (GetUserDefaultUILanguage() & 0x3FF) == 0x0C; }

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)inst, (void)prev, (void)show;
    static wchar_t here[32768], py[32768 + 64], gui[32768 + 128], cmd[3 * 32768];
    DWORD n = GetModuleFileNameW(NULL, here, 32768);
    if (!n || n >= 32768) return 2;
    wchar_t *slash = wcsrchr(here, L'\\');
    if (!slash) return 2;
    *slash = 0;
    _snwprintf(py, sizeof py / sizeof *py, L"%ls\\python\\pythonw.exe", here);
    _snwprintf(gui, sizeof gui / sizeof *gui, L"%ls\\app\\scripts\\grapnel-builder\\grapnel_gui.py", here);
    if (!exists(py) || !exists(gui)) {
        MessageBoxW(NULL, french()
            ? L"Le dossier de Grapnel est incomplet.\n\nDécompressez tout le zip (clic droit, « Extraire tout… ») dans un dossier "
              L"à vous, par exemple C:\\Grapnel, puis lancez Grapnel.exe depuis ce dossier.\n\nSi c'est déjà fait, un antivirus a "
              L"peut-être retiré un fichier : voyez LISEZMOI.txt."
            : L"Grapnel's folder is incomplete.\n\nUnpack the whole zip (right click, \"Extract All...\") into a folder of yours, "
              L"C:\\Grapnel for instance, then start Grapnel.exe from that folder.\n\nIf you did, an antivirus may have taken a "
              L"file away: see README.txt.",
            L"Grapnel", MB_ICONERROR);
        return 2;
    }
    _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" -I \"%ls\" %ls", py, gui, args);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(py, cmd, NULL, NULL, FALSE, 0, NULL, here, &si, &pi)) {
        wchar_t msg[512];
        _snwprintf(msg, sizeof msg / sizeof *msg, french()
            ? L"Le Python de Grapnel n'a pas démarré (erreur %lu). Un antivirus l'a peut-être bloqué : voyez LISEZMOI.txt."
            : L"Grapnel's own Python did not start (error %lu). An antivirus may have blocked it: see README.txt.",
            GetLastError());
        MessageBoxW(NULL, msg, L"Grapnel", MB_ICONERROR);
        return 2;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
