#!/bin/sh
# Generate only Common's two adapted translation units and their private headers.
# Never patch the submodule in place (parallel builds and other consumers share it).
set -eu
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?output directory required}
work=$(mktemp -d "${TMPDIR:-/tmp}/cs-kz-common.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
mkdir "$work/src"
cp "$root/moonlight-common-c/src/InputStream.c" "$root/moonlight-common-c/src/LinkedBlockingQueue.c" "$work/src/"
(
    cd "$work"
    GIT_CEILING_DIRECTORIES="$(dirname "$(pwd -P)")" git apply --include='src/*' "$root/cs-kz-input.patch"
)
mkdir -p "$out"
cp "$work"/src/* "$out/"
