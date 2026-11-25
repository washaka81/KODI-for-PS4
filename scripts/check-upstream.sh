#!/usr/bin/env bash
#
# check-upstream.sh — Compare the PS4 port with xbmc/xbmc master.
#
# Lists the files the port has touched (committed or not) that also changed
# upstream since the port base (port/base.conf), so those changes can be
# reviewed before a build.  build-kodi-ps4.sh runs this automatically unless
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
#   PORT_BASE_CONF    Port base file (default: <repo>/port/base.conf)
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
      FILE="${2:?--file necesita una ruta}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'opción desconocida: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

git_kodi() { git -C "$KODI_DIR" "$@"; }

[[ -d "$KODI_DIR/.git" || -f "$KODI_DIR/.git" ]] ||
  die "no hay repositorio git en $KODI_DIR"

if (( FETCH )); then
  if ! git_kodi fetch --no-tags "$REMOTE" "$BRANCH" >/dev/null 2>&1; then
    say "aviso: no se pudo actualizar $REMOTE/$BRANCH; uso la referencia local"
  fi
fi

UPSTREAM="refs/remotes/$REMOTE/$BRANCH"
git_kodi rev-parse --verify --quiet "$UPSTREAM" >/dev/null ||
  die "no existe $UPSTREAM; ejecuta sin --no-fetch o revisa el remoto"

# Base del port: la base de la serie (port/base.conf), NO el merge-base.
# El merge-base apunta a donde la rama de release de Kodi bifurcó de
# master (p. ej. 2026-10-02 para Piers), así que difundir contra él
# contaba como "nuestros" los commits de la propia rama de release y el
# solape se llenaba de ruido upstream-vs-upstream.
BASE_CONF="${PORT_BASE_CONF:-$REPO_ROOT/port/base.conf}"
[[ -f "$BASE_CONF" ]] ||
  die "falta $BASE_CONF (fuente de verdad de la base del port)"
BASE="$(sed -n 's/^KODI_BASE=//p' "$BASE_CONF" | head -1 | tr -d '[:space:]')"
[[ -n "$BASE" ]] || die "no hay KODI_BASE en $BASE_CONF"
git_kodi rev-parse --verify --quiet "$BASE^{commit}" >/dev/null ||
  die "la base del port $BASE no existe en $KODI_DIR (prueba: git -C src/kodi fetch --tags origin)"
BASE="$(git_kodi rev-parse "$BASE^{commit}")"

# Punto de bifurcación (solo informativo).
MERGE_BASE="$(git_kodi merge-base HEAD "$UPSTREAM" 2>/dev/null || true)"
FORK_DESC=""
[[ -n "$MERGE_BASE" ]] &&
  FORK_DESC="$(git_kodi log -1 --format='%h %cs %s' "$MERGE_BASE")"

SHALLOW=0
[[ "$(git_kodi rev-parse --is-shallow-repository)" == "true" ]] && SHALLOW=1

if (( SHALLOW )); then
  AHEAD="?"
  BEHIND="?"
else
  AHEAD="$(git_kodi rev-list --count "$BASE..HEAD")"
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
    die "no encuentro $path ni en el árbol ni en $UPSTREAM"

  ours_count="$(git_kodi rev-list --count "$BASE..HEAD" -- "$path")"
  upstream_count="$(git_kodi rev-list --count "$BASE..$UPSTREAM" -- "$path")"
  local_state="$(git_kodi status --porcelain -- "$path")"
  [[ -n "$local_state" ]] || local_state="(sin cambios locales)"

  printf 'fichero:    %s\n' "$path"
  printf 'base:       %s\n' "$BASE_DESC"
  printf 'upstream:   %s\n' "$UPSTREAM_DESC"
  printf 'commits:    %s propios, %s upstream desde la base\n' "$ours_count" "$upstream_count"
  printf 'local:      %s\n' "$local_state"

  if (( PATCH )); then
    if (( upstream_count > 0 )); then
      printf '\n=== cambios upstream (%s..%s) ===\n' "${BASE:0:9}" "${UPSTREAM#refs/remotes/}"
      git_kodi diff --stat "$BASE..$UPSTREAM" -- "$path"
      git_kodi diff "$BASE..$UPSTREAM" -- "$path"
    else
      printf '\nsin cambios upstream para este fichero desde la base\n'
    fi

    if (( ours_count > 0 )); then
      printf '\n=== nuestros commits (%s..HEAD) ===\n' "${BASE:0:9}"
      git_kodi diff --stat "$BASE..HEAD" -- "$path"
      git_kodi diff "$BASE..HEAD" -- "$path"
    fi

    if [[ -n "$(git_kodi diff -- "$path")" ]]; then
      printf '\n=== cambios locales sin commit ===\n'
      git_kodi diff -- "$path"
    fi
  fi
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Files touched by the port: series commits, working tree and untracked.
# base -> working tree covers committed + staged + unstaged in one diff;
# the old BASE..HEAD + worktree-only union missed purely staged files.
{
  git_kodi diff --name-only "$BASE"
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
  printf '# Comprobación upstream — %s\n\n' "$(date -Iseconds)"
  printf 'repo:      %s\n' "$KODI_DIR"
  printf 'upstream:  %s/%s — %s\n' "$REMOTE" "$BRANCH" "$UPSTREAM_DESC"
  printf 'base:      %s\n' "$BASE_DESC"
  if [[ -n "$FORK_DESC" ]]; then
    printf 'fork:      %s\n' "$FORK_DESC"
  fi
  if (( SHALLOW )); then
    printf 'posición:  %s commits propios (clon shallow: no se puede contar lo que vamos detrás)\n' "$AHEAD"
  else
    printf 'posición:  %s commits propios, %s commits detrás de upstream\n' "$AHEAD" "$BEHIND"
  fi
  printf 'nuestros:  %s ficheros tocados\n' "$OURS_COUNT"
  printf 'upstream:  %s ficheros cambiados desde la base\n' "$THEIRS_COUNT"
  printf 'solape:    %s ficheros\n\n' "$OVERLAP_COUNT"

  if (( OVERLAP_COUNT > 0 )); then
    printf '## Ficheros nuestros que también cambiaron upstream\n\n'
    printf '%5s  %-66s  %s\n' 'commits' 'fichero' 'último cambio upstream'
    while IFS= read -r path; do
      count="$(git_kodi rev-list --count "$BASE..$UPSTREAM" -- "$path")"
      last="$(git_kodi log -1 --format='%h %cs %s' "$BASE..$UPSTREAM" -- "$path")"
      printf '%d\t%s\t%s\n' "$count" "$path" "$last"
    done <"$TMP/overlap" |
      LC_ALL=C sort -rn -k1,1 |
      while IFS=$'\t' read -r count path last; do
        printf '%5d  %-66s  %s\n' "$count" "$path" "$last"
      done
    printf '\nPara revisar un fichero:\n'
    printf '  ./scripts/check-upstream.sh --no-fetch --file <ruta>\n'
  else
    printf 'Ningún fichero tocado por el port cambió upstream desde la base.\n'
  fi
} >"$REPORT"

if (( QUIET )); then
  if (( SHALLOW )); then
    say "upstream: $UPSTREAM_DESC (clon shallow)"
  else
    say "upstream: $UPSTREAM_DESC — $AHEAD propios, $BEHIND detrás"
  fi
  if (( OVERLAP_COUNT > 0 )); then
    say "AVISO: $OVERLAP_COUNT ficheros nuestros cambiaron upstream; informe: $REPORT"
  else
    say "sin solape con upstream; informe: $REPORT"
  fi
else
  cat "$REPORT"
fi

if (( STRICT )) && (( OVERLAP_COUNT > 0 )); then
  exit 1
fi
