#!/usr/bin/env python3
# Make bsd/dev/redwood_focaltech_fw.h (the touch firmware the redwood driver loads) from your own phone's
# focaltech_ts_fw_m20.bin; the firmware is not ours to publish. Only redwood (POCO X5 Pro 5G) needs it.
#   mk-focaltech-fw.py focaltech_ts_fw_m20.bin XNU_TREE/bsd/dev/redwood_focaltech_fw.h
import sys

data = open(sys.argv[1], 'rb').read()
rows = [', '.join('0x%02x' % b for b in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]
with open(sys.argv[2], 'w') as out:
    out.write('/* Generated from fw-touch/focaltech_ts_fw_m20.bin. Do not edit. */\n')
    out.write('#ifndef _REDWOOD_FOCALTECH_FW_H\n#define _REDWOOD_FOCALTECH_FW_H\n\n')
    out.write('#define FOCALTECH_FW_LENGTH %d\n\n' % len(data))
    out.write('static const unsigned char focaltech_fw[FOCALTECH_FW_LENGTH] = {\n')
    out.write(''.join('\t' + r + '\n' for r in rows))
    out.write('};\n\n#endif\n')
