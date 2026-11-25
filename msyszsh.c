/*
 * msyszsh.exe - equivalent to:
 *   chcp 65001
 *   <MSYS2_ROOT>\msys2_shell.cmd -defterm -no-start -use-full-path -here -mingw64 -shell zsh %*
 *
 * Starts MSYS2's zsh as a login shell in the current console and forwards every
 * argument to it. The MSYS2 installation must be given by the MSYS2_ROOT
 * environment variable (the directory containing usr\bin\msys-2.0.dll).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- minimal dynamic wide string ---------- */

typedef struct {
    wchar_t *p;
    size_t len;
    size_t cap;
} wstr;

static void w_init(wstr *s)
{
    s->cap = 64;
    s->len = 0;
    s->p = (wchar_t *)malloc(s->cap * sizeof(wchar_t));
    if (!s->p) {
        fputs("msyszsh: out of memory\n", stderr);
        exit(2);
    }
    s->p[0] = L'\0';
}

static void w_reserve(wstr *s, size_t extra)
{
    if (s->len + extra + 1 <= s->cap)
        return;
    while (s->cap < s->len + extra + 1)
        s->cap *= 2;
    s->p = (wchar_t *)realloc(s->p, s->cap * sizeof(wchar_t));
    if (!s->p) {
        fputs("msyszsh: out of memory\n", stderr);
        exit(2);
    }
}

static void w_addc(wstr *s, wchar_t c)
{
    w_reserve(s, 1);
    s->p[s->len++] = c;
    s->p[s->len] = L'\0';
}

static void w_add(wstr *s, const wchar_t *t)
{
    size_t n = wcslen(t);
    w_reserve(s, n);
    memcpy(s->p + s->len, t, n * sizeof(wchar_t));
    s->len += n;
    s->p[s->len] = L'\0';
}

/* quote one argument the way MSVCRT does, then append it to a command line
   (with a leading space) */
static void w_add_quoted_arg(wstr *s, const wchar_t *arg)
{
    int need = (*arg == L'\0');
    const wchar_t *q;
    const wchar_t *p;

    for (q = arg; *q; ++q) {
        if (*q == L' ' || *q == L'\t' || *q == L'"') {
            need = 1;
            break;
        }
    }

    w_addc(s, L' ');
    if (!need) {
        w_add(s, arg);
        return;
    }

    w_addc(s, L'"');
    for (p = arg; *p; ) {
        size_t bs = 0;
        while (*p == L'\\') {
            ++bs;
            ++p;
        }
        if (*p == L'"') {
            size_t i;
            for (i = 0; i < 2 * bs + 1; ++i)
                w_addc(s, L'\\');
            w_addc(s, L'"');
            ++p;
        } else if (*p == L'\0') {
            size_t i;
            for (i = 0; i < 2 * bs; ++i)
                w_addc(s, L'\\');
        } else {
            size_t i;
            for (i = 0; i < bs; ++i)
                w_addc(s, L'\\');
            w_addc(s, *p);
            ++p;
        }
    }
    w_addc(s, L'"');
}

/* ---------- error output (console, UTF-8) ---------- */

static void eputs(const wchar_t *msg)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    DWORD mode = 0;

    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, msg, (DWORD)wcslen(msg), &written, NULL);
        return;
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        char buf[2048];
        int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, (int)sizeof(buf), NULL, NULL);
        if (n > 1)
            WriteFile(h, buf, (DWORD)(n - 1), &written, NULL);
    }
}

static void die(const wchar_t *msg)
{
    eputs(L"msyszsh: ");
    eputs(msg);
    eputs(L"\n");
    exit(2);
}

/* ---------- path helpers ---------- */

static int file_exists(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* a valid MSYS2 root contains usr\bin\msys-2.0.dll */
static int root_ok(const wchar_t *root)
{
    wstr p;
    int ok;
    w_init(&p);
    w_add(&p, root);
    w_add(&p, L"\\usr\\bin\\msys-2.0.dll");
    ok = file_exists(p.p);
    free(p.p);
    return ok;
}

/* strip trailing backslash or slash */
static void trim_sep(wstr *s)
{
    while (s->len > 1 && (s->p[s->len - 1] == L'\\' || s->p[s->len - 1] == L'/')) {
        s->p[--s->len] = L'\0';
    }
}

/* the MSYS2 root comes from MSYS2_ROOT only; it must be set and pass the check */
static void find_root(wstr *out)
{
    wchar_t *env = _wgetenv(L"MSYS2_ROOT");

    if (!env || !*env)
        die(L"MSYS2_ROOT is not set "
            L"(point it at the MSYS2 installation directory, the one containing usr\\bin\\msys-2.0.dll)");
    if (!root_ok(env))
        die(L"MSYS2_ROOT does not point at a valid MSYS2 installation "
            L"(expected usr\\bin\\msys-2.0.dll inside it)");

    w_init(out);
    w_add(out, env);
    trim_sep(out);
}

/* ---------- Ctrl+C: let the child handle it, this process keeps waiting ---------- */

static BOOL WINAPI ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return TRUE;
    default:
        return FALSE;
    }
}

/* A low integrity level process may not create objects under \BaseNamedObjects,
   so msys-2.0 dies during initialisation with STATUS_ACCESS_DENIED (0xC0000022).
   Typical cause: the directory holding the exe carries a "Low Mandatory Level"
   label, which demotes the process to low IL. Warn up front with a fix. */
static void warn_if_low_il(void)
{
    HANDLE tok;
    BYTE buf[128];
    DWORD len = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return;
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &len) && len) {
        TOKEN_MANDATORY_LABEL *ml = (TOKEN_MANDATORY_LABEL *)buf;
        DWORD cnt = *GetSidSubAuthorityCount(ml->Label.Sid);
        DWORD rid = *GetSidSubAuthority(ml->Label.Sid, cnt - 1);
        if (rid < 0x2000) { /* < SECURITY_MANDATORY_MEDIUM_RID */
            eputs(L"msyszsh: warning: this process runs at low integrity level, so the\n"
                  L"msyszsh:          MSYS2 runtime cannot initialise and the shell will die.\n"
                  L"msyszsh:          fix: icacls \"<full path to msyszsh.exe>\" /setintegritylevel Medium\n");
        }
    }
    CloseHandle(tok);
}

/* Switch the console into VT mode before the shell starts. The console mode is
   a property of the console, not of this process, so the shell inherits it:
   MSYS2's runtime then treats the console as a VT terminal and passes escape
   sequences through both ways instead of swallowing the ESC, which is what
   leaves a literal "[200~" in the input line after a bracketed paste.
   ENABLE_VIRTUAL_TERMINAL_INPUT additionally makes the console deliver input as
   VT sequences (ESC[200~ / ESC[201~) rather than cooked key events.
   Returns the previous mode, or 0 when it could not be changed. */
static DWORD enable_vt_mode(DWORD std_handle, DWORD flag)
{
    HANDLE h = GetStdHandle(std_handle);
    DWORD mode = 0;

    if (!h || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode))
        return 0;
    if (mode & flag)
        return 0; /* already on, nothing to restore */
    if (!SetConsoleMode(h, mode | flag))
        return 0; /* not supported by this console (e.g. legacy console) */
    return mode;
}

static void restore_console_mode(DWORD std_handle, DWORD mode)
{
    HANDLE h = GetStdHandle(std_handle);

    if (mode && h && h != INVALID_HANDLE_VALUE)
        SetConsoleMode(h, mode);
}

static void print_help(void)
{
    eputs(L"Usage: msyszsh [options] [shell arguments]\n"
          L"\n"
          L"Starts the MSYS2 login shell (zsh by default) in the current console,\n"
          L"with MSYSTEM=MINGW64, the full Windows PATH exported and the current\n"
          L"directory preserved.\n"
          L"\n"
          L"Options:\n"
          L"  -shell SHELL      login shell to run (default: zsh)\n"
          L"  -mingw64 | -mingw32 | -ucrt64 | -clang64 | -clangarm64 | -msys[2]\n"
          L"                    set MSYSTEM (default: MINGW64)\n"
          L"  -[use-]full-path  export the full Windows PATH (default on)\n"
          L"  -here | -defterm | -no-start\n"
          L"                    accepted for msys2_shell.cmd compatibility, no effect\n"
          L"  -h | --help | -?  show this help and exit\n"
          L"  --                treat all remaining arguments as shell arguments\n"
          L"\n"
          L"Anything that is not a known option is passed to the login shell.\n");
}

int main(void)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *shell = L"zsh";
    const wchar_t *msystem = L"MINGW64";
    int full_path = 1;
    wstr extra;
    wstr root;
    wstr shellpath;
    wstr cmdline;
    UINT old_cp_in;
    UINT old_cp_out;
    DWORD old_out_mode;
    DWORD old_in_mode;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 0;
    int i;

    if (!argv)
        die(L"CommandLineToArgvW failed");

    old_cp_out = GetConsoleOutputCP();
    old_cp_in = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    old_out_mode = enable_vt_mode(STD_OUTPUT_HANDLE, ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    old_in_mode = enable_vt_mode(STD_INPUT_HANDLE, ENABLE_VIRTUAL_TERMINAL_INPUT);

    w_init(&extra);

    for (i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];

        if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help") || !wcscmp(a, L"-?") || !wcscmp(a, L"/?")) {
            print_help();
            return 0;
        } else if (!wcscmp(a, L"-shell")) {
            if (i + 1 >= argc)
                die(L"-shell requires a shell name");
            shell = argv[++i];
        } else if (!wcscmp(a, L"-mingw64")) {
            msystem = L"MINGW64";
        } else if (!wcscmp(a, L"-mingw32")) {
            msystem = L"MINGW32";
        } else if (!wcscmp(a, L"-ucrt64")) {
            msystem = L"UCRT64";
        } else if (!wcscmp(a, L"-clang64")) {
            msystem = L"CLANG64";
        } else if (!wcscmp(a, L"-clangarm64")) {
            msystem = L"CLANGARM64";
        } else if (!wcscmp(a, L"-msys") || !wcscmp(a, L"-msys2")) {
            msystem = L"MSYS";
        } else if (!wcscmp(a, L"-full-path") || !wcscmp(a, L"-use-full-path")) {
            full_path = 1;
        } else if (!wcscmp(a, L"-here") || !wcscmp(a, L"-defterm") || !wcscmp(a, L"-no-start")) {
            /* behaviour is built in, nothing to do */
        } else if (!wcscmp(a, L"--")) {
            for (++i; i < argc; ++i)
                w_add_quoted_arg(&extra, argv[i]);
            break;
        } else {
            w_add_quoted_arg(&extra, a);
        }
    }

    find_root(&root);

    /* absolute path of the shell: <root>\usr\bin\<shell>.exe */
    w_init(&shellpath);
    w_add(&shellpath, root.p);
    w_add(&shellpath, L"\\usr\\bin\\");
    w_add(&shellpath, shell);
    if (shellpath.len < 4 || _wcsicmp(shellpath.p + shellpath.len - 4, L".exe") != 0)
        w_add(&shellpath, L".exe");
    if (!file_exists(shellpath.p)) {
        wstr msg;
        w_init(&msg);
        w_add(&msg, L"login shell not found: ");
        w_add(&msg, shellpath.p);
        die(msg.p);
    }

    /* environment */
    SetEnvironmentVariableW(L"MSYSTEM", msystem);
    if (full_path)
        SetEnvironmentVariableW(L"MSYS2_PATH_TYPE", L"inherit");
    SetEnvironmentVariableW(L"CHERE_INVOKING", L"enabled_from_arguments");

    /* command line: "<shell>" -l [args...] */
    w_init(&cmdline);
    w_add_quoted_arg(&cmdline, shellpath.p);
    w_add(&cmdline, L" -l");
    if (extra.len)
        w_add(&cmdline, extra.p);

    warn_if_low_il();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(shellpath.p, cmdline.p, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        wstr msg;
        DWORD err = GetLastError();
        wchar_t num[32];
        _snwprintf(num, 32, L" (error %lu)", (unsigned long)err);
        w_init(&msg);
        w_add(&msg, L"cannot start ");
        w_add(&msg, cmdline.p);
        w_add(&msg, num);
        die(msg.p);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    restore_console_mode(STD_INPUT_HANDLE, old_in_mode);
    restore_console_mode(STD_OUTPUT_HANDLE, old_out_mode);
    SetConsoleOutputCP(old_cp_out);
    SetConsoleCP(old_cp_in);
    LocalFree(argv);
    return (int)exit_code;
}
