/**
 * @file installer.c
 * @brief Self-installer subcommands (install / uninstall / prep-image).
 *
 * Replaces deploy/install.ps1 + deploy/env-setup.ps1 + scripts/image-prep.ps1
 * with Win32 API calls so operators only need assessment-agent.exe — no
 * PowerShell scripts on the target host.
 *
 * Behaviour mirrors the prior ps1 trio one-to-one; see header for surface.
 */

#include "installer.h"
#include "service.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winreg.h>
#include <winsvc.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

/* ====================================================================
 *  User-level install layout — everything under
 *  %LOCALAPPDATA%\assessment-agent, resolved at runtime. No admin is needed
 *  to read/write these; only registering the boot scheduled task wants a
 *  one-time elevation.
 *
 *  Lazy accessors return pointers to per-path static buffers. The installer
 *  is single-threaded, so the lazy init needs no locking.
 * ==================================================================== */
static const wchar_t *p_base(void)
{
    static wchar_t b[MAX_PATH];
    if (!b[0] && agent_data_path_w(NULL, b, MAX_PATH) != 0) b[0] = L'\0';
    return b;
}
static const wchar_t *p_exe(void)
{
    static wchar_t b[MAX_PATH];
    if (!b[0] && agent_data_path_w(L"assessment-agent.exe", b, MAX_PATH) != 0) b[0] = L'\0';
    return b;
}
static const wchar_t *p_env(void)
{
    static wchar_t b[MAX_PATH];
    if (!b[0] && agent_data_path_w(L"agent.env", b, MAX_PATH) != 0) b[0] = L'\0';
    return b;
}
static const wchar_t *p_env_local(void)
{
    static wchar_t b[MAX_PATH];
    if (!b[0] && agent_data_path_w(L"agent.env.local", b, MAX_PATH) != 0) b[0] = L'\0';
    return b;
}
static const wchar_t *p_worker(void)
{
    static wchar_t b[MAX_PATH];
    if (!b[0] && agent_data_path_w(L"worker", b, MAX_PATH) != 0) b[0] = L'\0';
    return b;
}

/* Scheduled-task name (replaces the prior Windows service). */
#define TASK_NAME    L"AssessmentAgent"
#define SERVICE_DESC L"Resource assessment collector — publishes inventory/metrics/error to RabbitMQ."

/* env-setup.ps1 의 PromptKeys / SecretKeys 와 1:1 매핑. */
static const char *const PROMPT_KEYS[] = {
    "RABBITMQ_HOST",
    "WORKER_DOWNLOAD_ALLOWED_HOSTS",
    NULL,
};
static const char *const SECRET_KEYS[] = {
    "RABBITMQ_PASS",
    "RABBITMQ_WORKER_PASS",
    NULL,
};

static int str_in_list(const char *needle, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(*list, needle) == 0) return 1;
    return 0;
}

/* ====================================================================
 *  Win admin / version gates
 * ==================================================================== */
static int is_elevated(void)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    TOKEN_ELEVATION el = {0};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

/* Use RtlGetVersion — GetVersionExW lies without an app manifest entry. */
static int check_windows_version(void)
{
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOEXW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return 0;
    RtlGetVersion_t fn = (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion");
    if (!fn) return 0;
    RTL_OSVERSIONINFOEXW v = { .dwOSVersionInfoSize = sizeof v };
    if (fn(&v) != 0) return 0;
    if (v.dwMajorVersion < 10) {
        fprintf(stderr, "[install] unsupported Windows %lu.%lu — requires 10 / Server 2016+\n",
                (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion);
        return 0;
    }
    fprintf(stderr, "[install] OS         : Windows %lu.%lu build %lu\n",
            (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
            (unsigned long)v.dwBuildNumber);
    return 1;
}

/* ====================================================================
 *  Embedded RT_RCDATA — agent.env.example bytes shipped inside the exe.
 *  Returned buffer is the resource lock (do NOT free).
 * ==================================================================== */
static const char *load_env_example(size_t *out_len)
{
    HRSRC h = FindResourceW(NULL, L"AGENT_ENV_EXAMPLE", RT_RCDATA);
    if (!h) return NULL;
    HGLOBAL g = LoadResource(NULL, h);
    if (!g) return NULL;
    DWORD sz = SizeofResource(NULL, h);
    const char *p = (const char *)LockResource(g);
    if (!p) return NULL;
    *out_len = sz;
    return p;
}

/* ====================================================================
 *  Filesystem helpers
 * ==================================================================== */
static int ensure_dir(const wchar_t *path)
{
    int rc = SHCreateDirectoryExW(NULL, path, NULL);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
        rc == ERROR_FILE_EXISTS) return 0;
    fprintf(stderr, "[install] SHCreateDirectoryEx failed (%d) on %ls\n", rc, path);
    return -1;
}

/* (Former apply_strict_acl removed: user-level install lives under the user's
 * own %LOCALAPPDATA%, whose default ACL already restricts access to that user
 * plus SYSTEM/Administrators. A SYSTEM+Administrators-only ACL would lock out
 * the agent itself, which now runs as the user rather than LocalSystem.) */

static int copy_self_to(const wchar_t *target)
{
    wchar_t self[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        fprintf(stderr, "[install] could not resolve own executable path\n");
        return -1;
    }
    if (_wcsicmp(self, target) == 0) {
        /* Already in place (re-running from the install dir) — skip. */
        return 0;
    }
    if (!CopyFileW(self, target, FALSE)) {
        fprintf(stderr, "[install] CopyFile %ls -> %ls failed: %lu\n",
                self, target, (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

/* ====================================================================
 *  Scheduled-task control (replaces the Windows service)
 *
 *  User-level model: the agent runs as a per-user Task Scheduler job, not as
 *  a LocalSystem service. Registering an ONSTART (boot, whether-logged-on-or-
 *  not) task for the current user needs admin ONCE (it creates an S4U task —
 *  no stored password); the agent itself then runs unprivileged as that user.
 *
 *  We drive schtasks.exe rather than the Task Scheduler COM API: far less code
 *  and no extra dependency. CreateProcessW (not _wsystem) avoids a cmd.exe
 *  quoting layer.
 * ==================================================================== */

/* Run a command line synchronously, no console window. Returns the child exit
 * code, or -1 on spawn failure. The buffer is copied because CreateProcessW
 * may write into its lpCommandLine argument. */
static int run_process_w(const wchar_t *cmdline)
{
    wchar_t buf[2048];
    if ((size_t)_snwprintf(buf, 2048, L"%ls", cmdline) >= 2048) return -1;

    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[install] CreateProcess failed: %lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* "DOMAIN\user" of the current process, for schtasks /RU. Falls back to the
 * bare username if USERDOMAIN is unset. */
static int current_user_w(wchar_t *out, size_t cap)
{
    wchar_t dom[128] = {0}, usr[128] = {0};
    DWORD nd = GetEnvironmentVariableW(L"USERDOMAIN", dom, 128);
    DWORD nu = GetEnvironmentVariableW(L"USERNAME",   usr, 128);
    if (nu == 0 || nu >= 128) return -1;
    int w = (nd > 0 && nd < 128)
        ? _snwprintf(out, cap, L"%ls\\%ls", dom, usr)
        : _snwprintf(out, cap, L"%ls", usr);
    return (w > 0 && (size_t)w < cap) ? 0 : -1;
}

static int task_exists(void)
{
    return run_process_w(L"schtasks /Query /TN " TASK_NAME) == 0;
}

static int stop_task_if_running(void)
{
    /* Best-effort: /End is harmless if the task is not running, and lets the
     * exe file be overwritten on upgrade. */
    run_process_w(L"schtasks /End /TN " TASK_NAME);
    Sleep(1500);
    return 0;
}

static int register_task(void)
{
    wchar_t user[260];
    if (current_user_w(user, 260) != 0) {
        fprintf(stderr, "[install] could not resolve current user for task principal\n");
        return -1;
    }

    /* Action = "<exe>" run  (foreground agent loop). On the schtasks command
     * line the /TR value is one quoted token that itself contains quotes
     * around the exe path, so the inner quotes are backslash-escaped.
     *
     * /SC ONSTART       — start at boot, before any interactive logon.
     * /RU <user> (no /RP)— S4U: runs whether logged on or not, no stored
     *                      password. Creating this requires admin ONCE.
     * /RL LIMITED       — runtime stays unprivileged (no elevation).
     * /F                — overwrite an existing task (upgrade/idempotent). */
    wchar_t cmd[1536];
    int w = _snwprintf(cmd, 1536,
        L"schtasks /Create /TN " TASK_NAME
        L" /TR \"\\\"%ls\\\" run\""
        L" /SC ONSTART /RU \"%ls\" /RL LIMITED /F",
        p_exe(), user);
    if (w <= 0 || (size_t)w >= 1536) return -1;

    int rc = run_process_w(cmd);
    if (rc != 0) {
        fprintf(stderr, "[install] schtasks /Create failed (exit %d).\n", rc);
        fprintf(stderr, "[install]   Registering a boot task that runs whether you are\n");
        fprintf(stderr, "[install]   logged on or not needs a one-time elevated (Admin)\n");
        fprintf(stderr, "[install]   shell. The agent then runs as you, unprivileged.\n");
        return -1;
    }
    return 0;
}

static int run_task(void)
{
    return run_process_w(L"schtasks /Run /TN " TASK_NAME) == 0 ? 0 : -1;
}

/* ====================================================================
 *  env-setup — embedded example drives the prompt loop.
 *
 *  Key list: parsed from the embedded agent.env.example. Both bare
 *  (KEY=value) and commented-out (# KEY=value) lines feed the canonical
 *  key list; commented secrets are tracked separately via SECRET_KEYS.
 * ==================================================================== */
typedef struct kv {
    char *key;
    char *value;
    int   commented;  /* from a "# KEY=value" line in the example */
} kv_t;

typedef struct kv_arr {
    kv_t  *items;
    size_t len, cap;
} kv_arr_t;

static void kv_arr_free(kv_arr_t *a)
{
    for (size_t i = 0; i < a->len; i++) {
        free(a->items[i].key);
        free(a->items[i].value);
    }
    free(a->items);
    a->items = NULL; a->len = a->cap = 0;
}

static int kv_arr_push(kv_arr_t *a, const char *k, const char *v, int commented)
{
    if (a->len == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        kv_t *ni = realloc(a->items, nc * sizeof *ni);
        if (!ni) return -1;
        a->items = ni; a->cap = nc;
    }
    a->items[a->len].key       = _strdup(k);
    a->items[a->len].value     = _strdup(v ? v : "");
    a->items[a->len].commented = commented;
    if (!a->items[a->len].key || !a->items[a->len].value) return -1;
    a->len++;
    return 0;
}

static kv_t *kv_arr_find(kv_arr_t *a, const char *k)
{
    for (size_t i = 0; i < a->len; i++)
        if (strcmp(a->items[i].key, k) == 0) return &a->items[i];
    return NULL;
}

static int kv_arr_set(kv_arr_t *a, const char *k, const char *v)
{
    kv_t *e = kv_arr_find(a, k);
    if (e) {
        char *nv = _strdup(v ? v : "");
        if (!nv) return -1;
        free(e->value);
        e->value = nv;
        e->commented = 0;
        return 0;
    }
    return kv_arr_push(a, k, v, 0);
}

static void trim_inplace(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int is_key_char(int c, int first)
{
    if (first) return c == '_' || (c >= 'A' && c <= 'Z');
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Parse a single "KEY=value" line.
 *   leading_hash != NULL → also accept a single leading `#` (commented-out
 *                          template entry). *leading_hash set to 1 if seen.
 * Returns 0 on success (key/value written to out), -1 if not a kv line. */
static int parse_kv_line(const char *line, char *key, size_t kcap,
                         char *val, size_t vcap, int *leading_hash)
{
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    int hash = 0;
    if (*p == '#') {
        if (!leading_hash) return -1;
        hash = 1;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (!is_key_char(*p, 1)) return -1;

    size_t kl = 0;
    while (is_key_char(*p, 0)) {
        if (kl + 1 >= kcap) return -1;
        key[kl++] = *p++;
    }
    key[kl] = '\0';

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    size_t vl = 0;
    while (*p && *p != '\n' && *p != '\r') {
        if (vl + 1 >= vcap) return -1;
        val[vl++] = *p++;
    }
    val[vl] = '\0';
    trim_inplace(val);
    /* Strip matching surrounding quotes. */
    size_t vlen = strlen(val);
    if (vlen >= 2 &&
        ((val[0] == '"'  && val[vlen - 1] == '"') ||
         (val[0] == '\'' && val[vlen - 1] == '\''))) {
        memmove(val, val + 1, vlen - 2);
        val[vlen - 2] = '\0';
    }
    if (leading_hash) *leading_hash = hash;
    return 0;
}

/* Parse a byte buffer or file by splitting on newlines, feeding each line
 * into a callback. Used for both the embedded example and on-disk files. */
typedef void (*line_cb_t)(const char *line, void *ctx);

static void iterate_lines(const char *buf, size_t len, line_cb_t cb, void *ctx)
{
    size_t i = 0;
    char line[4096];
    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        size_t n = j - i;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, buf + i, n);
        line[n] = '\0';
        cb(line, ctx);
        i = j + 1;
    }
}

static void example_line_cb(const char *line, void *ctx)
{
    kv_arr_t *a = (kv_arr_t *)ctx;
    char k[128], v[2048];
    int hash = 0;
    if (parse_kv_line(line, k, sizeof k, v, sizeof v, &hash) == 0) {
        if (!kv_arr_find(a, k)) kv_arr_push(a, k, v, hash);
    }
}

static void existing_line_cb(const char *line, void *ctx)
{
    kv_arr_t *a = (kv_arr_t *)ctx;
    char k[128], v[2048];
    if (parse_kv_line(line, k, sizeof k, v, sizeof v, NULL) == 0)
        kv_arr_set(a, k, v);
}

static int read_file_all(const wchar_t *path, char **out, size_t *out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        *out = NULL; *out_len = 0;
        return -1;
    }
    LARGE_INTEGER sz; sz.QuadPart = 0;
    GetFileSizeEx(f, &sz);
    char *buf = malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(f); return -1; }
    DWORD rd = 0;
    ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, NULL);
    buf[rd] = '\0';
    CloseHandle(f);
    *out = buf; *out_len = rd;
    return 0;
}

/* Atomic write — UTF-8, no BOM (matches env-setup.ps1 behaviour and the
 * Linux env file format read by load_env_file). */
static int write_env_file(const wchar_t *path, const kv_arr_t *a, int skip_commented)
{
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    for (size_t i = 0; i < a->len; i++) {
        if (skip_commented && a->items[i].commented) continue;
        char line[2560];
        int n = _snprintf(line, sizeof line, "%s=%s\r\n",
                          a->items[i].key, a->items[i].value);
        if (n > 0) {
            DWORD w = 0;
            WriteFile(f, line, (DWORD)n, &w, NULL);
        }
    }
    CloseHandle(f);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) return -1;
    return 0;
}

/* Console input with optional echo suppression. Returns the trimmed line.
 * Wide is not needed — values are ASCII / UTF-8 bytes. */
static int read_console_line(char *buf, size_t cap, int echo)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD prev = 0;
    int restored = 0;
    if (!echo) {
        if (GetConsoleMode(in, &prev)) {
            SetConsoleMode(in, prev & ~ENABLE_ECHO_INPUT);
            restored = 1;
        }
    }
    int rc = -1;
    if (fgets(buf, (int)cap, stdin)) {
        trim_inplace(buf);
        rc = 0;
    }
    if (restored) {
        SetConsoleMode(in, prev);
        fputc('\n', stdout);  /* echo-off swallowed the user's Enter */
    }
    return rc;
}

static int env_setup_run(const char *example, size_t example_len)
{
    kv_arr_t schema = {0};  /* canonical key list from the example */
    iterate_lines(example, example_len, example_line_cb, &schema);

    /* --- agent.env (non-secret) --- */
    kv_arr_t env_state = {0};
    /* Seed from example so the file is complete even if every non-prompt key
     * silently takes the default (preserves Linux env_setup behaviour). */
    for (size_t i = 0; i < schema.len; i++) {
        if (schema.items[i].commented) continue;
        if (str_in_list(schema.items[i].key, SECRET_KEYS)) continue;
        kv_arr_push(&env_state, schema.items[i].key,
                    schema.items[i].value, 0);
    }
    {
        char *existing = NULL; size_t elen = 0;
        if (read_file_all(p_env(), &existing, &elen) == 0 && existing) {
            iterate_lines(existing, elen, existing_line_cb, &env_state);
            free(existing);
        }
    }
    for (size_t i = 0; i < schema.len; i++) {
        const char *k = schema.items[i].key;
        if (schema.items[i].commented) continue;
        if (str_in_list(k, SECRET_KEYS)) continue;
        if (!str_in_list(k, PROMPT_KEYS)) continue;
        kv_t *cur = kv_arr_find(&env_state, k);
        if (cur && cur->value && *cur->value &&
            strcmp(cur->value, schema.items[i].value) != 0) {
            /* already customized — never overwrite */
            continue;
        }
        /* Non-interactive (high-latency / no console): a preset in the
         * environment wins outright — `set RABBITMQ_HOST=broker & install`. */
        const char *envp = getenv(k);
        if (envp && *envp) {
            kv_arr_set(&env_state, k, envp);
            continue;
        }
        const char *def = schema.items[i].value;
        if (def && *def) {
            fprintf(stdout, "%s [%s]: ", k, def);
        } else {
            fprintf(stdout, "%s: ", k);
        }
        fflush(stdout);
        char input[1024] = {0};
        if (read_console_line(input, sizeof input, 1) == 0 && *input) {
            kv_arr_set(&env_state, k, input);
        } else if (def && *def) {
            kv_arr_set(&env_state, k, def);
        }
    }
    if (write_env_file(p_env(), &env_state, 1) != 0) {
        fprintf(stderr, "[install] writing %ls failed\n", p_env());
        kv_arr_free(&schema); kv_arr_free(&env_state);
        return -1;
    }
    fprintf(stdout, "[install] wrote %ls\n", p_env());

    /* --- agent.env.local (secret) --- */
    kv_arr_t local_state = {0};
    {
        char *existing = NULL; size_t elen = 0;
        if (read_file_all(p_env_local(), &existing, &elen) == 0 && existing) {
            iterate_lines(existing, elen, existing_line_cb, &local_state);
            free(existing);
        }
    }
    for (const char *const *k = SECRET_KEYS; *k; k++) {
        kv_t *cur = kv_arr_find(&local_state, *k);
        if (cur && cur->value && *cur->value) continue;
        /* Preset secret in the environment → use it, no prompt. */
        const char *envp = getenv(*k);
        if (envp && *envp) {
            kv_arr_set(&local_state, *k, envp);
            continue;
        }
        fprintf(stdout, "%s (input hidden): ", *k);
        fflush(stdout);
        char input[1024] = {0};
        if (read_console_line(input, sizeof input, 0) == 0 && *input)
            kv_arr_set(&local_state, *k, input);
    }
    if (write_env_file(p_env_local(), &local_state, 0) != 0) {
        fprintf(stderr, "[install] writing %ls failed\n", p_env_local());
        kv_arr_free(&schema); kv_arr_free(&env_state); kv_arr_free(&local_state);
        return -1;
    }
    /* No explicit ACL: the file lives under the user's own %LOCALAPPDATA%,
     * whose default ACL already grants only this user (+ SYSTEM/Admins). The
     * old SYSTEM+Administrators-only ACL would lock out the agent itself,
     * which now runs as this user, not LocalSystem. */
    fprintf(stdout, "[install] wrote %ls (user-private)\n", p_env_local());

    kv_arr_free(&schema);
    kv_arr_free(&env_state);
    kv_arr_free(&local_state);
    return 0;
}

/* ====================================================================
 *  install
 * ==================================================================== */
int installer_run_install(int image_prep)
{
    fprintf(stdout, "\n=== Assessment Agent installer (user-level) ===\n");
    fprintf(stdout, "Installs under %%LOCALAPPDATA%%\\assessment-agent and runs as you.\n");
    fprintf(stdout, "One-time Administrator rights are needed only to register the\n");
    fprintf(stdout, "boot scheduled task; the agent itself runs unprivileged.\n\n");

    if (!check_windows_version()) return 1;

    if (!p_base()[0] || !p_exe()[0]) {
        fprintf(stderr, "[install] could not resolve %%LOCALAPPDATA%% — aborting\n");
        return 1;
    }

    size_t ex_len = 0;
    const char *ex = load_env_example(&ex_len);
    if (!ex) {
        fprintf(stderr, "[install] embedded env example missing — corrupt binary\n");
        return 1;
    }

    /* Stop a previously-registered task so the exe can be overwritten. */
    if (task_exists())
        stop_task_if_running();

    wchar_t sub[MAX_PATH];
    int dirs_ok =
        ensure_dir(p_base()) == 0 &&
        ensure_dir(p_worker()) == 0;
    if (dirs_ok && _snwprintf(sub, MAX_PATH, L"%ls\\results", p_worker()) > 0)
        dirs_ok = ensure_dir(sub) == 0;
    if (dirs_ok && _snwprintf(sub, MAX_PATH, L"%ls\\done", p_worker()) > 0)
        dirs_ok = ensure_dir(sub) == 0;
    if (dirs_ok && _snwprintf(sub, MAX_PATH, L"%ls\\running", p_worker()) > 0)
        dirs_ok = ensure_dir(sub) == 0;
    if (!dirs_ok) return 1;

    if (copy_self_to(p_exe()) != 0)
        return 1;
    fprintf(stdout, "[install] binary     : %ls\n", p_exe());

    if (env_setup_run(ex, ex_len) != 0)
        return 1;

    /* Register / refresh the boot scheduled task (needs admin once). */
    fprintf(stdout, "[install] registering scheduled task '%ls'...\n", TASK_NAME);
    if (register_task() != 0)
        return 1;

    if (image_prep) {
        fprintf(stdout, "\n[install] --image-prep — task registered, NOT started.\n");
        fprintf(stdout, "[install] before sealing the VM image, run:\n");
        fprintf(stdout, "[install]     assessment-agent.exe prep-image\n\n");
        return 0;
    }

    fprintf(stdout, "[install] starting task...\n");
    if (run_task() != 0) {
        fprintf(stderr, "[install] schtasks /Run failed — start it manually with\n");
        fprintf(stderr, "[install]   schtasks /Run /TN %ls\n", TASK_NAME);
        return 1;
    }

    fprintf(stdout, "\n[install] OK — assessment-agent scheduled task is running.\n");
    fprintf(stdout, "[install] status:    schtasks /Query /TN %ls /V /FO LIST\n", TASK_NAME);
    fprintf(stdout, "[install] stop:      schtasks /End /TN %ls\n", TASK_NAME);
    fprintf(stdout, "[install] uninstall: assessment-agent.exe uninstall\n");
    return 0;
}

/* ====================================================================
 *  uninstall — stop + delete the scheduled task; leave on-disk state alone.
 *
 *  Removing a boot task that was registered to "run whether logged on or not"
 *  needs the same one-time admin as install. We attempt it regardless and let
 *  schtasks report a clear error if not elevated.
 * ==================================================================== */
int installer_run_uninstall(void)
{
    if (!task_exists()) {
        fprintf(stdout, "[uninstall] scheduled task not registered — nothing to do\n");
        return 0;
    }
    stop_task_if_running();
    if (run_process_w(L"schtasks /Delete /TN " TASK_NAME L" /F") != 0) {
        fprintf(stderr, "[uninstall] schtasks /Delete failed — re-run from an\n");
        fprintf(stderr, "[uninstall]   elevated (Administrator) shell.\n");
        return 1;
    }
    fprintf(stdout, "[uninstall] scheduled task removed. on-disk state preserved at:\n");
    fprintf(stdout, "[uninstall]   %ls\n", p_base());
    return 0;
}

/* ====================================================================
 *  prep-image — regenerate HKLM MachineGuid (+ optional sysprep).
 * ==================================================================== */
static int regenerate_machine_guid(void)
{
    /* Build a UUID v4 string via compat_rand_bytes (bcrypt → CryptoAPI
     * fallback, so this works on NT 5.2 / Server 2003 too). 36-char canonical
     * form, lowercased, to match the format Windows stores under MachineGuid. */
    unsigned char r[16];
    if (!compat_rand_bytes(r, sizeof r)) return -1;
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;

    wchar_t guid[40];
    _snwprintf(guid, 40,
               L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
               r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);

    HKEY k;
    LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\Microsoft\\Cryptography",
                               0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[prep-image] RegOpenKey failed: %ld\n", rc);
        return -1;
    }
    /* Old value, for the operator's audit trail. */
    wchar_t prev[64] = {0};
    DWORD plen = sizeof prev;
    DWORD type = REG_SZ;
    RegQueryValueExW(k, L"MachineGuid", NULL, &type, (LPBYTE)prev, &plen);

    DWORD vlen = (DWORD)((wcslen(guid) + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(k, L"MachineGuid", 0, REG_SZ, (const BYTE *)guid, vlen);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[prep-image] RegSetValueEx failed: %ld\n", rc);
        return -1;
    }
    fprintf(stdout, "[prep-image] MachineGuid\n");
    if (*prev) fprintf(stdout, "[prep-image]   was : %ls\n", prev);
    fprintf(stdout, "[prep-image]   now : %ls\n", guid);
    return 0;
}

int installer_run_prep_image(int run_sysprep)
{
    fprintf(stdout, "\n=== Assessment Agent — image preparation ===\n");
    fprintf(stdout, "Run once on the GOLDEN TEMPLATE before snapshotting.\n\n");

    /* Stop the scheduled task so it isn't running in the sealed image. */
    if (task_exists())
        stop_task_if_running();

    /* MachineGuid regen is best-effort: machine_id is display-only — the
     * engine keys hosts on composite_id = sha256(machine_id + MACs), which
     * already diverges per clone via fresh NIC MACs. Regen needs admin (HKLM);
     * when not elevated we skip with a note instead of failing. */
    if (!is_elevated()) {
        fprintf(stdout, "[prep-image] not elevated — SKIP MachineGuid reset.\n");
        fprintf(stdout, "[prep-image]   Not fatal: composite_id diverges via per-clone MACs.\n");
        fprintf(stdout, "[prep-image]   Run from an Admin shell to also reset MachineGuid.\n");
    } else if (regenerate_machine_guid() != 0) {
        return 1;
    }

    if (run_sysprep) {
        const wchar_t sys[] = L"C:\\Windows\\System32\\Sysprep\\Sysprep.exe";
        fprintf(stdout, "[prep-image] launching sysprep /generalize /oobe /shutdown...\n");
        HINSTANCE rc = ShellExecuteW(NULL, L"open", sys,
                                     L"/generalize /oobe /shutdown",
                                     NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            fprintf(stderr, "[prep-image] ShellExecute failed: %lld\n", (long long)(INT_PTR)rc);
            return 1;
        }
    } else {
        fprintf(stdout, "\n[prep-image] recommended next step (full generalization):\n");
        fprintf(stdout, "    C:\\Windows\\System32\\Sysprep\\Sysprep.exe /generalize /oobe /shutdown\n");
        fprintf(stdout, "  or rerun with --sysprep\n\n");
    }
    fprintf(stdout, "[prep-image] done.\n");
    return 0;
}
