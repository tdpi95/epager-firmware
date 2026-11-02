#!/bin/sh
# Speech for the Paper speaker test (PLAN D89, CONFIG_EPAGER_SOUND_TEST): ffmpeg's flite voice, 16 kHz mono
# signed 16-bit little-endian, peak about 14000. Output: components/app/assets/voice_test.pcm
set -e
out="$(dirname "$0")/../../components/app/assets/voice_test.pcm"
mkdir -p "$(dirname "$out")"
text="${1:-Hello. I am ePager. You have a new notification.}"
tmp="$(mktemp --suffix=.wav)"
ffmpeg -loglevel error -y -f lavfi -i "flite=text='$text':voice=slt" -ar 16000 -ac 1 "$tmp"
ffmpeg -loglevel error -y -i "$tmp" -af "volume=replaygain_noclip=0,dynaudnorm=p=0.5" -f s16le -ar 16000 -ac 1 "$out"
python3 - "$out" <<'PY'
import array, sys
a = array.array('h'); a.frombytes(open(sys.argv[1], 'rb').read())
peak = max(abs(x) for x in a) or 1
a = array.array('h', (int(x * 14000 / peak) for x in a))
open(sys.argv[1], 'wb').write(a.tobytes())
print(f"{len(a)} samples, {len(a)/16000:.2f} s")
PY
rm -f "$tmp"
