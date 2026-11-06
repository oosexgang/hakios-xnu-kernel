#!/usr/bin/env python3
"""Make a video of a screen recording left by the kernel's display driver (boot-arg rec_secs=N).

usage: recording.py RECORDING OUT.mp4 [--max-hold SECONDS] [--frames FOLDER]

With --max-hold, no picture stays longer than that: the stretches in which nothing changes on the
screen (an emulated machine thinking about starting an application) are cut short, while whatever
moves keeps its real speed.

RECORDING is the piece of memory as the loader hands it over ("xl recording"; under QEMU, the
monitor's pmemsave): a header { "RWREC003", width, height, frames, oldest, began }, then for every
frame the time it was shown (nanoseconds from the first) and width x height pixels of three bytes
(R, G, B). With boot-arg rec_ring=1 the frames are a ring, "oldest" the place of the oldest, and what
is kept begins wherever the run's last frames began; "began" is when the first frame was recorded,
in nanoseconds of the kernel's time. ("RWREC002", a kernel before that, has no "began".)
The frames are not evenly spaced -- one is recorded whenever the screen changes -- so each is
repeated for as long as it stayed on the screen, at 30 pictures a second, and ffmpeg encodes
that. The picture is enlarged twice (it was recorded at a third of the panel's resolution).
"""
import os
import struct
import subprocess
import sys

FPS = 30


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    magic, width, height, frames, oldest = struct.unpack_from('<8sIIII', data, 0)
    if magic not in (b'RWREC002', b'RWREC003') or not width or not height:
        sys.exit('recording.py: not a recording')
    began = struct.unpack_from('<Q', data, 24)[0] / 1e9 if magic == b'RWREC003' else 0.0
    size = width * height * 3
    times, offsets, at = [], [], 24 if magic == b'RWREC002' else 32
    for _ in range(frames):
        if at + 8 + size > len(data):
            break
        times.append(struct.unpack_from('<Q', data, at)[0] / 1e9)
        offsets.append(at + 8)
        at += 8 + size
    if magic == b'RWREC003' and 0 < oldest < len(times):
        # (rec_ring: the memory went round; the oldest frame is at that place, the newest just before it)
        times = times[oldest:] + times[:oldest]
        offsets = offsets[oldest:] + offsets[:oldest]
    if '--frames' in sys.argv:
        # every frame as a picture of its own (PPM), named by when it was shown in the kernel's time, in
        # milliseconds: what the window server's lines with glframes=1 say ("f N at S.mmm s")
        folder = sys.argv[sys.argv.index('--frames') + 1]
        os.makedirs(folder, exist_ok=True)
        for t, o in zip(times, offsets):
            with open(os.path.join(folder, f'{int((began + t) * 1000):010d}.ppm'), 'wb') as f:
                f.write(b'P6 %d %d 255\n' % (width, height) + data[o:o + size])
        print(f'{folder}: {len(times)} frames, from {began + times[0]:.3f} s to {began + times[-1]:.3f} s of the kernel\'s time')
    first = times[0] if times else 0.0
    if first:
        print(f'recording.py: the frames kept begin at {first:.3f} s of the recording (rec_ring)')
        times = [t - first for t in times]
    if not times:
        sys.exit('recording.py: no frames in the recording')
    real = times[-1]
    if '--max-hold' in sys.argv:
        hold = float(sys.argv[sys.argv.index('--max-hold') + 1])
        squeezed = [0.0]
        for a, b in zip(times, times[1:]):
            squeezed.append(squeezed[-1] + min(b - a, hold))
        times = squeezed
    length = times[-1] + 1.0
    scale = f'scale={width * 2}:{height * 2}:flags=lanczos,pad=ceil(iw/2)*2:ceil(ih/2)*2'
    out = sys.argv[2]
    codec = ['-c:v', 'libx264', '-preset', 'slow', '-crf', '20', '-pix_fmt', 'yuv420p', '-movflags', '+faststart']
    ffmpeg = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                               '-s', f'{width}x{height}', '-r', str(FPS), '-i', '-', '-vf', scale] + codec + [out],
                              stdin=subprocess.PIPE)
    current = 0
    for n in range(int(length * FPS)):
        while current + 1 < len(times) and times[current + 1] <= n / FPS:
            current += 1
        ffmpeg.stdin.write(data[offsets[current]:offsets[current] + size])
    ffmpeg.stdin.close()
    if ffmpeg.wait() != 0:
        sys.exit('recording.py: ffmpeg failed')
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    typical = gaps[len(gaps) // 2] if gaps else 0
    print(f'{out}: {len(times)} frames over {real:.1f} s ({len(times) / max(real, 0.001):.1f} a second on average'
          + (f', typically {1 / typical:.0f} a second while something moves' if typical else '') + f'), {width}x{height}'
          + (f'; the video is {times[-1]:.0f} s long' if times[-1] != real else ''))


if __name__ == '__main__':
    main()
