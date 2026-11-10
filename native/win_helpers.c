#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <windows.h>
#include <knownfolders.h>
#include <security.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "catch_native.h"

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *) malloc(sizeof(wchar_t) * (size_t) (n > 0 ? n : 1));
    w[0] = 0;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int narrow(const wchar_t *w, char *out, int out_len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_len, NULL, NULL);
    if (n <= 0) {
        if (out_len > 0) out[0] = 0;
        return 0;
    }
    return n - 1;
}

int cw_known_folder(int which, char *out, int out_len) {
    const KNOWNFOLDERID *ids[] = {&FOLDERID_Downloads, &FOLDERID_Pictures, &FOLDERID_Documents, &FOLDERID_RoamingAppData, &FOLDERID_LocalAppData};
    if (which < 0 || which >= (int) (sizeof(ids) / sizeof(ids[0]))) return 0;
    PWSTR path = NULL;
    int n = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(ids[which], KF_FLAG_CREATE, NULL, &path))) n = narrow(path, out, out_len);
    CoTaskMemFree(path);
    return n;
}

int cw_user_display_name(char *out, int out_len) {
    wchar_t name[256];
    ULONG size = 256;
    if (GetUserNameExW(NameDisplay, name, &size) && name[0]) {
        // "First Last" greets as "First"
        wchar_t *space = wcschr(name, L' ');
        if (space) *space = 0;
        return narrow(name, out, out_len);
    }
    DWORD len = 256;
    if (GetUserNameW(name, &len)) return narrow(name, out, out_len);
    return 0;
}

int cw_computer_name(char *out, int out_len) {
    wchar_t name[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return narrow(name, out, out_len);
    return 0;
}

static int path_exists(const wchar_t *w) {
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

int cw_move_into(const char *src, const char *dest_dir, char *out, int out_len) {
    wchar_t *wsrc = widen(src);
    wchar_t *wdir = widen(dest_dir);
    SHCreateDirectoryExW(NULL, wdir, NULL);
    const wchar_t *slash = wcsrchr(wsrc, L'\\');
    const wchar_t *name = slash ? slash + 1 : wsrc;
    wchar_t base[MAX_PATH], ext[MAX_PATH] = L"";
    wcsncpy(base, name, MAX_PATH - 1);
    base[MAX_PATH - 1] = 0;
    wchar_t *dot = wcsrchr(base, L'.');
    if (dot && dot != base) {
        wcsncpy(ext, dot, MAX_PATH - 1);
        *dot = 0;
    }
    wchar_t dest[2 * MAX_PATH + 32];
    for (int i = 0; i < 10000; i++) {
        if (i == 0) _snwprintf(dest, sizeof(dest) / sizeof(dest[0]), L"%s\\%s%s", wdir, base, ext);
        else _snwprintf(dest, sizeof(dest) / sizeof(dest[0]), L"%s\\%s (%d)%s", wdir, base, i, ext);
        if (!path_exists(dest)) break;
    }
    int ok = MoveFileExW(wsrc, dest, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
    if (ok) narrow(dest, out, out_len);
    free(wsrc);
    free(wdir);
    return ok;
}

// Called only after Save. CREATE_NEW avoids overwriting an existing file.
int cw_write_into(const unsigned char *data, int64_t size, const char *name, const char *dest_dir, char *out, int out_len) {
    if (size < 0 || (size > 0 && !data)) return 0;
    char safe[512];
    snprintf(safe, sizeof(safe), "%s", name);
    for (char *p = safe; *p; p++) {
        if ((unsigned char) *p < 32 || strchr("<>:\"/\\|?*", *p)) *p = '_';
    }
    size_t n = strlen(safe);
    while (n && (safe[n - 1] == '.' || safe[n - 1] == ' ')) safe[--n] = 0;
    if (!n) strcpy(safe, "file");
    wchar_t *base = widen(safe), *dir = widen(dest_dir);
    SHCreateDirectoryExW(NULL, dir, NULL);
    wchar_t ext[512] = L"";
    wchar_t *dot = wcsrchr(base, L'.');
    if (dot && dot != base) {
        wcscpy(ext, dot);
        *dot = 0;
    }
    wchar_t dest[2048];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10000; i++) {
        int chars = i == 0 ? _snwprintf(dest, 2048, L"%s\\%s%s", dir, base, ext) :
            _snwprintf(dest, 2048, L"%s\\%s (%d)%s", dir, base, i, ext);
        if (chars < 0 || chars >= 2048) break;
        file = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    free(base), free(dir);
    if (file == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    for (int64_t ofs = 0; ofs < size;) {
        DWORD written = 0, count = (DWORD) (size - ofs < 1024 * 1024 ? size - ofs : 1024 * 1024);
        if (!WriteFile(file, data + ofs, count, &written, NULL) || written != count) {
            ok = 0;
            break;
        }
        ofs += written;
    }
    if (!CloseHandle(file)) ok = 0;
    if (ok && !narrow(dest, out, out_len)) ok = 0;
    if (!ok) DeleteFileW(dest);
    return ok;
}

int cw_remove_dir(const char *dir) {
    wchar_t *w = widen(dir);
    int ok = RemoveDirectoryW(w) != 0;
    free(w);
    return ok;
}

int cw_open(const char *path_or_url) {
    wchar_t *w = widen(path_or_url);
    INT_PTR rc = (INT_PTR) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return rc > 32;
}

int cw_reveal(const char *path) {
    wchar_t *w = widen(path);
    wchar_t args[2 * MAX_PATH + 16];
    DWORD attr = GetFileAttributesW(w);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        _snwprintf(args, sizeof(args) / sizeof(args[0]), L"\"%s\"", w);
    } else {
        _snwprintf(args, sizeof(args) / sizeof(args[0]), L"/select,\"%s\"", w);
    }
    INT_PTR rc = (INT_PTR) ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    free(w);
    return rc > 32;
}

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

int cw_set_startup(const char *name, const char *command) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return 0;
    wchar_t *wname = widen(name);
    LSTATUS rc;
    if (command) {
        wchar_t *wcmd = widen(command);
        rc = RegSetValueExW(key, wname, 0, REG_SZ, (const BYTE *) wcmd, (DWORD) ((wcslen(wcmd) + 1) * sizeof(wchar_t)));
        free(wcmd);
    } else {
        rc = RegDeleteValueW(key, wname);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    free(wname);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

int cw_get_startup(const char *name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return 0;
    wchar_t *wname = widen(name);
    LSTATUS rc = RegQueryValueExW(key, wname, NULL, NULL, NULL, NULL);
    free(wname);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

int cw_exe_path(char *out, int out_len) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return 0;
    return narrow(path, out, out_len);
}

int cw_copy_text(const char *text) {
    wchar_t *w = widen(text);
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    int ok = 0;
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            memcpy(GlobalLock(mem), w, bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
    }
    free(w);
    return ok;
}

int cw_local_day(int64_t unix_seconds) {
    time_t t = (time_t) unix_seconds;
    struct tm tm;
    if (localtime_s(&tm, &t) != 0) return 0;
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static BOOL CALLBACK raise_own_window(HWND hwnd, LPARAM arg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
    *(int *) arg = 1;
    return FALSE;
}

int cw_bring_to_front(void) {
    int found = 0;
    EnumWindows(raise_own_window, (LPARAM) &found);
    return found;
}

void cw_work_area(int *left, int *top, int *right, int *bottom) {
    RECT r = {0, 0, 1280, 720};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    *left = r.left, *top = r.top, *right = r.right, *bottom = r.bottom;
}

int cw_folder_name(const char *path, char *out, int out_len) {
    const char *end = path + strlen(path);
    while (end > path && (end[-1] == '\\' || end[-1] == '/')) end--;
    const char *start = end;
    while (start > path && start[-1] != '\\' && start[-1] != '/') start--;
    int n = (int) (end - start);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, (size_t) n);
    out[n] = 0;
    return n;
}
