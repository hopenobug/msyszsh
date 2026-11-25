/*
 * msyszsh.exe - 等价于:
 *   chcp 65001
 *   <MSYS2_ROOT>\msys2_shell.cmd -defterm -no-start -use-full-path -here -mingw64 -shell zsh %*
 *
 * 在当前控制台里以登录 shell 方式启动 MSYS2 的 zsh，并转发全部参数。
 * MSYS2 安装位置必须由环境变量 MSYS2_ROOT 给出（指向含 usr\bin\msys-2.0.dll 的目录）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- 极简宽字符动态字符串 ---------- */

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

/* 按 MSVCRT 规则给参数加引号，然后拼到命令行（带前导空格） */
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

/* ---------- 错误输出（直接走控制台 UTF-8） ---------- */

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

/* ---------- 路径工具 ---------- */

static int file_exists(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* 合法 MSYS2 根目录: 存在 usr\bin\msys-2.0.dll */
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

/* 去掉末尾反斜杠/斜杠 */
static void trim_sep(wstr *s)
{
    while (s->len > 1 && (s->p[s->len - 1] == L'\\' || s->p[s->len - 1] == L'/')) {
        s->p[--s->len] = L'\0';
    }
}

/* MSYS2 根目录只从环境变量 MSYS2_ROOT 取，必须存在且通过校验 */
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

/* ---------- Ctrl+C: 让子进程处理，本进程在等待期间不退出 ---------- */

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

/* 低完整性级别(Low IL)的进程无法在 \BaseNamedObjects 中创建对象，msys-2.0 在
   初始化时会以 STATUS_ACCESS_DENIED(0xC0000022) 崩溃。典型诱因: exe 所在目录带
   "Low Mandatory Level" 标签，进程会被降到 Low IL。这里提前给出可操作的提示。 */
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
            /* 行为已内置，忽略 */
        } else if (!wcscmp(a, L"--")) {
            for (++i; i < argc; ++i)
                w_add_quoted_arg(&extra, argv[i]);
            break;
        } else {
            w_add_quoted_arg(&extra, a);
        }
    }

    find_root(&root);

    /* 拼 shell 绝对路径: <root>\usr\bin\<shell>.exe */
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

    /* 环境变量 */
    SetEnvironmentVariableW(L"MSYSTEM", msystem);
    if (full_path)
        SetEnvironmentVariableW(L"MSYS2_PATH_TYPE", L"inherit");
    SetEnvironmentVariableW(L"CHERE_INVOKING", L"enabled_from_arguments");

    /* 命令行: "<shell>" -l [args...] */
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

    SetConsoleOutputCP(old_cp_out);
    SetConsoleCP(old_cp_in);
    LocalFree(argv);
    return (int)exit_code;
}
