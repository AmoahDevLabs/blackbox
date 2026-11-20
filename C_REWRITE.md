# Rewriting Blackbox in C — step by step

This branch (`c-rewrite`) ports the shell recovery tools to a single C11 program.
The goal is not just "same behaviour", but code you can read and reason about:

- **one binary, many names** (the busybox pattern),
- **no shell injection** — `fork()` + `execvp()`, never `system()`,
- **strict compilation** — every warning is an error,
- **read-only by default** — repairs are explicit opt-ins,
- **bounded buffers** — no unbounded `strcpy`/`sprintf`.

---

## Step 0 — Create the branch

```bash
cd ~/.local/bin/blackbox
git switch main
git pull
git switch -c c-rewrite
```

Everything below lives on `c-rewrite`. The shell scripts stay on `main` until we
are ready to switch over.

---

## Step 1 — Build skeleton and command dispatch

Files added:

```
Makefile
src/blackbox.c
src/util.h src/util.c
src/zed.h   src/zed.c
src/opencode.h src/opencode.c
src/firefox.h  src/firefox.c
```

`src/blackbox.c` is tiny. It looks at `argv[0]` and picks a command:

```c
const char *name = program_basename(argv[0]);
if (strcmp(name, "opencode-safe") == 0)
    return cmd_opencode_safe(argc - 1, argv + 1);
```

Why this design?

- One compiled binary is faster and easier to audit than six shell scripts.
- Symlinks like `~/.local/bin/opencode-safe -> blackbox-c` select the behaviour.
- The binary can also be called explicitly: `blackbox-c opencode-doctor`.

The `Makefile` compiles with:

```make
-std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
```

`-Werror` is deliberate: a warning is a bug we fix now, not later.

---

## Step 2 — Safe primitives in `src/util.c`

This is the foundation. Every command uses these helpers.

| Helper | Purpose | Why it matters |
|---|---|---|
| `bb_home()` | resolve `HOME`, fall back to `getpwuid()` | never uses an uninitialised path |
| `bb_path_join()` | build paths with length checks | prevents overflow before `snprintf` |
| `bb_read_file_clean()` | read a file fully to EOF | catches btrfs `EIO`, not just `access()` |
| `bb_run_raw()` | `fork` + `execvp` + `waitpid` | no shell, no injection, real wait status |
| `bb_run_capture()` | capture stdout of a child | used for `--version`, SQLite checks, `findmnt` |
| `bb_find_pids()` | scan `/proc/<pid>/cmdline` | replaces `pgrep` with no extra process |
| `bb_copy_file()` | backup settings byte-for-byte | handles short reads/writes and `EINTR` |
| `bb_timestamp()` | `localtime_r` + `strftime` | thread-safe, no `ctime` static buffer |
| `bb_chmod_tree_u_rwx()` | `nftw(..., FTW_PHYS)` | never follows symlinks during repair |

One subtle but important line in both child-launching helpers:

```c
fflush(NULL); /* keep output ordering deterministic */
```

Without it, the parent's buffered `printf()` can appear **after** the child's
output. We saw exactly that while testing `zed-doctor`.

Signal handling is also explicit:

```c
bool bb_status_crashed(int raw_status)
{
    if (WIFSIGNALED(raw_status))
        return WTERMSIG(raw_status) == SIGSEGV || WTERMSIG(raw_status) == SIGABRT;
    ...
}
```

Bash reports a SIGSEGV as exit `139`; C gets the real `WIFSIGNALED` status. We
accept both forms so the OpenCode fallback cannot miss a crash.

---

## Step 3 — `zed-doctor` in `src/zed.c`

The shell version did: hash check → kill stuck processes → remove socket →
backup settings → reinstall → verify → launch. The C version does the same,
but with explicit checks at each boundary.

1. **Health check**
   ```c
   if (bb_read_file_clean(editor)) {
       puts("Zed binary reads cleanly - no repair needed.");
       (void)run_version(launcher);
       return 0;
   }
   ```
   Reading the whole file is what detects the power-loss corruption we saw.

2. **Kill only matching processes**
   ```c
   bb_find_pids("zed.app", pids, 256u, &count);
   ...
   (void)kill(pids[i], SIGKILL);
   ```
   Fixed array of 256, with a count — no dynamic allocation needed.

3. **Safe temp installer**
   ```c
   char installer[] = "/tmp/zed-install.XXXXXX";
   int fd = mkstemp(installer);
   ```
   `mkstemp` creates the file atomically with mode 0600. We close the fd, then
   let `curl` write to that exact path.

4. **Verify before launch**
   If the fresh binary is still unreadable, the tool prints the btrfs/SMART
   follow-up commands and returns `1`. It never claims success on a bad binary.

---

## Step 4 — `zed-recover` in `src/zed.c`

This wraps `zed-doctor`, then handles disk checks.

```c
char *sudo_check[] = { "sudo", "-n", "true", NULL };
int rc = bb_run_status(sudo_check);
```

Three cases:

- `rc == 0` — passwordless/cached sudo, run the checks,
- `rc == 127` — `sudo` missing, say so,
- otherwise — if `isatty(STDIN_FILENO)`, run `sudo -v` and let it prompt;
  if not interactive, print the manual commands instead.

No passwordless-sudo assumption is baked in.

---

## Step 5 — OpenCode in `src/opencode.c`

### `opencode-doctor`

- binary: `bb_read_file_clean()` + `opencode --version` captured through a pipe,
- database: `python3 -c ...` with a read-only SQLite URI (`file:...?mode=ro`),
- core dumps: `coredumpctl` output captured and reduced to the last 5 lines,
- service: `/proc` scan for `opencode serve --service`,
- `/tmp`: count leftover `.bun-1000-*.so` / `*opentui*.so` files and report
  free space via `statvfs()`.

It is strictly read-only. It does not kill or delete anything.

### `opencode-safe`

The crash fallback now understands real C wait statuses:

```c
if (!bb_status_crashed(raw)) return code;
...
build_child_args(child, bin, true, argc, argv);  /* prepend "mini" */
```

`--mini` and the old `--no-native` alias both start `opencode mini` directly,
because upstream removed `OTUI_NO_NATIVE_RENDER` in OpenTUI 0.6.

---

## Step 6 — `firefox-doctor` in `src/firefox.c`

Read-only by default; `--fix` is opt-in.

- profile base: XDG path, legacy `~/.mozilla/firefox`, then flatpak path,
- mount: `findmnt -T ... -o OPTIONS -n`, then `bb_contains_word(opts, "ro")`,
- write test: `open(O_CREAT | O_EXCL)` + `unlink`,
- process detection: search `/proc` for `firefox/firefox` (not merely
  `firefox`, which would match our own `firefox-doctor` process),
- locks: only touched when Firefox is not running,
- SQLite: same read-only `PRAGMA integrity_check` helper,
- permissions repair uses `nftw` with `FTW_PHYS` so symlinks are never followed.

---

## Step 7 — Top-level `blackboxd`

`cmd_blackboxd()` calls the other command functions directly:

```c
(void)cmd_zed_recover();
(void)cmd_opencode_doctor();
(void)cmd_firefox_doctor(false);
```

Each sub-command is independent, so one failure does not abort the rest — the
same behaviour as the shell runner, but without spawning three interpreters.

---

## Step 8 — Build, test, install

```bash
cd ~/.local/bin/blackbox
make clean
make
make check
```

`make check` verifies the binary builds, prints its version/help, and rejects
an unknown command with a non-zero exit status.

Manual smoke tests:

```bash
./build/blackbox --version
./build/blackbox opencode-doctor
./build/blackbox firefox-doctor
./build/blackbox blackboxd
```

To replace the shell-script symlinks on `PATH`:

```bash
make install
```

This installs `~/.local/bin/blackbox-c` and points `blackboxd`, `zed-doctor`,
`zed-recover`, `opencode-doctor`, `opencode-safe`, and `firefox-doctor` at it.

---

## Rules that keep this error-free

1. Compile with `-Werror`; fix warnings immediately.
2. Never use `system()`; use `execvp()` with an explicit argv array.
3. Flush stdout before `fork()`.
4. Check every return value that can fail: `open`, `read`, `write`, `close`,
   `waitpid`, `snprintf`, `chmod`.
5. Retry `read`/`write`/`waitpid` on `EINTR`.
6. Bound every array; pass capacity explicitly.
7. Treat repair operations as opt-in; default to read-only diagnosis.
8. Test through the symlink name, because `argv[0]` dispatch is part of the API.

---

## Current status

Implemented on `c-rewrite`:

- one C11 binary with six command identities,
- Zed doctor + recover,
- OpenCode doctor + safe launcher,
- Firefox doctor (with opt-in `--fix`),
- strict build and basic checks.

Not yet implemented (deliberately):

- byte-for-byte comparison of the Zed binary hash against a published release
  hash — the current check reads the whole file to detect I/O errors, matching
  the shell tool,
- automatic sudo password handling beyond `sudo -v` in a real terminal.
