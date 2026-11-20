#define _POSIX_C_SOURCE 200809L

#include "zed.h"
#include "sha256.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void build_paths(char editor[BB_PATH_MAX], char launcher[BB_PATH_MAX],
                        char sock[BB_PATH_MAX], char settings[BB_PATH_MAX])
{
    char home[BB_PATH_MAX] = "/";
    if (!bb_home(home)) {
        fputs("zed: could not determine HOME; using /\n", stderr);
    }
    (void)bb_path_join(editor, home, ".local/zed.app/libexec/zed-editor");
    (void)bb_path_join(launcher, home, ".local/bin/zed");
    (void)bb_path_join(sock, home, ".local/share/zed/zed-stable.sock");
    (void)bb_path_join(settings, home, ".config/zed/settings.json");
}

static int run_version(const char *launcher)
{
    char *argv[] = { (char *)launcher, "--version", NULL };
    return bb_run_status(argv);
}

static int run_installer(char *installer)
{
    char *curl[] = { "curl", "-fsSL", "https://zed.dev/install.sh", "-o", installer, NULL };
    int rc = bb_run_status(curl);
    if (rc != 0) {
        return rc;
    }
    char *sh[] = { "sh", installer, NULL };
    return bb_run_status(sh);
}

static void backup_settings(const char *settings)
{
    if (!bb_file_exists(settings)) {
        return;
    }
    char stamp[64];
    if (!bb_timestamp(stamp)) {
        (void)snprintf(stamp, sizeof stamp, "unknown");
    }
    char backup[BB_PATH_MAX];
    if (snprintf(backup, sizeof backup, "%s.bak.%s", settings, stamp) >= (int)sizeof backup) {
        fprintf(stderr, "zed-doctor: backup path too long; skipping backup\n");
        return;
    }
    if (bb_copy_file(settings, backup)) {
        printf("Settings backed up to: %s\n", backup);
    } else {
        fprintf(stderr, "zed-doctor: WARNING: could not back up settings\n");
    }
}

int cmd_zed_doctor(void)
{
    char editor[BB_PATH_MAX];
    char launcher[BB_PATH_MAX];
    char sock[BB_PATH_MAX];
    char settings[BB_PATH_MAX];
    build_paths(editor, launcher, sock, settings);

    puts("== Zed doctor ==");

    char digest[65];
    if (bb_sha256_file_hex(editor, digest)) {
        puts("Zed binary reads cleanly - no repair needed.");
        printf("SHA256: %s\n", digest);
        (void)run_version(launcher);
        return 0;
    }

    puts("Corruption detected (or Zed is missing). Starting repair...");

    pid_t pids[256];
    size_t count = 0u;
    bb_find_pids("zed.app", pids, 256u, &count);
    if (count > 0u) {
        printf("Killing stuck Zed processes:");
        for (size_t i = 0u; i < count; i++) {
            printf(" %ld", (long)pids[i]);
            (void)kill(pids[i], SIGKILL);
        }
        putchar('\n');
    }

    if (!bb_unlink_if_exists(sock)) {
        fprintf(stderr, "zed-doctor: WARNING: could not remove stale socket\n");
    }

    backup_settings(settings);

    puts("Downloading and reinstalling Zed...");
    char installer[] = "/tmp/zed-install.XXXXXX";
    int fd = mkstemp(installer);
    if (fd < 0) {
        fprintf(stderr, "zed-doctor: could not create temp installer: %s\n", strerror(errno));
        return 1;
    }
    close(fd);

    int rc = run_installer(installer);
    (void)bb_unlink_if_exists(installer);
    if (rc != 0) {
        fprintf(stderr, "zed-doctor: installer failed (exit %d)\n", rc);
        return 1;
    }

    if (!bb_sha256_file_hex(editor, digest)) {
        fprintf(stderr, "ERROR: the newly installed binary is still unreadable.\n");
        fprintf(stderr, "Run: sudo btrfs scrub start /home ; sudo smartctl -a /dev/nvme0n1\n");
        return 1;
    }
    printf("SHA256: %s\n", digest);

    rc = run_version(launcher);
    if (rc != 0) {
        return rc;
    }

    puts("Repair complete. Launching Zed...");
    char *launch[] = { launcher, NULL };
    return bb_run_status(launch);
}

static void run_root_checks(void)
{
    puts("[zed-recover] Running btrfs scrub on /home and SMART health on /dev/nvme0n1 ...");

    char *scrub_start[] = { "sudo", "/usr/bin/btrfs", "scrub", "start", "/home", NULL };
    if (bb_run_status(scrub_start) != 0) {
        puts("[zed-recover] WARN: could not start scrub");
    }
    bb_sleep_seconds(2u);

    char *scrub_status[] = { "sudo", "/usr/bin/btrfs", "scrub", "status", "/home", NULL };
    (void)bb_run_status(scrub_status);

    char *smart[] = { "sudo", "/usr/bin/smartctl", "-H", "/dev/nvme0n1", NULL };
    if (bb_run_status(smart) != 0) {
        puts("[zed-recover] WARN: SMART health check failed");
    }
}

static void run_disk_checks(void)
{
    char *sudo_check[] = { "sudo", "-n", "true", NULL };
    int rc = bb_run_status(sudo_check);

    if (rc == 127) {
        puts("[zed-recover] WARN: sudo not found; skipping disk checks.");
        return;
    }
    if (rc == 0) {
        run_root_checks();
        return;
    }
    if (isatty(STDIN_FILENO)) {
        puts("[zed-recover] Root privileges needed for disk checks - sudo will prompt.");
        char *sudo_v[] = { "sudo", "-v", NULL };
        if (bb_run_status(sudo_v) == 0) {
            run_root_checks();
        } else {
            puts("[zed-recover] WARN: sudo authentication failed; skipping disk checks.");
        }
        return;
    }

    puts("[zed-recover] Non-interactive session: cannot prompt for sudo.");
}

static void print_manual_disk_commands(void)
{
    puts("[zed-recover] If disk checks were skipped, run these manually in a terminal:");
    puts("  sudo btrfs scrub start /home");
    puts("  sudo btrfs scrub status /home");
    puts("  sudo smartctl -H /dev/nvme0n1");
}

static void ensure_zed_running(const char *sock, const char *launcher)
{
    puts("[zed-recover] Step 3/3: ensuring Zed is running...");
    if (bb_process_running("zed.app")) {
        puts("[zed-recover] Zed is already running.");
        return;
    }

    (void)bb_unlink_if_exists(sock);
    char *launch[] = { (char *)launcher, NULL };
    (void)bb_run_status(launch);
    bb_sleep_seconds(3u);

    if (bb_process_running("zed.app")) {
        puts("[zed-recover] Zed launched successfully.");
    } else {
        fprintf(stderr,
                "[zed-recover] WARN: Zed did not appear to start; "
                "check ~/.local/share/zed/logs/Zed.log\n");
    }
}

int cmd_zed_recover(void)
{
    char editor[BB_PATH_MAX];
    char launcher[BB_PATH_MAX];
    char sock[BB_PATH_MAX];
    char settings[BB_PATH_MAX];
    build_paths(editor, launcher, sock, settings);
    (void)editor;
    (void)settings;

    puts("[zed-recover] Step 1/3: checking/repairing Zed...");
    if (cmd_zed_doctor() != 0) {
        fprintf(stderr, "[zed-recover] ERROR: Zed repair failed. Aborting.\n");
        return 1;
    }

    puts("[zed-recover] Step 2/3: filesystem and disk checks");
    run_disk_checks();
    print_manual_disk_commands();

    ensure_zed_running(sock, launcher);
    puts("[zed-recover] Done.");
    return 0;
}
