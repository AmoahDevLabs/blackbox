#define _POSIX_C_SOURCE 200809L

#include "opencode.h"
#include "sha256.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *PYTHON_INTEGRITY =
    "import sqlite3,sys\n"
    "try:\n"
    "    c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True)\n"
    "    print(c.execute('PRAGMA integrity_check').fetchone()[0])\n"
    "except Exception as e:\n"
    "    print('ERROR: '+str(e))\n";

static void build_paths(char bin[BB_PATH_MAX], char db[BB_PATH_MAX])
{
    char home[BB_PATH_MAX] = "/";
    if (!bb_home(home)) {
        fputs("opencode: could not determine HOME; using /\n", stderr);
    }
    (void)bb_path_join(bin, home, ".opencode/bin/opencode");
    (void)bb_path_join(db, home, ".local/share/opencode/opencode.db");
}

static bool json_find_string(const char *json, const char *key, char *out, size_t outsz)
{
    if (json == NULL || key == NULL || out == NULL || outsz == 0u) {
        return false;
    }
    char needle[128];
    int n = snprintf(needle, sizeof needle, "\"%s\":\"", key);
    if (n < 0 || (size_t)n >= sizeof needle) {
        return false;
    }
    const char *p = strstr(json, needle);
    if (p == NULL) {
        /* also accept whitespace after colon */
        int n2 = snprintf(needle, sizeof needle, "\"%s\": \"", key);
        if (n2 < 0 || (size_t)n2 >= sizeof needle) {
            return false;
        }
        p = strstr(json, needle);
    }
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    size_t i = 0u;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p != '\0') {
            c = *p++; /* keep escaped char value simply */
        }
        if (i + 1u < outsz) {
            out[i++] = c;
        }
    }
    if (*p != '"') {
        return false;
    }
    out[i] = '\0';
    return i > 0u;
}

static bool json_find_file_sha(const char *json, const char *file, char *out, size_t outsz)
{
    if (json == NULL || file == NULL || out == NULL) {
        return false;
    }
    char needle[256];
    int n = snprintf(needle, sizeof needle, "\"%s\":", file);
    if (n < 0 || (size_t)n >= sizeof needle) {
        return false;
    }
    const char *p = strstr(json, needle);
    if (p == NULL) {
        return false;
    }
    /* search within a reasonable window after the file entry */
    const char *window = p;
    const char *sha = strstr(window, "\"sha256\":\"");
    if (sha == NULL || (size_t)(sha - window) > 4096u) {
        return false;
    }
    sha += strlen("\"sha256\":\"");
    size_t i = 0u;
    while (*p != '\0' && sha[i] != '"' && i + 1u < outsz) {
        char c = sha[i];
        if (c == '\0' || c == '"') {
            break;
        }
        out[i] = c;
        i++;
    }
    /* copy properly */
    i = 0u;
    while (sha[i] != '\0' && sha[i] != '"' && i + 1u < outsz) {
        out[i] = sha[i];
        i++;
    }
    if (sha[i] != '"') {
        out[0] = '\0';
        return false;
    }
    out[i] = '\0';
    return i > 0u;
}

static bool cache_fresh(const char *path, unsigned int max_age_seconds)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1 || st.st_mtime > now) {
        return false;
    }
    return (unsigned long long)(now - st.st_mtime) <= (unsigned long long)max_age_seconds;
}

static bool read_small_file(const char *path, char *buf, size_t bufsz)
{
    if (path == NULL || buf == NULL || bufsz < 2u) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t used = 0u;
    for (;;) {
        ssize_t n = read(fd, buf + used, bufsz - 1u - used);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return false;
        }
        used += (size_t)n;
        if (used + 1u >= bufsz) {
            break;
        }
    }
    buf[used] = '\0';
    close(fd);
    return used > 0u;
}

static bool write_small_file(const char *path, const char *data)
{
    if (path == NULL || data == NULL) {
        return false;
    }
    char tmp[BB_PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    size_t len = strlen(data);
    size_t off = 0u;
    bool ok = true;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return false;
    }
    return true;
}

static bool fetch_stable_release(char *json, size_t jsonsz)
{
    const char *channel = getenv("BLACKBOX_OPENCODE_CHANNEL");
    if (channel == NULL || channel[0] == '\0') {
        channel = "stable";
    }

    char home[BB_PATH_MAX] = "/";
    if (!bb_home(home)) {
        return false;
    }
    char cache_dir[BB_PATH_MAX];
    char cache_file[BB_PATH_MAX];
    if (!bb_path_join(cache_dir, home, ".cache/blackbox") ||
        !bb_path_join(cache_file, cache_dir, "opencode-release.json")) {
        return false;
    }

    const unsigned int ttl = 6u * 60u * 60u;
    if (cache_fresh(cache_file, ttl) && read_small_file(cache_file, json, jsonsz)) {
        char cached_channel[64];
        if (json_find_string(json, "channel", cached_channel, sizeof cached_channel) &&
            strcmp(cached_channel, channel) == 0) {
            return true;
        }
    }

    char url[512];
    int n = snprintf(url, sizeof url,
                     "https://opencode.ai/update/api/latest/cli/opencode?channel=%s", channel);
    if (n < 0 || (size_t)n >= sizeof url) {
        return false;
    }

    char out[BB_OUT_MAX];
    int raw = 0;
    char *argv[] = { "curl", "-fsSL", "--max-time", "5", url, NULL };
    if (!bb_run_capture(argv, out, sizeof out, &raw) || bb_status_exit_code(raw) != 0) {
        /* fall back to stale cache if present */
        return read_small_file(cache_file, json, jsonsz);
    }
    if (out[0] != '{') {
        return read_small_file(cache_file, json, jsonsz);
    }

    size_t len = strlen(out);
    if (len + 1u > jsonsz) {
        return false;
    }
    memcpy(json, out, len + 1u);

    (void)mkdir(cache_dir, 0755);
    (void)write_small_file(cache_file, json);
    return true;
}

static const char *platform_tarball(void)
{
#if defined(__linux__) && defined(__x86_64__)
    return "opencode-linux-x64.tar.gz";
#elif defined(__linux__) && defined(__aarch64__)
    return "opencode-linux-arm64.tar.gz";
#else
    return NULL;
#endif
}

static void check_binary(const char *bin)
{
    char digest[65];
    if (!bb_sha256_file_hex(bin, digest) || access(bin, X_OK) != 0) {
        puts("Binary:    CORRUPT/UNREADABLE - reinstall with: opencode upgrade --force");
        return;
    }
    char out[BB_OUT_MAX];
    int raw = 0;
    char version[128] = "unknown";
    if (bb_run_capture((char *[]){ (char *)bin, "--version", NULL }, out, sizeof out, &raw)) {
        bb_first_line(out);
        if (out[0] != '\0') {
            snprintf(version, sizeof version, "%.120s", out);
        }
    }
    printf("Binary:    OK - %s\n", version);
    printf("SHA256:    %s\n", digest);

    const char *env_expected = getenv("BLACKBOX_EXPECT_OPENCODE_SHA256");
    if (env_expected != NULL && env_expected[0] != '\0') {
        if (strcasecmp(digest, env_expected) == 0) {
            puts("Hash:      matches BLACKBOX_EXPECT_OPENCODE_SHA256");
        } else {
            printf("Hash:      MISMATCH - expected %s\n", env_expected);
        }
    }

    char json[BB_OUT_MAX];
    if (!fetch_stable_release(json, sizeof json)) {
        puts("Release:   could not check stable channel (offline or curl unavailable)");
        puts("           Set BLACKBOX_OPENCODE_CHANNEL=stable (default) or latest.");
        return;
    }

    char latest[64];
    char channel[64] = "stable";
    if (!json_find_string(json, "version", latest, sizeof latest)) {
        puts("Release:   could not parse stable release metadata");
        return;
    }
    (void)json_find_string(json, "channel", channel, sizeof channel);

    const char *installed = version;
    if (strncmp(installed, "opencode v", 10) == 0) {
        installed += 10;
    } else if (installed[0] == 'v') {
        installed++;
    }

    if (strcmp(installed, latest) == 0) {
        printf("Release:   up to date with %s channel (v%s)\n", channel, latest);
    } else {
        printf("Release:   installed v%s, %s channel latest is v%s\n", installed, channel, latest);
        printf("           Upgrade: opencode upgrade %s\n", latest);
    }

    const char *tar = platform_tarball();
    if (tar != NULL) {
        char tar_sha[128];
        if (json_find_file_sha(json, tar, tar_sha, sizeof tar_sha)) {
            printf("Tarball:   %s sha256=%s\n", tar, tar_sha);
        }
    }
}

static void check_database(const char *db)
{
    if (!bb_file_exists(db)) {
        printf("Database:  missing (%s)\n", db);
        return;
    }
    char out[BB_OUT_MAX];
    int raw = 0;
    char *argv[] = { "python3", "-c", (char *)PYTHON_INTEGRITY, (char *)db, NULL };
    if (bb_run_capture(argv, out, sizeof out, &raw)) {
        bb_first_line(out);
        if (out[0] == '\0') {
            puts("Database:  no result");
        } else {
            printf("Database:  %s\n", out);
        }
    } else {
        puts("Database:  ERROR: could not run python3");
    }
}

static void check_core_dumps(void)
{
    char out[BB_OUT_MAX];
    int raw = 0;
    char *argv[] = { "coredumpctl", "--no-pager", "list", "opencode", NULL };
    if (!bb_run_capture(argv, out, sizeof out, &raw)) {
        return;
    }
    if (bb_status_exit_code(raw) == 127) {
        return;
    }
    puts("Recent OpenCode core dumps:");
    if (out[0] == '\0') {
        puts("  none");
    } else {
        bb_print_tail(out, 5);
    }
}

static void check_service(void)
{
    pid_t pids[1];
    size_t count = 0u;
    if (bb_find_pids("opencode serve --service", pids, 1u, &count) > 0u) {
        printf("Service:   running (PID %ld)\n", (long)pids[0]);
    } else {
        puts("Service:   not running");
    }
}

static void check_temp_libs(void)
{
    DIR *dir = opendir("/tmp");
    if (dir == NULL) {
        puts("Temp libs: unavailable");
        return;
    }

    size_t count = 0u;
    unsigned long long bytes = 0ull;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        bool bun = strncmp(name, ".bun-1000-", 10u) == 0;
        bool opentui = strstr(name, "opentui") != NULL;
        if (!bb_ends_with(name, ".so") || (!bun && !opentui)) {
            continue;
        }
        char path[BB_PATH_MAX];
        if (snprintf(path, sizeof path, "/tmp/%s", name) >= (int)sizeof path) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) == 0) {
            count++;
            bytes += (unsigned long long)st.st_size;
        }
    }
    closedir(dir);

    if (count == 0u) {
        puts("Temp libs: none");
        return;
    }
    double mib = (double)bytes / 1048576.0;
    printf("Temp libs: %zu files (%.1f MiB) in /tmp\n", count, mib);
    struct statvfs vfs;
    if (statvfs("/tmp", &vfs) == 0) {
        double free_mib = (double)vfs.f_bavail * (double)vfs.f_frsize / 1048576.0;
        printf("Temp free: %.1f GiB\n", free_mib / 1024.0);
    }
    if (count > 10u) {
        puts("  Many leftovers can fill /tmp and break TUI startup.");
        puts("  After quitting ALL OpenCode processes: rm -f /tmp/.bun-1000-*.so /tmp/*opentui*.so");
    }
}

int cmd_opencode_doctor(void)
{
    char bin[BB_PATH_MAX];
    char db[BB_PATH_MAX];
    build_paths(bin, db);

    puts("== OpenCode doctor ==");
    check_binary(bin);
    check_database(db);
    check_core_dumps();
    check_service();
    check_temp_libs();

    if (getenv("OTUI_NO_NATIVE_RENDER") != NULL) {
        puts("Note:      OTUI_NO_NATIVE_RENDER is set but ignored by v2 (removed upstream).");
    }

    putchar('\n');
    puts("If the TUI segfaults (SIGSEGV / exit 139):");
    puts("  opencode-safe             # auto-retries in mini mode after a crash");
    puts("  opencode mini             # minimal interface, no native renderer");
    puts("  opencode-safe --mini      # start in mini mode immediately");
    puts("  opencode service restart  # if the background service is wedged");
    return 0;
}

static size_t build_child_args(char *child[BB_CMD_MAX], const char *bin,
                               bool mini, int argc, char **argv)
{
    size_t n = 0u;
    child[n++] = (char *)bin;
    if (mini) {
        child[n++] = (char *)"mini";
    }
    for (int i = 0; i < argc && n + 1u < BB_CMD_MAX; i++) {
        child[n++] = argv[i];
    }
    child[n] = NULL;
    return n;
}

static int run_opencode(char *const child[])
{
    int raw = 0;
    if (bb_run_raw(child, &raw) != 0) {
        fputs("opencode-safe: could not execute opencode\n", stderr);
        return 1;
    }
    int code = bb_status_exit_code(raw);
    return code < 0 ? 1 : code;
}

int cmd_opencode_safe(int argc, char **argv)
{
    bool mini = false;
    if (argc > 0 && (strcmp(argv[0], "--mini") == 0 || strcmp(argv[0], "--no-native") == 0)) {
        mini = true;
        argc--;
        argv++;
    }

    char bin[BB_PATH_MAX] = "/";
    char home[BB_PATH_MAX] = "/";
    if (!bb_home(home)) {
        fputs("opencode-safe: could not determine HOME; using /\n", stderr);
    }
    (void)bb_path_join(bin, home, ".opencode/bin/opencode");
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "opencode-safe: %s is not executable\n", bin);
        return 1;
    }

    char *child[BB_CMD_MAX];
    if (mini) {
        puts("[opencode-safe] Starting minimal interface (no native renderer).");
        (void)build_child_args(child, bin, true, argc, argv);
        return run_opencode(child);
    }

    (void)build_child_args(child, bin, false, argc, argv);
    int raw = 0;
    if (bb_run_raw(child, &raw) != 0) {
        fputs("opencode-safe: could not execute opencode\n", stderr);
        return 1;
    }

    if (!bb_status_crashed(raw)) {
        int code = bb_status_exit_code(raw);
        return code < 0 ? 1 : code;
    }

    puts("");
    printf("[opencode-safe] Native TUI crashed (raw status %d). Re-launching in minimal mode...\n", raw);
    puts("[opencode-safe] Your sessions/data are safe; run 'opencode-doctor' to verify.");
    (void)build_child_args(child, bin, true, argc, argv);
    return run_opencode(child);
}
