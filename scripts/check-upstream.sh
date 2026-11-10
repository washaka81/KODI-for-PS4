#!/usr/bin/env bash
#
# check-upstream.sh — Compare the PS4 port with xbmc/xbmc master.
#
# Lists the files the port has touched (committed or not) that also changed
# upstream since our merge base, so those changes can be reviewed before a
# build.  build-kodi-ps4.sh runs this automatically unless
# SKIP_UPSTREAM_CHECK=1.
#
# Usage:
#   ./scripts/check-upstream.sh [options]
#   ./scripts/check-upstream.sh --file xbmc/application/Application.cpp
#
# Options:
#   --no-fetch        Use the last known origin/master instead of fetching.
#   --quiet           Print only the summary (the full report is still written).
#   --strict          Exit 1 when files we touched changed upstream.
#   --file PATH       Inspect one file: upstream and local changes since base.
#   --no-patch        With --file, print metadata only (no diffs).
#   -h, --help        Show this help.
#
# Environment:
#   KODI_DIR          Submodule path (default: <repo>/src/kodi)
#   UPSTREAM_REMOTE   Git remote (default: origin)
#   UPSTREAM_BRANCH   Git branch (default: master)
#   UPSTREAM_REPORT   Report path (default: <repo>/build/upstream-check.txt)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KODI_DIR="${KODI_DIR:-$REPO_ROOT/src/kodi}"
REMOTE="${UPSTREAM_REMOTE:-origin}"
BRANCH="${UPSTREAM_BRANCH:-master}"
REPORT="${UPSTREAM_REPORT:-$REPO_ROOT/build/upstream-check.txt}"

FETCH=1
QUIET=0
STRICT=0
FILE=""
PATCH=1

usage() {
  sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
  printf '\033[1;31m[upstream]\033[0m %s\n' "$*" >&2
  exit 1
}

say() {
  printf '\033[1;36m[upstream]\033[0m %s\n' "$*"
}

while (( $# )); do
  case "$1" in
    --no-fetch) FETCH=0 ;;
    --quiet) QUIET=1 ;;
    --strict) STRICT=1 ;;
    --no-patch) PATCH=0 ;;
    --file)
      FILE="${2:?--file needs a path}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

git_kodi() { git -C "$KODI_DIR" "$@"; }

[[ -d "$KODI_DIR/.git" || -f "$KODI_DIR/.git" ]] ||
  die "no git repository at $KODI_DIR"

if (( FETCH )); then
  if ! git_kodi fetch --no-tags "$REMOTE" "$BRANCH" >/dev/null 2>&1; then
    say "warning: could not update $REMOTE/$BRANCH; using the local reference"
  fi
fi

UPSTREAM="refs/remotes/$REMOTE/$BRANCH"
git_kodi rev-parse --verify --quiet "$UPSTREAM" >/dev/null ||
  die "$UPSTREAM does not exist; run without --no-fetch or check the remote"

BASE="$(git_kodi merge-base HEAD "$UPSTREAM")" ||
  die "HEAD y $UPSTREAM no comparten historia"

SHALLOW=0
[[ "$(git_kodi rev-parse --is-shallow-repository)" == "true" ]] && SHALLOW=1

if (( SHALLOW )); then
  AHEAD="?"
  BEHIND="?"
else
  AHEAD="$(git_kodi rev-list --count "$UPSTREAM..HEAD")"
  BEHIND="$(git_kodi rev-list --count "HEAD..$UPSTREAM")"
fi

UPSTREAM_DESC="$(git_kodi log -1 --format='%h %cs %s' "$UPSTREAM")"
BASE_DESC="$(git_kodi log -1 --format='%h %cs %s' "$BASE")"

normalize_path() {
  local path="$1"
  path="${path#"$KODI_DIR"/}"
  path="${path#./}"
  printf '%s' "$path"
}

if [[ -n "$FILE" ]]; then
  path="$(normalize_path "$FILE")"
  [[ -e "$KODI_DIR/$path" ]] ||
    git_kodi cat-file -e "$UPSTREAM:$path" 2>/dev/null ||
    die "cannot find $path in the tree or in $UPSTREAM"

  ours_count="$(git_kodi rev-list --count "$BASE..HEAD" -- "$path")"
  upstream_count="$(git_kodi rev-list --count "$BASE..$UPSTREAM" -- "$path")"
  local_state="$(git_kodi status --porcelain -- "$path")"
  [[ -n "$local_state" ]] || local_state="(no local changes)"

  printf 'file:       %s\n' "$path"
  printf 'base:       %s\n' "$BASE_DESC"
  printf 'upstream:   %s\n' "$UPSTREAM_DESC"
  printf 'commits:    %s ours, %s upstream since base\n' "$ours_count" "$upstream_count"
  printf 'local:      %s\n' "$local_state"

  if (( PATCH )); then
    if (( upstream_count > 0 )); then
      printf '\n=== cambios upstream (%s..%s) ===\n' "${BASE:0:9}" "${UPSTREAM#refs/remotes/}"
      git_kodi diff --stat "$BASE..$UPSTREAM" -- "$path"
      git_kodi diff "$BASE..$UPSTREAM" -- "$path"
    else
      printf '\nno upstream changes for this file since the base\n'
    fi

    if (( ours_count > 0 )); then
      printf '\n=== nuestros commits (%s..HEAD) ===\n' "${BASE:0:9}"
      git_kodi diff --stat "$BASE..HEAD" -- "$path"
      git_kodi diff "$BASE..HEAD" -- "$path"
    fi

    if [[ -n "$(git_kodi diff -- "$path")" ]]; then
      printf '\n=== uncommitted local changes ===\n'
      git_kodi diff -- "$path"
    fi
  fi
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Files touched by the port: 38 local commits, working tree and untracked.
{
  git_kodi diff --name-only "$BASE..HEAD"
  git_kodi diff --name-only
  git_kodi ls-files --others --exclude-standard
} | LC_ALL=C sort -u >"$TMP/ours"

# Files changed upstream since the merge base.
git_kodi diff --name-only "$BASE..$UPSTREAM" |
  LC_ALL=C sort -u >"$TMP/theirs"

LC_ALL=C comm -12 "$TMP/ours" "$TMP/theirs" >"$TMP/overlap"

OURS_COUNT="$(wc -l <"$TMP/ours" | tr -d ' ')"
THEIRS_COUNT="$(wc -l <"$TMP/theirs" | tr -d ' ')"
OVERLAP_COUNT="$(wc -l <"$TMP/overlap" | tr -d ' ')"

mkdir -p "$(dirname "$REPORT")"

{
  printf '# Upstream check — %s\n\n' "$(date -Iseconds)"
  printf 'repo:      %s\n' "$KODI_DIR"
  printf 'upstream:  %s/%s — %s\n' "$REMOTE" "$BRANCH" "$UPSTREAM_DESC"
  printf 'base:      %s\n' "$BASE_DESC"
  if (( SHALLOW )); then
    printf 'position:  %s own commits (shallow clone: cannot count how far behind)\n' "$AHEAD"
  else
    printf 'position:  %s own commits, %s commits behind upstream\n' "$AHEAD" "$BEHIND"
  fi
  printf 'ours:      %s touched files\n' "$OURS_COUNT"
  printf 'upstream:  %s files changed since base\n' "$THEIRS_COUNT"
  printf 'overlap:   %s files\n\n' "$OVERLAP_COUNT"

  if (( OVERLAP_COUNT > 0 )); then
    printf '## Our files that also changed upstream\n\n'
    printf '%5s  %-66s  %s\n' 'commits' 'file' 'latest upstream change'
    while IFS= read -r path; do
      count="$(git_kodi rev-list --count "$BASE..$UPSTREAM" -- "$path")"
      last="$(git_kodi log -1 --format='%h %cs %s' "$BASE..$UPSTREAM" -- "$path")"
      printf '%d\t%s\t%s\n' "$count" "$path" "$last"
    done <"$TMP/overlap" |
      LC_ALL=C sort -rn -k1,1 |
      while IFS=$'\t' read -r count path last; do
        printf '%5d  %-66s  %s\n' "$count" "$path" "$last"
      done
    printf '\nTo inspect a file:\n'
    printf '  ./scripts/check-upstream.sh --no-fetch --file <ruta>\n'
  else
    printf 'No file touched by the port changed upstream since the base.\n'
  fi
} >"$REPORT"

if (( QUIET )); then
  if (( SHALLOW )); then
    say "upstream: $UPSTREAM_DESC (clon shallow)"
  else
    say "upstream: $UPSTREAM_DESC — $AHEAD own, $BEHIND behind"
  fi
  if (( OVERLAP_COUNT > 0 )); then
    say "WARNING: $OVERLAP_COUNT of our files changed upstream; report: $REPORT"
  else
    say "no overlap with upstream; report: $REPORT"
  fi
else
  cat "$REPORT"
fi

if (( STRICT )) && (( OVERLAP_COUNT > 0 )); then
  exit 1
fi
