#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool bb_home(char dst[BB_PATH_MAX])
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw == NULL || pw->pw_dir == NULL) {
            return false;
        }
        home = pw->pw_dir;
    }
    int n = snprintf(dst, BB_PATH_MAX, "%s", home);
    return n >= 0 && (size_t)n < BB_PATH_MAX;
}

bool bb_path_join(char dst[BB_PATH_MAX], const char *base, const char *leaf)
{
    if (base == NULL || leaf == NULL) {
        return false;
    }
    size_t blen = strlen(base);
    size_t llen = strlen(leaf);
    bool slash = blen > 0 && base[blen - 1] == '/';
    if (blen + (slash ? 0u : 1u) + llen + 1u > BB_PATH_MAX) {
        return false;
    }
    int n = snprintf(dst, BB_PATH_MAX, "%s%s%s", base, slash ? "" : "/", leaf);
    return n >= 0 && (size_t)n < BB_PATH_MAX;
}

bool bb_ends_with(const char *s, const char *suffix)
{
    if (s == NULL || suffix == NULL) {
        return false;
    }
    size_t slen = strlen(s);
    size_t sufllen = strlen(suffix);
    return sufllen <= slen && strcmp(s + slen - sufllen, suffix) == 0;
}

bool bb_contains_word(const char *list, const char *word)
{
    if (list == NULL || word == NULL || word[0] == '\0') {
        return false;
    }
    size_t wlen = strlen(word);
    const char *pos = list;
    while (pos != NULL && pos[0] != '\0') {
        const char *hit = strstr(pos, word);
        if (hit == NULL) {
            return false;
        }
        size_t idx = (size_t)(hit - list);
        bool before = idx == 0u || hit[-1] == ',' || isspace((unsigned char)hit[-1]);
        char after = hit[wlen];
        bool aft = after == '\0' || after == ',' || isspace((unsigned char)after);
        if (before && aft) {
            return true;
        }
        pos = hit + wlen;
    }
    return false;
}

void bb_trim(char *s)
{
    if (s == NULL) {
        return;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0u && isspace((unsigned char)s[len - 1u])) {
        s[--len] = '\0';
    }
}

void bb_first_line(char *s)
{
    if (s == NULL) {
        return;
    }
    char *nl = strchr(s, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }
    bb_trim(s);
}

void bb_print_tail(const char *buffer, int lines)
{
    if (buffer == NULL || buffer[0] == '\0' || lines <= 0) {
        return;
    }

    const char *starts[256];
    size_t count = 0u;
    starts[count++] = buffer;
    for (const char *p = buffer; *p != '\0' && count < 256u; p++) {
        if (*p == '\n' && *(p + 1u) != '\0') {
            starts[count++] = p + 1u;
        }
    }

    size_t from = count > (size_t)lines ? count - (size_t)lines : 0u;
    for (size_t i = from; i < count; i++) {
        const char *end = strchr(starts[i], '\n');
        if (end == NULL) {
            puts(starts[i]);
        } else {
            fwrite(starts[i], 1u, (size_t)(end - starts[i]), stdout);
            putchar('\n');
        }
    }
}

bool bb_file_exists(const char *path)
{
    return path != NULL && access(path, F_OK) == 0;
}

bool bb_is_dir(const char *path)
{
    if (path == NULL) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool bb_read_file_clean(const char *path)
{
    if (path == NULL) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
    }
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}

bool bb_copy_file(const char *src, const char *dst)
{
    if (src == NULL || dst == NULL) {
        return false;
    }
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return false;
    }
    struct stat st;
    if (fstat(in, &st) != 0) {
        close(in);
        return false;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return false;
    }

    char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        size_t written = 0u;
        while (written < (size_t)n) {
            ssize_t w = write(out, buf + written, (size_t)n - written);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }
            written += (size_t)w;
        }
        if (!ok) {
            break;
        }
    }

    if (close(out) != 0) {
        ok = false;
    }
    if (close(in) != 0) {
        ok = false;
    }
    return ok;
}

bool bb_unlink_if_exists(const char *path)
{
    if (path == NULL) {
        return false;
    }
    if (!bb_file_exists(path)) {
        return true;
    }
    return unlink(path) == 0;
}

bool bb_timestamp(char dst[64])
{
    if (dst == NULL) {
        return false;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return false;
    }
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL) {
        return false;
    }
    size_t n = strftime(dst, 64u, "%Y%m%d-%H%M%S", &tm);
    return n > 0u;
}

static int chmod_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)ftw;
    if (type == FTW_SL || type == FTW_SLN) {
        return 0;
    }
    if (st == NULL) {
        return -1;
    }
    return chmod(path, st->st_mode | S_IRWXU) == 0 ? 0 : -1;
}

bool bb_chmod_tree_u_rwx(const char *root)
{
    if (root == NULL) {
        return false;
    }
    return nftw(root, chmod_cb, 32, FTW_PHYS) == 0;
}

size_t bb_find_pids(const char *needle, pid_t *out, size_t cap, size_t *count)
{
    size_t found = 0u;
    if (needle == NULL || out == NULL || cap == 0u) {
        if (count != NULL) {
            *count = 0u;
        }
        return 0u;
    }

    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        if (count != NULL) {
            *count = 0u;
        }
        return 0u;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && found < cap) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[64];
        int n = snprintf(path, sizeof path, "/proc/%s/cmdline", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof path) {
            continue;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char cmd[8192];
        ssize_t r = read(fd, cmd, sizeof cmd - 1u);
        close(fd);
        if (r <= 0) {
            continue;
        }
        cmd[r] = '\0';
        for (ssize_t i = 0; i < r; i++) {
            if (cmd[i] == '\0') {
                cmd[i] = ' ';
            }
        }
        if (strstr(cmd, needle) != NULL) {
            out[found++] = (pid_t)strtol(entry->d_name, NULL, 10);
        }
    }
    closedir(dir);

    if (count != NULL) {
        *count = found;
    }
    return found;
}

bool bb_process_running(const char *needle)
{
    pid_t pids[1];
    size_t count = 0u;
    return bb_find_pids(needle, pids, 1u, &count) > 0u;
}

void bb_kill_pids(const char *needle)
{
    pid_t pids[256];
    size_t count = 0u;
    bb_find_pids(needle, pids, 256u, &count);
    for (size_t i = 0u; i < count; i++) {
        (void)kill(pids[i], SIGKILL);
    }
}

int bb_status_exit_code(int raw_status)
{
    if (WIFEXITED(raw_status)) {
        return WEXITSTATUS(raw_status);
    }
    return -1;
}

bool bb_status_crashed(int raw_status)
{
    if (WIFSIGNALED(raw_status)) {
        int sig = WTERMSIG(raw_status);
        return sig == SIGSEGV || sig == SIGABRT;
    }
    if (WIFEXITED(raw_status)) {
        int code = WEXITSTATUS(raw_status);
        return code == 139 || code == 134;
    }
    return false;
}

int bb_run_raw(char *const argv[], int *raw_status)
{
    if (argv == NULL || argv[0] == NULL || raw_status == NULL) {
        return -1;
    }
    fflush(NULL); /* avoid parent's buffered text appearing after the child's */
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        int err = errno;
        _exit(err == ENOENT ? 127 : 126);
    }

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid) {
            *raw_status = status;
            return 0;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}

int bb_run_status(char *const argv[])
{
    int raw = 0;
    if (bb_run_raw(argv, &raw) != 0) {
        return -1;
    }
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    if (WIFSIGNALED(raw)) {
        return 128 + WTERMSIG(raw);
    }
    return -1;
}

bool bb_run_capture(char *const argv[], char *out, size_t outsz, int *raw_status)
{
    if (argv == NULL || argv[0] == NULL || out == NULL || outsz < 2u) {
        return false;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }
    fflush(NULL); /* keep output ordering deterministic */

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull != STDERR_FILENO) {
                close(devnull);
            }
        } else {
            (void)close(STDERR_FILENO);
        }
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(pipefd[1]);
    size_t used = 0u;
    char scratch[1024];
    for (;;) {
        size_t room = (used + 1u < outsz) ? outsz - 1u - used : 0u;
        char *dst = room > 0u ? out + used : scratch;
        size_t cap = room > 0u ? room : sizeof scratch;
        ssize_t n = read(pipefd[0], dst, cap);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (room > 0u) {
            used += (size_t)n;
        }
    }
    out[used] = '\0';
    close(pipefd[0]);

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid) {
            break;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    if (raw_status != NULL) {
        *raw_status = status;
    }
    return true;
}

void bb_sleep_seconds(unsigned int seconds)
{
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        /* retry with the remaining time */
    }
}
