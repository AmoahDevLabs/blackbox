#define _POSIX_C_SOURCE 200809L

#include "firefox.h"
#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PYTHON_INTEGRITY =
    "import sqlite3,sys\n"
    "try:\n"
    "    c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True)\n"
    "    print(c.execute('PRAGMA integrity_check').fetchone()[0])\n"
    "except Exception as e:\n"
    "    print('ERROR: '+str(e))\n";

static bool is_profile_name(const char *name)
{
    return bb_ends_with(name, ".default") || bb_ends_with(name, ".default-release");
}

static bool find_profile_base(char base[BB_PATH_MAX])
{
    char home[BB_PATH_MAX] = "/";
    if (!bb_home(home)) {
        fputs("firefox-doctor: could not determine HOME; using /\n", stderr);
    }

    char config[BB_PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg == NULL || xdg[0] == '\0') {
        (void)bb_path_join(config, home, ".config");
    } else {
        (void)snprintf(config, sizeof config, "%s", xdg);
    }

    char candidate[BB_PATH_MAX];
    if (bb_path_join(candidate, config, "mozilla/firefox") && bb_is_dir(candidate)) {
        (void)snprintf(base, BB_PATH_MAX, "%s", candidate);
        return true;
    }
    if (bb_path_join(candidate, home, ".mozilla/firefox") && bb_is_dir(candidate)) {
        (void)snprintf(base, BB_PATH_MAX, "%s", candidate);
        return true;
    }
    if (bb_path_join(candidate, home, ".var/app/org.mozilla.firefox/.mozilla/firefox") &&
        bb_is_dir(candidate)) {
        (void)snprintf(base, BB_PATH_MAX, "%s", candidate);
        return true;
    }
    return false;
}

static void check_mount(const char *base)
{
    char out[BB_OUT_MAX];
    int raw = 0;
    char *argv[] = { "findmnt", "-T", (char *)base, "-o", "OPTIONS", "-n", NULL };
    if (!bb_run_capture(argv, out, sizeof out, &raw)) {
        puts("MOUNT:      unknown");
        return;
    }
    bb_first_line(out);
    if (out[0] == '\0') {
        puts("MOUNT:      unknown");
    } else if (bb_contains_word(out, "ro")) {
        puts("MOUNT:      READ-ONLY");
        puts("  Fix: sudo mount -o remount,rw /home");
        puts("  Then re-run: firefox-doctor");
    } else {
        puts("MOUNT:      writable");
    }
}

static void check_write(const char *base)
{
    char test[BB_PATH_MAX];
    if (!bb_path_join(test, base, ".blackbox-write-test")) {
        puts("WRITE TEST: FAILED (path too long)");
        return;
    }
    int fd = open(test, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        puts("WRITE TEST: FAILED");
        printf("  Fix: chmod -R u+rwX \"%s\"\n", base);
        return;
    }
    close(fd);
    (void)unlink(test);
    puts("WRITE TEST: OK");
}

static bool find_firefox_pid(pid_t *pid)
{
    size_t count = 0u;
    if (bb_find_pids("firefox/firefox", pid, 1u, &count) > 0u) {
        return true;
    }
    if (bb_find_pids("org.mozilla.firefox", pid, 1u, &count) > 0u) {
        return true;
    }
    return false;
}

static void check_locks(const char *base, bool running, bool fix)
{
    if (running) {
        return; /* main already reported the running PID */
    }
    puts("PROCESS:    not running");

    DIR *dir = opendir(base);
    if (dir == NULL) {
        return;
    }
    bool stale = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_profile_name(entry->d_name)) {
            continue;
        }
        char profile[BB_PATH_MAX];
        char lock[BB_PATH_MAX];
        char parent[BB_PATH_MAX];
        if (!bb_path_join(profile, base, entry->d_name)) {
            continue;
        }
        (void)bb_path_join(lock, profile, "lock");
        (void)bb_path_join(parent, profile, ".parentlock");
        if (!bb_file_exists(lock) && !bb_file_exists(parent)) {
            continue;
        }
        printf("STALE LOCK: %s\n", profile);
        stale = true;
        if (fix) {
            (void)bb_unlink_if_exists(lock);
            (void)bb_unlink_if_exists(parent);
            puts("  removed");
        }
    }
    closedir(dir);
    if (stale && !fix) {
        puts("  Clear with: firefox-doctor --fix");
    }
}

static void check_permissions(const char *base, bool fix)
{
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return;
    }
    bool bad = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && !bad) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char first[BB_PATH_MAX];
        if (!bb_path_join(first, base, entry->d_name) || !bb_is_dir(first)) {
            continue;
        }
        if (access(first, R_OK | W_OK | X_OK) != 0) {
            printf("PERMISSIONS: not user-writable: %s\n", first);
            bad = true;
            break;
        }
        DIR *sub = opendir(first);
        if (sub == NULL) {
            continue;
        }
        struct dirent *child;
        while ((child = readdir(sub)) != NULL && !bad) {
            if (child->d_name[0] == '.') {
                continue;
            }
            char second[BB_PATH_MAX];
            if (!bb_path_join(second, first, child->d_name) || !bb_is_dir(second)) {
                continue;
            }
            if (access(second, R_OK | W_OK | X_OK) != 0) {
                printf("PERMISSIONS: not user-writable: %s\n", second);
                bad = true;
            }
        }
        closedir(sub);
    }
    closedir(dir);

    if (!bad) {
        puts("PERMISSIONS: OK");
        return;
    }
    if (fix) {
        if (bb_chmod_tree_u_rwx(base)) {
            puts("  permissions repaired");
        } else {
            puts("  WARNING: could not repair all permissions");
        }
    } else {
        puts("  Fix with: firefox-doctor --fix");
    }
}

static void check_databases(const char *base, bool running)
{
    if (running) {
        return;
    }
    static const char *const dbs[] = { "places.sqlite", "cookies.sqlite", "favicons.sqlite" };

    DIR *dir = opendir(base);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_profile_name(entry->d_name)) {
            continue;
        }
        char profile[BB_PATH_MAX];
        if (!bb_path_join(profile, base, entry->d_name)) {
            continue;
        }
        for (size_t i = 0u; i < sizeof dbs / sizeof dbs[0]; i++) {
            char path[BB_PATH_MAX];
            if (!bb_path_join(path, profile, dbs[i]) || !bb_file_exists(path)) {
                continue;
            }
            char out[BB_OUT_MAX];
            int raw = 0;
            char *argv[] = { "python3", "-c", (char *)PYTHON_INTEGRITY, path, NULL };
            if (bb_run_capture(argv, out, sizeof out, &raw)) {
                bb_first_line(out);
                printf("DB: %s -> %s\n", path, out[0] == '\0' ? "no result" : out);
            } else {
                printf("DB: %s -> ERROR: could not run python3\n", path);
            }
        }
    }
    closedir(dir);
}

static void print_recommendations(void)
{
    putchar('\n');
    puts("If Firefox reports 'profile cannot be loaded / read-only':");
    puts("  1. Quit Firefox completely, then: firefox-doctor --fix");
    puts("  2. If mount was READ-ONLY: sudo mount -o remount,rw /home");
    puts("  3. After any power cut: sudo btrfs scrub start /home");
    puts("  4. Last resort - new profile, then copy back places.sqlite (bookmarks/history),");
    puts("     sessionstore.jsonlz4 (session), logins.json + key4.db (passwords).");
}

int cmd_firefox_doctor(bool fix)
{
    puts("== Firefox doctor ==");

    char base[BB_PATH_MAX];
    if (!find_profile_base(base)) {
        fputs("No Firefox profile directory found.\n", stderr);
        return 1;
    }
    printf("Profile base: %s\n", base);

    check_mount(base);
    check_write(base);

    pid_t pid = 0;
    bool running = find_firefox_pid(&pid);
    if (running) {
        printf("PROCESS:    Firefox running (PID %ld) - locks are expected, not stale\n", (long)pid);
    }

    check_locks(base, running, fix);
    check_permissions(base, fix);
    check_databases(base, running);
    print_recommendations();
    return 0;
}
