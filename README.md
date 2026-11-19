# blackbox

Recovery and diagnostic tools for desktop developer apps after power loss or
unexpected crashes.

Canonical location: `~/.local/bin/blackbox`

Thin symlinks in `~/.local/bin` keep every command on `PATH`:

```
zed-doctor      -> blackbox/zed-doctor
zed-recover     -> blackbox/zed-recover
opencode-doctor -> blackbox/opencode-doctor
opencode-safe   -> blackbox/opencode-safe
blackboxd       -> blackbox/blackboxd
```

## Commands

### `blackboxd`
Top-level entrypoint. Runs `zed-recover` followed by `opencode-doctor`.
This replaces the old `recover-all` command.

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
integrity, lists recent core dumps, and shows service state. Never kills
processes or deletes data.

### `opencode-safe`
Drop-in OpenCode launcher. Runs the normal TUI; if it segfaults (exit 139), it
automatically relaunches in minimal interface mode so you can keep working.
Optional: `opencode-safe --no-native` disables the native renderer up front.

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
