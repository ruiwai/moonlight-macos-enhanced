#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
for fixture in motion wheel check_native_wiring keyboard lifecycle; do
    python3 "tests/input-coherence/$fixture.py"
done
python3 tests/window-cleanup/shared_input.py

# Exercise the patch against this branch's Common (including its existing
# controller queue and shutdown regressions), without modifying the checkout.
common=moonlight-common/moonlight-common-c
work=$(mktemp -d "${TMPDIR:-/tmp}/input-coherence.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/common"
git -C "$common" archive HEAD | tar -x -C "$work/common"
git -C "$common/enet" archive HEAD | tar -x -C "$work/common/enet"
(cd "$work/common" && GIT_CEILING_DIRECTORIES="$work" git apply "$root/moonlight-common/cs-kz-input.patch")
sh moonlight-common/prepare-input-sources.sh "$work/generated"
for source in InputStream.c LinkedBlockingQueue.c InputStreamPolicy.h InputQueueExtensions.h; do
    cmp "$work/generated/$source" "$work/common/src/$source"
done

targets=(input-stream-policy-test input-stream-worker-test linked-blocking-queue-atomic-test
         controller-handoff-queue-tests input-stream-shutdown-tests controller-motion-queue-tests)
cmake -S "$work/common" -B "$work/build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=OFF "$@"
cmake --build "$work/build" --parallel 4 --target "${targets[@]}"
ctest --test-dir "$work/build" --output-on-failure -R "^($(IFS='|'; echo "${targets[*]}"))$"
