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
        echo 'SKIP: H.264/HEVC recorder smoke test requires macOS and AVFoundation.' >&2
        exit 77
    fi
    for file in "$stream/MLCompressedVideoRecorder.h" "$stream/MLCompressedVideoRecorder.m"; do
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
    fixture_codec=$1 width=$2 height=$3 output=$4
    case "$fixture_codec" in
        h264)
            tag=avc1
            set -- -c:v libx264 -g 4 -sc_threshold 0 -bf 0 -x264-params open-gop=0
            ;;
        hevc)
            tag=hvc1
            set -- -c:v libx265 -x265-params 'keyint=4:min-keyint=4:scenecut=0:bframes=0:open-gop=0:pools=none:frame-threads=1:log-level=error'
            ;;
    esac
    ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=${width}x${height}:rate=25" \
        -frames:v 8 -vf 'setpts=PTS+gte(N\,6)' -fps_mode vfr -an \
        "$@" -pix_fmt yuv420p -tag:v "$tag" "$output"

    # Probe all streams so extra tracks also fail the single-stream check.
    actual=$(ffprobe -v error -count_packets -show_entries stream=codec_name,codec_tag_string,width,height,nb_read_packets \
        -of csv=p=0 "$output")
    expected="$fixture_codec,$tag,$width,$height,8"
    fields='codec,tag,width,height,packets'
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $fixture_codec/${output##*/}: expected $expected ($fields); ffprobe reported: $actual" >&2
        exit 1
    fi
    echo "Fixture $fixture_codec/${output##*/}: $actual ($fields)"
}
for codec in h264 hevc; do
    mkdir "$work/$codec"
    make_fixture "$codec" 96 64 "$work/$codec/input.mov"
    make_fixture "$codec" 128 96 "$work/$codec/changed.mov"
done

if "$fixtures_only"; then
    exit 0
fi

xcrun --sdk macosx clang -fobjc-arc -fmodules -Wall -Wextra \
    -Wno-deprecated-declarations -fmodules-cache-path="$work/modules" \
    -I "$stream" -I "$root/Limelight/Utility" \
    "$test_dir/smoke.m" "$stream/MLCompressedVideoRecorder.m" \
    -framework Foundation -framework AVFoundation -framework CoreMedia \
    -o "$work/smoke"

status=0
for codec in h264 hevc; do
    dir="$work/$codec"
    mkdir "$dir/recorded" "$dir/no-idr" "$dir/rollover"
    echo "Testing $codec passthrough"
    "$work/smoke" "$codec" "$dir/input.mov" "$dir/changed.mov" \
        "$dir/recorded" "$dir/no-idr" "$dir/rollover" || status=1
done
exit "$status"
