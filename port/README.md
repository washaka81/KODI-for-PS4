# PS4 port — patches outside the Kodi tree

`src/kodi` is a disposable working tree. The source of truth for the port
is this directory, split into two independent series:

```text
port/                 RELEASE
  base.conf           xbmc/xbmc commit the port applies to
  patches/            release patch series
  overlay/            new files that do not exist upstream
  addons/             add-ons packaged with the port
  python/             files for the embedded Python runtime
  series              release application order
  MANIFEST            release checksums

port/diag/            DEVELOPMENT
  base.conf
  patches/            release series plus the current work-in-progress
  overlay/            FFmpeg and diagnostic helpers used by the WIP
  series
  MANIFEST
```

The goal is to keep Kodi as close to upstream as possible and avoid local
commits inside `src/kodi`. Port changes live here and are applied when
building or packaging.

`port/` is the **release** series: platform and translator patches on the
RC2 base. `port/diag/` is the **development** series used for the current
console work. `0.1.2-alpha1` is built from `port/diag`; promotion of the
validated changes to `port/` is still pending.

`addons/` currently contains `script.ps4.launcher` and
`pvr.iptvsimple`. `scripts/package-port.sh` copies them into the build tree
before packaging; they are not applied to the Kodi source tree.

`python/sitecustomize.py` is copied into the embedded stdlib during
packaging. It applies `TCP_NODELAY`, 1 MiB socket buffers and keepalive to
Python add-on sockets. It does not set hosts, IP addresses, SSIDs or MTU.

## Normal workflow

1. Edit `src/kodi`.
2. Capture and check the changes:

   ```bash
   ./scripts/kodi-port.sh capture
   ./scripts/kodi-port.sh check
   ```

3. Build and package:

   ```bash
   ./scripts/package-port.sh <name>
   ```

`package-port.sh` calls `kodi-port.sh apply`. If the tree already matches
the capture it does nothing; if there are uncaptured changes it stops and
asks for `capture`.

## Commands

```bash
./scripts/kodi-port.sh status
./scripts/kodi-port.sh verify            # src/kodi matches the capture
./scripts/kodi-port.sh capture           # src/kodi -> port/
./scripts/kodi-port.sh check             # test the series in a clean worktree
./scripts/kodi-port.sh check --base REF  # test against another base
./scripts/kodi-port.sh apply [--force]   # rebuild src/kodi from port/
./scripts/kodi-port.sh update [REF]      # test/update the upstream base
```

`apply` does nothing when `src/kodi` already matches the capture (tracked in
`port/.applied`). `apply --force` always rebuilds and creates a backup:

- `build/port-backup-<date>.patch` (tracked changes)
- `build/port-backup-<date>-untracked.tar.gz` (new files)
- branch `port/backup-<date>`

`verify` is the check used by `package-port.sh`: uncaptured changes in
`src/kodi` stop packaging.

## Updating Kodi

```bash
./scripts/kodi-port.sh update origin/master
```

The command fetches the new base, tests the patch series on it and only
updates `base.conf` if everything applies. On conflicts it leaves the test
worktree and the exact command to finish:

```bash
./scripts/kodi-port.sh check --base origin/master --keep
# resolve conflicts in build/kodi-port-check.*
git -C build/kodi-port-check.* am --continue
./scripts/kodi-port.sh finalize build/kodi-port-check.* origin/master
```

`finalize` applies the WIP and overlay, updates `base.conf`, recaptures the
series, rebuilds `src/kodi` on the new base and runs `check`.

Do not use `git merge origin/master` inside `src/kodi` to update the base:
it mixes the port history with upstream and fills the tree with local
commits again.

## Rules

- Do not use `src/kodi` as permanent storage: capture changes.
- Do not edit `port/patches` by hand except to resolve a specific conflict;
  edit `src/kodi` and capture.
- New files go to `overlay/`; modifications to upstream files go to
  `patches/`.
- `check` must pass before packaging.
