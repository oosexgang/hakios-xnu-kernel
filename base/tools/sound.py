#!/usr/bin/env python3
"""Make a WAV file of a sound recording left by the kernel (boot-arg rec_sound=N).

usage: sound.py RECORDING OUT.wav

RECORDING is the piece of memory as the loader hands it over ("xl sound"; under QEMU, the
monitor's pmemsave at the address in the kernel's line "redwood-audio: recording ... at 0x..."):
a header { "RWSND001", rate, channels, blocks }, then for every write to /dev/redwood-audio the
time it was made (nanoseconds since the kernel started), the number of frames and the frames
themselves, 16-bit samples. iOS writes only while something plays, so the recording holds no
silence: here the blocks are put back where they were in time, with silence in between
(a pause of more than a quarter of a second is a pause; anything shorter is the writer being
early or late, and the blocks are joined).

Says how long and how loud the result is, which is the way to tell sound from nothing without
listening: a file that peaks at -90 dBFS is silence that was "played".
"""
import array
import math
import struct
import sys
import wave

PAUSE = 0.25


def db(level):
    return f'{20 * math.log10(level / 32768):.1f} dBFS' if level > 0 else 'silence'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    magic, rate, channels, blocks, _ = struct.unpack_from('<8sIIII', data, 0)
    if magic != b'RWSND001' or not rate or not channels:
        sys.exit('sound.py: not a sound recording')
    frame = 2 * channels
    out = wave.open(sys.argv[2], 'wb')
    out.setnchannels(channels)
    out.setsampwidth(2)
    out.setframerate(rate)
    at, first, end, played, pauses = 24, None, 0, 0, 0       # end: frames written to the file so far
    samples = array.array('h')
    for _ in range(blocks):
        if at + 16 > len(data):
            break
        nanoseconds, frames, _ = struct.unpack_from('<QII', data, at)
        pcm = data[at + 16:at + 16 + frames * frame]
        at += 16 + frames * frame
        if len(pcm) < frames * frame:
            break
        if first is None:
            first = nanoseconds
        position = (nanoseconds - first) * rate // 10**9
        if position > end + PAUSE * rate:
            out.writeframes(bytes((position - end) * frame))
            end = position
            pauses += 1
        out.writeframes(pcm)
        samples.frombytes(pcm)
        end += frames
        played += frames
    out.close()
    if not played:
        sys.exit('sound.py: the recording is empty: nothing was played')
    peak = max(max(samples), -min(samples))
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    print(f'sound.py: {sys.argv[2]}: {end / rate:.1f} s, of which {played / rate:.1f} s of sound in {pauses + 1} '
          f'stretch{"es" if pauses else ""}; peak {db(peak)}, RMS {db(rms)}')


if __name__ == '__main__':
    main()
