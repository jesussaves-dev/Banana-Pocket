#!/bin/sh
set -eu

work=$1
host=$work/banana-retro
core=$work/fixture.so
rom="$work/test content.rom"

unset BANANA_RETRO_FIXTURE_MODE
printf 'BRC1\n' > "$rom"
printf 'NOPE\n' > "$work/rejected.rom"
: > "$work/empty.rom"
dd if=/dev/null of="$work/oversized.rom" bs=1 seek=67108865 count=0 2> "$work/dd.log"
mkfifo "$work/fifo.rom"

initialized='fixture api
fixture set_environment
fixture set_video
fixture set_sample
fixture set_batch
fixture set_poll
fixture set_input
fixture init
fixture system_info'

loaded="$initialized
fixture load
fixture av_info"

running="$loaded
fixture controller"

two_frames="$running
fixture run 1
fixture run 2
fixture unload
fixture deinit"

expected_frames='frame 1 video=1 2x2 pitch=6 pixel=00002301 audio=3 first=1,-2 last=5,-6 input=1/2
frame 2 video=1 2x2 pitch=6 pixel=00002302 audio=3 first=2,-2 last=6,-6 input=1/2'

check_trace()
{
    if [ -n "$1" ]; then
        printf '%s\n' "$1" > "$work/expected.trace"
    else
        : > "$work/expected.trace"
    fi
    sed -n '/^fixture /p' "$work/stderr" > "$work/actual.trace"
    diff -u "$work/expected.trace" "$work/actual.trace"
}

check_success()
{
    mode=$1
    expected=$2
    BANANA_RETRO_FIXTURE_MODE=$mode "$host" "$core" "$rom" --frames 2 \
        > "$work/stdout" 2> "$work/stderr" || {
            sed -n '1,30p' "$work/stderr" >&2
            return 1
        }
    printf '%s\n' "$expected" > "$work/expected.stdout"
    diff -u "$work/expected.stdout" "$work/stdout"
    check_trace "$two_frames"
    diff -u "$work/expected.trace" "$work/stderr"
}

check_failure()
{
    label=$1
    expected_status=$2
    diagnostic=$3
    trace=$4
    shift 4

    status=0
    "$@" > "$work/stdout" 2> "$work/stderr" || status=$?
    if [ "$status" -ne "$expected_status" ]; then
        printf '%s: expected exit %s, got %s\n' "$label" "$expected_status" "$status" >&2
        sed -n '1,30p' "$work/stderr" >&2
        return 1
    fi
    if [ -s "$work/stdout" ] || ! grep -Fq "$diagnostic" "$work/stderr"; then
        printf '%s: unexpected output\n' "$label" >&2
        sed -n '1,30p' "$work/stderr" >&2
        return 1
    fi
    check_trace "$trace"
}

check_success memory "$expected_frames"
check_success samples "$expected_frames"
check_success path "$expected_frames"
check_success default-format "$expected_frames"
check_success xrgb 'frame 1 video=1 2x2 pitch=12 pixel=00123401 audio=3 first=1,-2 last=5,-6 input=1/2
frame 2 video=1 2x2 pitch=12 pixel=00123402 audio=3 first=2,-2 last=6,-6 input=1/2'

check_failure no-library 1 'Cannot open core' '' "$host" "$work/missing.so" "$rom" --frames 2
check_failure bad-library 1 'Cannot load core' '' "$host" "$rom" "$rom" --frames 2
check_failure missing-export 1 'Missing core export retro_get_memory_size' '' \
    "$host" "$work/fixture-missing-export.so" "$rom" --frames 2
check_failure wrong-api 1 'Unsupported Libretro API version' 'fixture api' \
    "$host" "$work/fixture-wrong-api.so" "$rom" --frames 2

check_failure rejected-content 1 'Core rejected content' "$initialized
fixture load
fixture deinit" "$host" "$core" "$work/rejected.rom" --frames 2

for path in "$work/empty.rom" "$work" "$work/fifo.rom"; do
    check_failure invalid-file 1 'Content must be a nonempty regular file' "$initialized
fixture deinit" timeout 5 "$host" "$core" "$path" --frames 2
done

check_failure missing-content 1 'Cannot open content' "$initialized
fixture deinit" "$host" "$core" "$work/missing.rom" --frames 2
check_failure oversized-memory 1 'Content exceeds the 64 MiB limit' "$initialized
fixture deinit" "$host" "$core" "$work/oversized.rom" --frames 2
check_failure oversized-path 1 'Content exceeds the 64 MiB limit' "$initialized
fixture deinit" env BANANA_RETRO_FIXTURE_MODE=path \
    "$host" "$core" "$work/oversized.rom" --frames 2

check_failure invalid-av 1 'Core returned invalid AV information' "$loaded
fixture unload
fixture deinit" env BANANA_RETRO_FIXTURE_MODE=bad-av "$host" "$core" "$rom" --frames 2

check_failure bad-pitch 1 'Core returned invalid frame data' "$running
fixture run 1
fixture unload
fixture deinit" env BANANA_RETRO_FIXTURE_MODE=bad-pitch "$host" "$core" "$rom" --frames 2

for count in 0 2x 10001; do
    check_failure invalid-count 2 'Frame count must be between' '' \
        "$host" "$core" "$rom" --frames "$count"
done

printf '%s\n' 'banana-retro lifecycle checks passed'
