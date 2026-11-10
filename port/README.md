# PS4 port — patches outside the Kodi tree

`src/kodi` is a **regenerable working tree**. The port itself lives in this
directory:

```text
port/
  base.conf     upstream commit the port is applied on
  patches/      ordered patch series (plain diffs)
  series        patch order
  overlay/      optional new files not covered by the patches
```

The goal is to keep Kodi as close to upstream as possible. The port changes
are applied when building or packaging, not stored as local commits inside
the Kodi submodule.

## Getting the source

Clone Kodi at the commit in `base.conf`:

```bash
git clone https://github.com/xbmc/xbmc.git src/kodi
git -C src/kodi checkout 85cd79a00cfaff6212cf5265c91f92fddb9846a5
```

Then apply the port:

```bash
./scripts/kodi-port.sh apply
./scripts/kodi-port.sh verify
```

## Daily workflow

1. Edit `src/kodi` as usual.
2. Capture the changes into `port/`:

   ```bash
   ./scripts/kodi-port.sh capture
   ./scripts/kodi-port.sh check
   ```

3. Build or package:

   ```bash
   ./scripts/package-port.sh <name>
   ```

`package-port.sh` applies the port if needed, builds the shim and Kodi, and
creates the package. It refuses to run when `src/kodi` has changes that were
not captured.

## Commands

```bash
./scripts/kodi-port.sh status
./scripts/kodi-port.sh verify            # src/kodi matches the capture
./scripts/kodi-port.sh capture           # export src/kodi -> port/
./scripts/kodi-port.sh check             # apply the series in a clean worktree
./scripts/kodi-port.sh check --base REF  # test against another base
./scripts/kodi-port.sh apply [--force]   # regenerate src/kodi from port/
./scripts/kodi-port.sh update [REF]      # test/update the upstream base
./scripts/kodi-port.sh finalize <worktree> <base>
```

`apply` does nothing when `src/kodi` already matches the capture (tracked in
`port/.applied`). `apply --force` always regenerates and backs up first:

- `build/port-backup-<date>.patch` (tracked changes)
- `build/port-backup-<date>-untracked.tar.gz` (new files)
- `port/backup-<date>` branch

## Updating Kodi

```bash
./scripts/kodi-port.sh update origin/master
```

The command fetches upstream, tests the patch series on the new base, and
updates `base.conf` only when every patch applies. On conflicts it leaves a
worktree for resolution and prints the exact `finalize` command:

```bash
# resolve conflicts in build/kodi-port-check.*
git -C build/kodi-port-check.* am --continue
./scripts/kodi-port.sh finalize build/kodi-port-check.* origin/master
```

`finalize` applies the work-in-progress and overlay, updates the base,
re-captures the series, regenerates `src/kodi`, and runs `check`.

Do not use `git merge origin/master` inside `src/kodi` as the update
mechanism: it mixes upstream history with local port commits and defeats the
point of keeping the port outside the tree.

## Rules

- Do not use `src/kodi` as permanent storage. Capture changes before
  switching trees or packaging.
- Do not edit `port/patches` by hand except to resolve a specific conflict;
  edit `src/kodi` and re-capture.
- Keep the Kodi tree at the base commit in `base.conf`; run `check` before
  packaging.
