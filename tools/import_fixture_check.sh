#!/usr/bin/env bash
# Real-data check of the legacy importer: clones three public pychron DVC
# repositories at pinned commits, imports them into a throwaway SQLite store
# with `elctl import`, and verifies each source.
#
#   tools/import_fixture_check.sh <build dir>        e.g. build/dev
#   KEEP=1 tools/import_fixture_check.sh build/dev   keep the temp directory
#
# Needs network access and git (>= 2.32). Not part of CI. Nothing is written
# outside the temporary directory; no credentials are used.
#
# Exit status: the worst exit code of `import verify` (0 ok, 1 not ok,
# 2 error), once the import has run. Anything that fails before that (a
# clone, `import add`, `import status`) ends the script at once with exit
# status 2, whatever the failing command returned. A non-zero `import run` is
# reported ("run exit code") and does not end the script.

set -Eeuo pipefail
trap 'echo "$0: failed at line $LINENO" >&2; exit 2' ERR

BASE_URL=https://github.com/NMGRLData
META_NAME=MetaData
META_URL=$BASE_URL/MetaData
META_SHA=0ef8d84412ab2f7401c6bae86e0a821ccded72ed
BLANK_NAME=Felix_blank180
BLANK_URL=$BASE_URL/Felix_blank180
BLANK_SHA=d831c6231d0d0cbbb6bc474f5ab296d25a47004a
PROJECT_NAME=IR1010
PROJECT_URL=$BASE_URL/IR1010
PROJECT_SHA=5283d6c85f3e2f57aedd6ad9d0418c7a7257bcd0
TZ_NAME=America/Denver
PIN_BRANCH=pinned

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <build dir>" >&2
  exit 2
fi
elctl=$1/apps/elctl/elctl
if [[ ! -x $elctl ]]; then
  echo "$0: $elctl not found or not executable; build elctl first" >&2
  exit 2
fi
elctl=$(cd "$(dirname "$elctl")" && pwd)/$(basename "$elctl")

work=$(mktemp -d "${TMPDIR:-/tmp}/import_fixture_check.XXXXXX")
cleanup() {
  if [[ ${KEEP:-0} == 1 ]]; then
    echo "kept: $work"
  else
    rm -rf "$work"
  fi
}
trap cleanup EXIT

repos=$work/repos
cache=$work/cache
db=$work/store.db
mkdir -p "$repos" "$cache"

clone_pinned() { # name url sha
  echo "== clone $1 at $3"
  git clone --quiet "$2" "$repos/$1"
  git -C "$repos/$1" checkout --quiet -B "$PIN_BRANCH" "$3"
  git -C "$repos/$1" rev-parse --verify --quiet "$3^{commit}" >/dev/null
}

clone_pinned "$META_NAME" "$META_URL" "$META_SHA"
clone_pinned "$BLANK_NAME" "$BLANK_URL" "$BLANK_SHA"
clone_pinned "$PROJECT_NAME" "$PROJECT_URL" "$PROJECT_SHA"

# The store is created by the first `import add`.
store=sqlite:$db
common=(--db "$store" --cache "$cache")

echo "== add"
"$elctl" import add "${common[@]}" --kind meta_repo --source "$repos/$META_NAME" \
  --branch "$PIN_BRANCH" --tz "$TZ_NAME"
"$elctl" import add "${common[@]}" --kind project_repo --source "$repos/$BLANK_NAME" \
  --branch "$PIN_BRANCH" --tz "$TZ_NAME" --reference-runs --catalog-from-repos
"$elctl" import add "${common[@]}" --kind project_repo --source "$repos/$PROJECT_NAME" \
  --branch "$PIN_BRANCH" --tz "$TZ_NAME" --catalog-from-repos

echo "== run --all"
start=$SECONDS
run_code=0
"$elctl" import run "${common[@]}" --all || run_code=$?
elapsed=$((SECONDS - start))
echo "run exit code: $run_code"

echo "== status"
status_out=$("$elctl" import status "${common[@]}")
echo "$status_out"

echo "== conflicts"
"$elctl" import conflicts "${common[@]}" || true

worst=0
for name in "$META_NAME" "$BLANK_NAME" "$PROJECT_NAME"; do
  echo "== verify $name"
  code=0
  "$elctl" import verify "${common[@]}" --source "$name" || code=$?
  echo "verify $name exit code: $code"
  if ((code > worst)); then worst=$code; fi
done

commits=$(echo "$status_out" | awk '{ split($5, p, "/"); n += p[1] } END { print n + 0 }')
size=$(wc -c <"$db" | tr -d ' ')
echo "== summary"
echo "run wall time: ${elapsed} s"
awk -v c="$commits" -v t="$elapsed" 'BEGIN { if (t < 1) t = 1; printf "commits: %d, %.1f commits/s (wall time counted in whole seconds)\n", c, c / t }'
echo "store file size: ${size} bytes"
echo "worst verify exit code: $worst"
exit "$worst"
