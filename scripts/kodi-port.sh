#!/usr/bin/env bash
#
# kodi-port.sh — Keep the PS4 port as patches outside the Kodi tree.
#
# The source of truth is port/:
#   base.conf   upstream commit the port is applied on
#   patches/    patch series (git format-patch + WIP)
#   overlay/    new files that do not exist upstream
#
# src/kodi is a regenerable working tree: edit it, capture it with
# `capture`, and rebuild it with `apply` before building/packaging.
#
# Usage:
#   ./scripts/kodi-port.sh status
#   ./scripts/kodi-port.sh verify
#   ./scripts/kodi-port.sh capture
#   ./scripts/kodi-port.sh check [--base REF] [--keep]
#   ./scripts/kodi-port.sh apply [--force]
#   ./scripts/kodi-port.sh update [REF]
#   ./scripts/kodi-port.sh finalize <worktree> <base>
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KODI_DIR="${KODI_DIR:-$REPO_ROOT/src/kodi}"
PORT_DIR="${PORT_DIR:-$REPO_ROOT/port}"
BASE_CONF="$PORT_DIR/base.conf"
PATCH_DIR="$PORT_DIR/patches"
OVERLAY_DIR="$PORT_DIR/overlay"
SERIES="$PORT_DIR/series"

log() { printf '\033[1;36m[port]\033[0m %s\n' "$*"; }
ok() { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die() {
  printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2
  exit 1
}

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

git_kodi() { git -C "$KODI_DIR" "$@"; }

load_base() {
  [[ -f "$BASE_CONF" ]] || die "falta $BASE_CONF"
  # shellcheck disable=SC1090
  source "$BASE_CONF"
  [[ -n "${KODI_BASE:-}" ]] || die "KODI_BASE is empty in $BASE_CONF"
  BASE_COMMIT="$(git_kodi rev-parse --verify "${1:-$KODI_BASE}^{commit}")" ||
    die "no encuentro la base ${1:-$KODI_BASE} en el repo"
}

write_base_conf() {
  local new_base="$1"
  cat >"$BASE_CONF" <<EOF
# Port upstream base (the source of truth is port/patches and port/overlay)
KODI_UPSTREAM_URL=${KODI_UPSTREAM_URL:-https://github.com/xbmc/xbmc.git}
KODI_UPSTREAM_BRANCH=${KODI_UPSTREAM_BRANCH:-master}
KODI_BASE=$new_base
KODI_BASE_DATE=$(date -I)
EOF
}

series_list() {
  [[ -f "$SERIES" ]] || die "missing $SERIES; run capture"
  grep -v '^\s*$' "$SERIES"
}

series_signature() {
  sha256sum "$SERIES" | cut -d' ' -f1
}

tree_signature() {
  {
    git_kodi rev-parse HEAD
    git_kodi status --porcelain | LC_ALL=C sort
  } | sha256sum | cut -d' ' -f1
}

write_state() {
  printf 'PORT_BASE=%s\nPORT_SERIES=%s\nPORT_TREE=%s\nPORT_APPLIED=%s\n' \
    "$KODI_BASE" \
    "$(series_signature)" \
    "$(tree_signature)" \
    "$(date -Iseconds)" >"$PORT_DIR/.applied"
}

check_state() {
  local mismatches=0
  [[ -f "$PORT_DIR/.applied" ]] || return 1
  # shellcheck disable=SC1090
  source "$PORT_DIR/.applied"
  [[ "${PORT_BASE:-}" == "$KODI_BASE" ]] || mismatches=1
  [[ "${PORT_SERIES:-}" == "$(series_signature)" ]] || mismatches=1
  [[ "${PORT_TREE:-}" == "$(tree_signature)" ]] || mismatches=1
  return "$mismatches"
}

copy_overlay() {
  local dest="$1"
  [[ -d "$OVERLAY_DIR" ]] || return 0
  cp -a "$OVERLAY_DIR/." "$dest/"
}

apply_series() {
  local dest="$1"
  local patch
  while IFS= read -r patch; do
    [[ -n "$patch" ]] || continue
    if head -n 1 "$PATCH_DIR/$patch" | grep -q '^From '; then
      if ! git -C "$dest" am --3way --quiet "$PATCH_DIR/$patch"; then
        return 1
      fi
    else
      if ! git -C "$dest" apply --index --binary "$PATCH_DIR/$patch"; then
        return 1
      fi
    fi
  done < <(series_list)
  copy_overlay "$dest"
}

cmd_status() {
  load_base
  local head dirty
  head="$(git_kodi rev-parse --short HEAD)"
  dirty="$(git_kodi status --porcelain | wc -l | tr -d ' ')"
  printf 'base:      %s (%s)\n' "${KODI_BASE:0:12}" "${KODI_BASE_DATE:-?}"
  printf 'upstream:  %s/%s\n' "${KODI_UPSTREAM_URL:-?}" "${KODI_UPSTREAM_BRANCH:-master}"
  printf 'parches:   %s\n' "$(series_list | wc -l | tr -d ' ')"
  printf 'overlay:   %s files\n' "$(find "$OVERLAY_DIR" -type f 2>/dev/null | wc -l | tr -d ' ')"
  printf 'src/kodi:  HEAD %s, %s uncommitted changes\n' "$head" "$dirty"
  if [[ -f "$PORT_DIR/.applied" ]]; then
    # shellcheck disable=SC1090
    source "$PORT_DIR/.applied"
    printf 'aplicado:  %s (base %s)\n' "${PORT_APPLIED:-?}" "${PORT_BASE:0:12}"
  fi
}

cmd_capture() {
  load_base
  git_kodi merge-base --is-ancestor "$BASE_COMMIT" HEAD ||
    die "HEAD no desciende de la base $KODI_BASE"

  rm -rf "$PATCH_DIR" "$OVERLAY_DIR"
  rm -f "$PORT_DIR/.applied"
  mkdir -p "$PATCH_DIR" "$OVERLAY_DIR"

  git_kodi format-patch --no-signature --zero-commit \
    --output-directory "$PATCH_DIR" "$BASE_COMMIT..HEAD" >/dev/null

  local n wip
  n="$(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' | wc -l | tr -d ' ')"
  wip="$(printf '%04d-work-in-progress.patch' $((n + 1)))"
  git_kodi diff --binary HEAD >"$PATCH_DIR/$wip"
  if [[ ! -s "$PATCH_DIR/$wip" ]]; then
    rm -f "$PATCH_DIR/$wip"
  fi

  while IFS= read -r -d '' f; do
    case "$f" in
      *.rej|*.orig) continue ;;
    esac
    mkdir -p "$OVERLAY_DIR/$(dirname "$f")"
    cp -a "$KODI_DIR/$f" "$OVERLAY_DIR/$f"
  done < <(git_kodi ls-files --others --exclude-standard -z)

  (cd "$PATCH_DIR" && ls -1 *.patch | LC_ALL=C sort) >"$SERIES"

  {
    printf 'base %s\n' "$KODI_BASE"
    printf 'commits %s\n' "$(git_kodi rev-list --count "$BASE_COMMIT..HEAD")"
    printf 'patches %s\n' "$(wc -l <"$SERIES" | tr -d ' ')"
    printf 'overlay %s\n' "$(find "$OVERLAY_DIR" -type f | wc -l | tr -d ' ')"
    printf 'sha256\n'
    (cd "$PORT_DIR" && sha256sum $(cat "$SERIES" | sed 's|^|patches/|') 2>/dev/null || true)
    (cd "$OVERLAY_DIR" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum || true)
  } >"$PORT_DIR/MANIFEST"

  if [[ "${KODI_PORT_NO_STATE:-0}" != "1" ]]; then
    write_state
  fi
  ok "captured: $(wc -l <"$SERIES" | tr -d ' ') patches, $(find "$OVERLAY_DIR" -type f | wc -l | tr -d ' ') overlay files"
}

run_check() {
  local base="$1"
  local keep="$2"
  local tmp
  tmp="$(mktemp -d "$REPO_ROOT/build/kodi-port-check.XXXXXX")"
  CHECK_WORKTREE="$tmp"

  git_kodi worktree add --detach "$tmp" "$base" >/dev/null 2>&1 ||
    die "could not create the check worktree"

  if ! apply_series "$tmp"; then
    warn "patches do NOT apply on $base"
    warn "worktree kept for inspection: $tmp"
    return 1
  fi

  local dirty
  dirty="$(git -C "$tmp" status --porcelain | wc -l | tr -d ' ')"
  ok "patches apply on ${base:0:12} ($dirty modified files)"
  if (( !keep )); then
    git_kodi worktree remove --force "$tmp" >/dev/null 2>&1 || rm -rf "$tmp"
    CHECK_WORKTREE=""
  else
    log "worktree conservado: $tmp"
  fi
  return 0
}

cmd_check() {
  local base="" keep=0
  while (($#)); do
    case "$1" in
      --base)
        base="${2:?--base necesita un ref}"
        shift
        ;;
      --keep) keep=1 ;;
      *) die "unknown option: $1" ;;
    esac
    shift
  done
  load_base "$base"
  run_check "$BASE_COMMIT" "$keep"
}

cmd_apply() {
  local force=0
  while (($#)); do
    case "$1" in
      --force) force=1 ;;
      *) die "unknown option: $1" ;;
    esac
    shift
  done
  load_base

  if (( !force )) && check_state; then
    ok "port already applied and unchanged since ${PORT_APPLIED:-?}"
    return 0
  fi

  if [[ -n "$(git_kodi status --porcelain)" ]]; then
    ((force)) || die "src/kodi tiene cambios no capturados; usa capture y luego apply --force"
    local stamp branch patch_backup tar_backup
    stamp="$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$REPO_ROOT/build"
    patch_backup="$REPO_ROOT/build/port-backup-$stamp.patch"
    tar_backup="$REPO_ROOT/build/port-backup-$stamp-untracked.tar.gz"
    branch="port/backup-$stamp"

    git_kodi diff --binary HEAD >"$patch_backup" || true
    if git_kodi ls-files --others --exclude-standard -z | tar --null -czf "$tar_backup" -T - 2>/dev/null; then :; else
      rm -f "$tar_backup"
    fi
    git_kodi branch -f "$branch" HEAD >/dev/null
    warn "backup: $patch_backup"
    warn "backup: $tar_backup"
    warn "rama:   $branch"
  fi

  log "regenerating src/kodi from ${KODI_BASE:0:12}"
  git_kodi reset --hard "$BASE_COMMIT" >/dev/null
  git_kodi clean -fd >/dev/null

  apply_series "$KODI_DIR"

  write_state

  ok "port applied: $(git_kodi status --porcelain | wc -l | tr -d ' ') files"
}

cmd_verify() {
  load_base
  if check_state; then
    ok "src/kodi matches port/ (base ${KODI_BASE:0:12})"
  else
    die "src/kodi changed since the last capture; run ./scripts/kodi-port.sh capture"
  fi
}

cmd_update() {
  local ref="${1:-origin/master}"
  load_base
  git_kodi fetch --no-tags origin master >/dev/null 2>&1 ||
    warn "fetch failed; using the local reference"
  local new
  new="$(git_kodi rev-parse --verify "$ref^{commit}")" ||
    die "no encuentro $ref"
  [[ "$new" != "$BASE_COMMIT" ]] || {
    ok "ya estamos en ${new:0:12}"
    return 0
  }

  log "testing patches on ${new:0:12}"
  if ! run_check "$new" 0; then
    die "base not updated; resolve in ${CHECK_WORKTREE:-the worktree} and run: $0 finalize ${CHECK_WORKTREE:-<worktree>} $new"
  fi

  write_base_conf "$new"
  ok "base updated to ${new:0:12}; run apply --force"
}

cmd_finalize() {
  local dir="${1:-}" base="${2:-}"
  [[ -n "$dir" && -n "$base" ]] || die "usage: $0 finalize <worktree> <base>"
  [[ -d "$dir" ]] || die "no existe $dir"
  git -C "$dir" rev-parse --git-dir >/dev/null 2>&1 || die "$dir no es un repo git"
  base="$(git -C "$dir" rev-parse --verify "$base^{commit}")" || die "invalid base"
  git -C "$dir" merge-base --is-ancestor "$base" HEAD ||
    die "la base no es ancestro de HEAD"
  [[ ! -e "$(git -C "$dir" rev-parse --git-path rebase-apply)" &&
     ! -e "$(git -C "$dir" rev-parse --git-path rebase-merge)" ]] ||
    die "finish the am/rebase before finalizing (git am --continue)"

  local wip
  wip="$(series_list | grep 'work-in-progress.patch' || true)"
  if [[ -n "$wip" ]]; then
    if git -C "$dir" apply --reverse --check "$PATCH_DIR/$wip" 2>/dev/null; then
      log "WIP ya aplicado en el worktree"
    elif git -C "$dir" apply --check "$PATCH_DIR/$wip" 2>/dev/null; then
      git -C "$dir" apply --index --binary "$PATCH_DIR/$wip"
    else
      die "WIP does not apply; resolve it in $dir and run finalize again"
    fi
  fi
  copy_overlay "$dir"

  write_base_conf "$base"
  KODI_PORT_NO_STATE=1 KODI_DIR="$dir" "$0" capture
  rm -f "$PORT_DIR/.applied"

  log "regenerating src/kodi on the new base"
  cmd_apply --force
  cmd_check
  ok "update finished at ${base:0:12}"
}

case "${1:-status}" in
  status) cmd_status ;;
  capture) shift; cmd_capture ;;
  check) shift; cmd_check "$@" ;;
  apply) shift; cmd_apply "$@" ;;
  update) shift; cmd_update "$@" ;;
  finalize) shift; cmd_finalize "$@" ;;
  verify) cmd_verify ;;
  -h|--help) usage ;;
  *) die "usage: $0 {status|verify|capture|check|apply|update|finalize}" ;;
esac
