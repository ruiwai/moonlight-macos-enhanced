#!/bin/sh
set -eu

test_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$test_dir/../.." && pwd)
stream="$root/Limelight/Stream"

fixtures_only=false
if [ "$#" -eq 1 ] && [ "$1" = --fixtures-only ]; then
    fixtures_only=true
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--fixtures-only]" >&2
    exit 2
fi

if ! "$fixtures_only"; then
    if [ "$(uname -s)" != Darwin ]; then
        echo 'SKIP: H.264 recorder smoke test requires macOS and AVFoundation.' >&2
        exit 77
    fi
    for file in "$stream/MLH264Recorder.h" "$stream/MLH264Recorder.m"; do
        if [ ! -f "$file" ]; then
            echo "Missing recorder source: $file" >&2
            exit 1
        fi
    done
    if ! command -v xcrun >/dev/null 2>&1; then
        echo 'Required tool not found: xcrun' >&2
        exit 1
    fi
fi

for tool in ffmpeg ffprobe; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required tool not found: $tool" >&2
        exit 1
    fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/h264-recording.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Two closed GOPs (IDR, P, P, P), with a deliberate timestamp gap in the second.
make_fixture() {
    ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=${1}x${2}:rate=25" \
        -frames:v 8 -vf 'setpts=PTS+gte(N\,6)' -fps_mode vfr -an \
        -c:v libx264 -pix_fmt yuv420p -g 4 -sc_threshold 0 -bf 0 \
        -x264-params open-gop=0 "$3"

    # Probe all streams so extra tracks also fail the single-stream check.
    actual=$(ffprobe -v error -count_packets -show_entries stream=codec_name,width,height,nb_read_packets \
        -of csv=p=0 "$3")
    expected="h264,$1,$2,8"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: ${3##*/}: expected $expected (codec,width,height,packets); ffprobe reported: $actual" >&2
        exit 1
    fi
    echo "Fixture ${3##*/}: $actual (codec,width,height,packets)"
}
make_fixture 96 64 "$work/input.mov"
make_fixture 128 96 "$work/changed.mov"

if "$fixtures_only"; then
    exit 0
fi

mkdir "$work/recorded" "$work/no-idr" "$work/rollover"

xcrun --sdk macosx clang -fobjc-arc -fmodules -Wall -Wextra \
    -Wno-deprecated-declarations -fmodules-cache-path="$work/modules" \
    -I "$stream" -I "$root/Limelight/Utility" \
    "$test_dir/smoke.m" "$stream/MLH264Recorder.m" \
    -framework Foundation -framework AVFoundation -framework CoreMedia \
    -o "$work/smoke"

"$work/smoke" "$work/input.mov" "$work/changed.mov" "$work/recorded" "$work/no-idr" "$work/rollover"
