#define _POSIX_C_SOURCE 200809L

#include "firefox.h"
#include "opencode.h"
#include "sha256.h"
#include "util.h"
#include "zed.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *program_basename(const char *path)
{
    if (path == NULL) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static void usage(FILE *out)
{
    fputs("blackbox C rewrite - one binary, many command names\n", out);
    fputs("\n", out);
    fputs("Usage:\n", out);
    fputs("  blackboxd                 run all checks\n", out);
    fputs("  zed-doctor                repair Zed if needed\n", out);
    fputs("  zed-recover               repair Zed + disk checks\n", out);
    fputs("  opencode-doctor           read-only OpenCode health check\n", out);
    fputs("  opencode-safe [--mini]    launch OpenCode with crash fallback\n", out);
    fputs("  firefox-doctor [--fix]    diagnose Firefox read-only profile errors\n", out);
    fputs("  sha256 <file>             print a file's SHA-256 digest\n", out);
    fputs("\n", out);
    fputs("You may also call: blackbox-c <command> [args...]\n", out);
}

static int cmd_blackboxd(void)
{
    puts("===== Zed =====");
    (void)cmd_zed_recover();

    puts("");
    puts("===== OpenCode =====");
    (void)cmd_opencode_doctor();

    puts("");
    puts("===== Firefox =====");
    (void)cmd_firefox_doctor(false);

    puts("");
    puts("===== Done =====");
    puts("Commands available on PATH:");
    puts("  blackboxd        - this all-in-one check");
    puts("  zed-recover      - repair Zed + disk checks");
    puts("  zed-doctor       - Zed-only repair");
    puts("  opencode-doctor  - OpenCode health check");
    puts("  opencode-safe    - OpenCode launcher with crash fallback");
    puts("  firefox-doctor   - Firefox read-only profile diagnosis/repair");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 1) {
        return 2;
    }

    const char *invoked = program_basename(argv[0]);
    const char *name = invoked;

    if (strcmp(name, "blackbox") == 0 || strcmp(name, "blackbox-c") == 0) {
        if (argc == 1) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            puts("blackbox C rewrite 0.1.0");
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        name = argv[1];
        argc--;
        argv++;
    }

    if (strcmp(name, "blackboxd") == 0) {
        return cmd_blackboxd();
    }
    if (strcmp(name, "zed-doctor") == 0) {
        return cmd_zed_doctor();
    }
    if (strcmp(name, "zed-recover") == 0) {
        return cmd_zed_recover();
    }
    if (strcmp(name, "opencode-doctor") == 0) {
        return cmd_opencode_doctor();
    }
    if (strcmp(name, "opencode-safe") == 0) {
        return cmd_opencode_safe(argc - 1, argv + 1);
    }
    if (strcmp(name, "firefox-doctor") == 0) {
        bool fix = argc > 1 && strcmp(argv[1], "--fix") == 0;
        return cmd_firefox_doctor(fix);
    }
    if (strcmp(name, "sha256") == 0) {
        if (argc < 2) {
            fprintf(stderr, "usage: %s sha256 <file>\n", invoked);
            return 2;
        }
        char hex[65];
        if (!bb_sha256_file_hex(argv[1], hex)) {
            fprintf(stderr, "sha256: could not read %s\n", argv[1]);
            return 1;
        }
        printf("%s  %s\n", hex, argv[1]);
        return 0;
    }

    fprintf(stderr, "blackbox: unknown command '%s'\n\n", name);
    usage(stderr);
    return 2;
}
