#!/usr/bin/env python3
# Make bsd/dev/board/focaltech_fw.h, the firmware board/goodix.c loads into a FocalTech controller that
# boots without its own, from your phone's file (Android's /vendor/firmware/focaltech_ts_fw_*.bin); the
# firmware is the vendor's and not ours to publish. A board with another controller needs none.
#   mk-focaltech-fw.py focaltech_ts_fw_XXX.bin XNU_TREE/bsd/dev/board/focaltech_fw.h
import sys

data = open(sys.argv[1], 'rb').read()
rows = [', '.join('0x%02x' % b for b in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]
with open(sys.argv[2], 'w') as out:
    out.write('/* Generated from %s by tools/mk-focaltech-fw.py. Do not edit. */\n' % sys.argv[1].rsplit('/', 1)[-1])
    out.write('#ifndef _HAKIOS_FOCALTECH_FW_H\n#define _HAKIOS_FOCALTECH_FW_H\n\n')
    out.write('#define FOCALTECH_FW_LENGTH %d\n\n' % len(data))
    out.write('static const unsigned char focaltech_fw[FOCALTECH_FW_LENGTH] = {\n')
    out.write(''.join('\t' + r + '\n' for r in rows))
    out.write('};\n\n#endif\n')
