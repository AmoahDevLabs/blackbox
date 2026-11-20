#ifndef BLACKBOX_UTIL_H
#define BLACKBOX_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define BB_PATH_MAX 4096
#define BB_CMD_MAX 64
#define BB_OUT_MAX 65536

/* Paths and strings. */
bool bb_home(char dst[BB_PATH_MAX]);
bool bb_path_join(char dst[BB_PATH_MAX], const char *base, const char *leaf);
bool bb_ends_with(const char *s, const char *suffix);
bool bb_contains_word(const char *list, const char *word);
void bb_trim(char *s);
void bb_first_line(char *s);
void bb_print_tail(const char *buffer, int lines);

/* Files. */
bool bb_file_exists(const char *path);
bool bb_is_dir(const char *path);
bool bb_read_file_clean(const char *path);
bool bb_copy_file(const char *src, const char *dst);
bool bb_unlink_if_exists(const char *path);
bool bb_timestamp(char dst[64]);
bool bb_chmod_tree_u_rwx(const char *root);

/* Processes. */
size_t bb_find_pids(const char *needle, pid_t *out, size_t cap, size_t *count);
bool bb_process_running(const char *needle);
void bb_kill_pids(const char *needle);
int bb_status_exit_code(int raw_status);
bool bb_status_crashed(int raw_status);

/* Child commands: fork + execvp, never system(). */
int bb_run_raw(char *const argv[], int *raw_status);
int bb_run_status(char *const argv[]);
bool bb_run_capture(char *const argv[], char *out, size_t outsz, int *raw_status);

/* Misc. */
void bb_sleep_seconds(unsigned int seconds);

#endif /* BLACKBOX_UTIL_H */
