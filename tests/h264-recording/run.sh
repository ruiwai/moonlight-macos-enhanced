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
    fixture_codec=$1 width=$2 height=$3 pixel_format=$4 output=$5
    case "$fixture_codec" in
        h264)
            tag=avc1
            set -- -c:v libx264 -g 4 -sc_threshold 0 -bf 0 -x264-params open-gop=0
            ;;
        hevc)
            tag=hvc1
            params='keyint=4:min-keyint=4:scenecut=0:bframes=0:open-gop=0:pools=none:frame-threads=1:log-level=error'
            set -- -c:v libx265
            if [ "$pixel_format" = yuv420p10le ]; then
                # Set both the HEVC VUI and the MOV color description explicitly.
                params="$params:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
                set -- "$@" -profile:v main10 -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc
            fi
            set -- "$@" -x265-params "$params"
            ;;
    esac
    ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=${width}x${height}:rate=25" \
        -frames:v 8 -vf 'setpts=PTS+gte(N\,6)' -fps_mode vfr -an \
        "$@" -pix_fmt "$pixel_format" -tag:v "$tag" "$output"

    # Probe all streams so extra tracks also fail the single-stream check.
    actual=$(ffprobe -v error -count_packets -show_entries stream=codec_name,codec_tag_string,width,height,pix_fmt,nb_read_packets \
        -of csv=p=0 "$output")
    expected="$fixture_codec,$tag,$width,$height,$pixel_format,8"
    fields='codec,tag,width,height,pixel format,packets'
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $fixture_codec/${output##*/}: expected $expected ($fields); ffprobe reported: $actual" >&2
        exit 1
    fi
    echo "Fixture $fixture_codec/${output##*/}: $actual ($fields)"
    if [ "$pixel_format" = yuv420p10le ]; then
        color=$(ffprobe -v error -show_entries stream=profile,color_space,color_transfer,color_primaries -of csv=p=0 "$output")
        if [ "$color" != 'Main 10,bt2020nc,smpte2084,bt2020' ]; then
            echo "FAIL: expected HEVC Main 10 with BT.2020/PQ, got $color" >&2
            exit 1
        fi
    fi
}

# A MOV may contain the right packet bytes but lack the decoder configuration
# carried outside those packets (e.g. HEVC VPS/SPS/PPS in hvcC). Reopen it
# independently and compare the decoded pixels, profile, bit depth and color tags.
picture_signature() {
    ffprobe -v error -select_streams v:0 -show_entries stream=profile,pix_fmt,color_space,color_transfer,color_primaries \
        -of default=noprint_wrappers=1 "$1" || return 1
    ffmpeg -nostdin -hide_banner -loglevel error -xerror -err_detect explode -i "$1" -map 0:v:0 \
        -vf "trim=start_frame=$2" -fps_mode passthrough -f hash -hash sha256 -
}

for codec in h264 hevc; do
    dir="$work/$codec"
    mkdir "$dir"
    make_fixture "$codec" 96 64 yuv420p "$dir/input.mov"
    changed_format=yuv420p
    if [ "$codec" = hevc ]; then changed_format=yuv420p10le; fi
    make_fixture "$codec" 128 96 "$changed_format" "$dir/changed.mov"
    # The harness starts each output at the second IDR (input frame index 4).
    picture_signature "$dir/input.mov" 4 > "$dir/input.sig"
    picture_signature "$dir/changed.mov" 4 > "$dir/changed.sig"
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
        "$dir/recorded" "$dir/no-idr" "$dir/rollover" || { status=1; continue; }
    # smoke checked file counts and part ordering. Part 1 of each session
    # comes from input.mov; the remaining rollover parts come from changed.mov.
    for session in recorded rollover; do
        reference="$dir/input.sig"
        for part in "$dir/$session"/*.mov; do
            picture_signature "$part" 0 > "$dir/part.sig"
            if ! diff -u "$reference" "$dir/part.sig"; then
                echo "FAIL: $part decodes differently from its source GOP" >&2
                status=1
            fi
            reference="$dir/changed.sig"
        done
    done
done
exit "$status"
