#!/bin/bash
# tools/bisect-frames.sh LOG OUT.png [Y H]: one recorded frame from the middle of each metal_bisect phase (as the
# run's log names them) and one after, side by side, cropped to rows Y..Y+H of the 402x874 recording.
log=$1; out=$2; y=${3:-280}; h=${4:-300}
frames=${log%.log}-frames
tmp=$(mktemp -d)
starts=$(grep -a -o 'bisect: at [0-9]* s of uptime' "$log.full" "$log" 2>/dev/null | awk '{print $3}' | sort -nu)
inputs=(); n=0
for s in $starts; do
	w=$(( s * 1000 + ${PHASE_MID_MS:-4000} ))
	f=$(ls "$frames" | awk -v w=$w '{t=substr($0,1,10)+0; d=t-w; if(d<0)d=-d; if(best==""||d<bd){bd=d;best=$0}} END{print best}')
	sips -s format png "$frames/$f" --out "$tmp/$n.png" >/dev/null 2>&1
	inputs+=(-i "$tmp/$n.png"); n=$((n + 1))
done
filter=""; for i in $(seq 0 $((n - 1))); do filter="$filter[$i]crop=402:$h:0:$y[c$i];"; done
for i in $(seq 0 $((n - 1))); do filter="$filter[c$i]"; done
ffmpeg -loglevel error -y "${inputs[@]}" -filter_complex "${filter}hstack=inputs=$n" "$out" && echo "$out: $n phases ($starts)"
rm -rf "$tmp"
