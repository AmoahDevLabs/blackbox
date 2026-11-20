# blackbox

Recovery and diagnostic tools for desktop developer apps after power loss or
unexpected crashes.

> **Current branch (`main`):** the tools are implemented as one strict C11
> binary. Build with `make`; `make install` installs `~/.local/bin/blackbox-c`
> and points the command symlinks at it. The original shell scripts are kept on
> the `bash` branch. See [`C_REWRITE.md`](C_REWRITE.md) for the step-by-step guide.

Canonical location: `~/.local/bin/blackbox`

Thin symlinks in `~/.local/bin` keep every command on `PATH`. Until you run
`make install`, they still point at the shell scripts in this directory; after
installation they all point at the single compiled binary:

```
blackbox-c      -> compiled C11 binary
blackboxd       -> blackbox-c
zed-doctor      -> blackbox-c
zed-recover     -> blackbox-c
opencode-doctor -> blackbox-c
opencode-safe   -> blackbox-c
firefox-doctor  -> blackbox-c
```

## Commands

### `blackboxd`
Top-level entrypoint. Runs `zed-recover`, `opencode-doctor`, and
`firefox-doctor`. This replaces the old `recover-all` command.

### `zed-recover`
All-in-one recovery after a power cut:

1. Repairs Zed if its binary is unreadable (kills stuck processes, removes the
   stale `zed-stable.sock`, backs up settings, reinstalls from the official
   installer, verifies the new binary, relaunches Zed).
2. Runs `btrfs scrub` on `/home` and a SMART health check on `/dev/nvme0n1`
   (prompts for `sudo` when run from a terminal).
3. Ensures Zed is running.

### `zed-doctor`
Zed-only repair. Safe to run anytime; if the binary is healthy it exits with a
version report and changes nothing.

### `opencode-doctor`
Read-only health check for OpenCode. Verifies binary readability, SQLite
integrity, lists recent core dumps, shows service state, and reports leftover
Bun/OpenTUI libraries in `/tmp`. Never kills processes or deletes data.

### `opencode-safe`
Drop-in OpenCode launcher. Runs the normal TUI; if it dies with SIGSEGV
(exit 139) or SIGABRT (exit 134), it automatically relaunches in the minimal
interface. Use `opencode-safe --mini` (or the old `--no-native` alias) to start
in minimal mode straight away.

**Note:** `OTUI_NO_NATIVE_RENDER` no longer works in v2 — OpenTUI 0.6 removed
it. The reliable escape hatch is the `mini` interface.

### `firefox-doctor`
Diagnoses Firefox "profile is read-only / cannot be loaded" errors. By default
it is read-only: it checks the mount flags, a real write test, profile
permissions, stale lock files (only when Firefox is fully closed), and the
integrity of `places.sqlite`, `cookies.sqlite`, and `favicons.sqlite`.
`firefox-doctor --fix` is opt-in and only clears stale locks / fixes permissions
when Firefox is not running.

Firefox profiles on this machine live under `~/.config/mozilla/firefox`
(not the legacy `~/.mozilla/firefox` path).

**If Firefox cannot open because of a read-only storage error:**
1. Quit Firefox completely.
2. Run `firefox-doctor` to see the cause.
3. If the mount is read-only: `sudo mount -o remount,rw /home` (then re-run).
4. If locks/permissions are the issue: `firefox-doctor --fix`.
5. After any power cut: `sudo btrfs scrub start /home`.
6. Last resort — create a fresh profile and copy back `places.sqlite`
   (bookmarks/history), `sessionstore.jsonlz4` (open tabs), and `logins.json` +
   `key4.db` (saved passwords).

## If OpenCode segfaults (core dump)

1. Confirm it is not data corruption:
   ```bash
   opencode-doctor
   ```
   The binary hash matches the official v2.3.2 release and `integrity_check`
   reports `ok`; the crash is in the native OpenTUI/Bun FFI renderer thread.
2. Keep working with the minimal interface:
   ```bash
   opencode-safe          # normal TUI, auto-falls back after SIGSEGV
   opencode-safe --mini   # start in mini mode immediately
   opencode mini          # equivalent direct command
   ```
3. If the background service is wedged:
   ```bash
   opencode service restart
   ```
4. If `/tmp` is nearly full, stale native libraries can stop the renderer from
   loading. After quitting **all** OpenCode processes:
   ```bash
   rm -f /tmp/.bun-1000-*.so /tmp/*opentui*.so
   ```
5. v2.3.2 is the latest v2 release, so there is no upgrade that fixes this yet;
   the mini fallback is the current resolution until upstream ships a fix.

## Background

- **Zed** failed with `SIGILL` after a power interruption because btrfs
  corruption made `~/.local/zed.app/libexec/zed-editor` unreadable. Reinstalling
  fixed it. `zed-doctor`/`zed-recover` automate that repair.
- **OpenCode** crashes are `SIGSEGV` in the native TUI renderer (OpenTUI/Bun
  FFI). Its binary and database were verified intact, so this is an upstream
  bug, not filesystem damage. `opencode-safe` degrades gracefully to `mini`
  mode.

## Manual follow-up after any power cut

```bash
sudo btrfs scrub start /home
sudo btrfs scrub status /home
sudo smartctl -H /dev/nvme0n1
```

## Git

GitHub repo: `AmoahDevLabs/blackbox`  
SSH remote (pushes need no credentials):

```bash
git@github.com:AmoahDevLabs/blackbox.git
```

Normal workflow:

```bash
cd ~/.local/bin/blackbox
git add -A && git commit -m "..." && git push
```
