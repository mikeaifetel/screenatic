#!/usr/bin/env bash
# MSE latency test for the Vega probe: streams live H.264 in fragmented MP4 to the TV's :7100,
# with this Mac's wall clock burned into the picture. Latency = TV clock (shown under the video)
# minus the clock in the picture. Both devices must use network time.
#
#   vega/scripts/stream_test.sh <tv-ip> [seconds]
#
# Audio: a 440 Hz tone with a short high beep at every whole second (FLAC), muxed into the same
# fMP4 stream. MSE on Vega takes FLAC/LPCM but not AAC-ELD or ALAC, so this is the path decoded
# AirPlay audio would take. The beep lands on each second change of the burned-in clock.
# AUDIO=pcm sends uncompressed 16-bit PCM ("ipcm") instead of FLAC.
set -euo pipefail
TV=${1:?usage: stream_test.sh <tv-ip> [seconds]}
SECS=${2:-60}
ACODEC=flac; [ "${AUDIO:-flac}" = pcm ] && ACODEC=pcm_s16le
# Seconds since local midnight, so pts (which tracks wall time under -re) prints as the clock.
OFFSET=$(python3 -c "import time,datetime;n=datetime.datetime.now();print(n.hour*3600+n.minute*60+n.second+n.microsecond/1e6)")
exec ffmpeg -hide_banner -loglevel warning -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=44100" \
  -vf "drawtext=fontfile=/System/Library/Fonts/Menlo.ttc:fontsize=72:fontcolor=white:box=1:boxcolor=black@0.7:x=40:y=40:text='%{pts\:hms\:$OFFSET}'" \
  -map 0:v -map 1:a -c:a "$ACODEC" -ac 2 -t "$SECS" \
  -c:v libx264 -profile:v main -level 3.1 -preset ultrafast -tune zerolatency -g 30 -bf 0 -pix_fmt yuv420p \
  -f mp4 -movflags frag_keyframe+empty_moov+default_base_moof -frag_duration 100000 \
  "tcp://$TV:7100"
