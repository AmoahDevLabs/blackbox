#define _POSIX_C_SOURCE 200809L

#include "opencode.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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

static void check_binary(const char *bin)
{
    if (!bb_read_file_clean(bin) || access(bin, X_OK) != 0) {
        puts("Binary:    CORRUPT/UNREADABLE - reinstall with: opencode upgrade --force");
        return;
    }
    char out[BB_OUT_MAX];
    int raw = 0;
    if (bb_run_capture((char *[]){ (char *)bin, "--version", NULL }, out, sizeof out, &raw)) {
        bb_first_line(out);
        printf("Binary:    OK - %s\n", out);
    } else {
        puts("Binary:    OK - version unavailable");
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
    putchar('\n');
    puts("v2.3.2 is the latest v2 release; no upgrade currently fixes this crash.");
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
