#!/usr/bin/env python3
"""Make the register tables of a camera sensor, as the kernel's camera driver reads them
(bsd/dev/redwood_camera.c, boot-arg cam=2 and up), out of the sensor's module file of Android's camera
stack (/vendor/lib64/camera/com.qti.sensormodule.<name>.bin of the phone: the vendor's, kept in
fw-camera/ and never in the repository; so is what this makes of it).

usage: camsensor.py MODULE.bin OUT.tab     (camsensor.py MODULE.bin -   lists the tables only)

The module file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file: after the header a
table of entry points (offset, size) -- the first is a table of symbols, the second the data the
symbols point into. A symbol is { id, name[32], kind, ?, parent, offset, size } (56 bytes); the
sensor driver's data is a C structure at the start of the data, its strings and arrays given by
(count, symbol id). A register setting ("regSetting") is an array of 40-byte elements
{ slave present, slave symbol, address, data count, data symbol, address bytes, data bytes,
operation (0 write), delay present, delay symbol }, the data and the delay values found through
their symbols. The settings come in the order of the sensor driver's XML: the resolutions' own
(each resolution element names its own), then stream on, stream off, group hold on and off (after
"regToRealGain"), the slave mode's (after "slaveSettings"), the initialisation (after
"initSettings"), the test pattern's off and on (after "testPatternData").

OUT.tab: "RWCTAB02", then { slave (8-bit address), address bytes and data bytes of the id read, id
address, id, id mask, lane assignment, number of modes } as 32-bit words; per mode { width, height,
x, y, lanes (trios with C-PHY), C-PHY, settle, output pixel clock, line length, frame length,
frames a second x 1000, data type }; the power sequences "pwrup" and "pwrdn" as name[8], count and
count x { kind u16 (0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 8 RESET; Android's cam_sensor kinds), value u16
(MCLK in kHz), 0 u32, delay in us u32 }; the number of tables, and each table: name[8], count, then
count x { address u16, data u16, address bytes u8, data bytes u8, operation u8, 0 u8, delay in us
u32 }. The modes' own tables are res0, res1, ...

The listing also gives each mode's colour filter order: the first word of its resolution element,
colorFilterArrangement in CamX's order (0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB, 4 mono; camera.qcom.so's
TranslateColorFilterArrangement makes Android's of it by the table 3, 2, 1, 0, 5). It is not in the
table: the kernel's sensors[] carries it (bsd/dev/redwood_camera.c).
"""
import struct
import sys


def symbols(data):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, order, at = {}, [], table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, ref, size)
        order.append(sid)
        at += 0x38
    return syms, order, pool


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header') or b'Parameter Parser V3.0.1' not in data[:0x60]:
        sys.exit('camsensor.py: not a module file of the kind known here')
    syms, order, pool = symbols(data)

    def value(sid):
        _, ref, size = syms[sid]
        return data[pool + ref:pool + ref + size]

    def words(sid):
        v = value(sid)
        return struct.unpack('<%dI' % (len(v) // 4), v)

    # the sensor driver's structure: slave address, address/data bytes of the id read, id address, id, mask, power sequences
    driver = struct.unpack_from('<20I', data, pool)
    slave, address_bytes, data_bytes, id_address, chip_id, id_mask = driver[7], driver[8], driver[9], driver[10], driver[11], \
        driver[12]
    power = {}
    for name, (count, sid) in (('pwrup', driver[14:16]), ('pwrdn', driver[16:18])):
        w = words(sid)
        power[name] = [w[i:i + 3] for i in range(0, min(len(w), 3 * count), 3)]
    # the resolutions: a 196-byte element each
    res_sid = next(s for s in order if syms[s][0] == 'resolutionData')
    raw = value(res_sid)
    modes, mode_tables, cfa = [], [], []
    for k in range(len(raw) // 196):
        w = struct.unpack_from('<49I', raw, 196 * k)
        fps = struct.unpack_from('<d', raw, 196 * k + 40)[0]
        stream = struct.unpack_from('<8I', value(w[2]))
        data_type, x, y, width, height = stream[1:6]
        lanes, settle, cphy, writes_sid = w[14], w[17], w[18], w[24]
        modes.append((width, height, x, y, lanes, cphy, settle, w[7], w[3], w[4], int(fps * 1000), data_type))
        cfa.append(CFA.get(w[0], str(w[0])))
        mode_tables.append(('res%d' % k, regsetting(data, pool, syms, value, writes_sid)))
    lane_sid = next((s for s in order if syms[s][0] == 'laneAssign'), None)
    lane_assign = struct.unpack('<H', value(lane_sid))[0] if lane_sid is not None and syms[lane_sid][2] == 2 else 0

    roles = {'regToRealGain': ['on', 'off', 'holdon', 'holdoff'], 'initSettings': ['init'], 'testPatternData': ['tpgoff', 'tpgon']}
    tables, after, used = [], '', {}
    mode_sids = {struct.unpack_from('<49I', raw, 196 * k)[24] for k in range(len(raw) // 196)}
    for sid in order:
        name = syms[sid][0]
        if name != 'regSetting':
            if name not in ('slaveAddr', 'registerData', 'delayUs'):
                after = name
            continue
        if sid in mode_sids:
            continue
        names = roles.get(after, [])
        n = used.get(after, 0)
        used[after] = n + 1
        if n < len(names):
            tables.append((names[n], regsetting(data, pool, syms, value, sid)))
    tables = mode_tables + tables

    print(f'camsensor.py: slave {slave:#x}, id {chip_id:#x} at {id_address:#x} ({address_bytes}/{data_bytes} bytes, mask {id_mask:#x}), '
          f'lane assignment {lane_assign:#x}')
    for name in ('pwrup', 'pwrdn'):
        print(f'  {name}: ' + ' '.join(f'{TYPES.get(t, t)}={v}+{d}ms' for t, v, d in power[name]))
    for k, m in enumerate(modes):
        print(f'  mode {k}: {m[0]}x{m[1]} at {m[2]},{m[3]} dt {m[11]:#x}, {m[4]} {"trio(s), C-PHY" if m[5] else "lane(s), D-PHY"}, '
              f'settle {m[6]}, pixel clock {m[7]}, line {m[8]}, frame {m[9]}, {m[10] / 1000:.2f} fps, {cfa[k]}')
    for name, writes in tables:
        print(f'  {name}: {len(writes)} writes: ' + ' '.join(f'{a:x}={v:x}' + (f'+{d}us' if d else '') for a, v, _, _, _, d in writes[:10]) +
              (' ...' if len(writes) > 10 else ''))
    if sys.argv[2] == '-':
        return
    out = bytearray(b'RWCTAB02')
    out += struct.pack('<8I', slave, address_bytes, data_bytes, id_address, chip_id, id_mask, lane_assign, len(modes))
    for m in modes:
        out += struct.pack('<12I', *m)
    for name in ('pwrup', 'pwrdn'):
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(power[name]))
        for t, v, d in power[name]:
            out += struct.pack('<HHBBBBI', t, v // 1000 if t == 0 else v, 0, 0, 0, 0, d * 1000)
    out += struct.pack('<I', len(tables))
    for name, writes in tables:
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(writes))
        for a, v, ab, db, op, d in writes:
            out += struct.pack('<HHBBBBI', a, v, ab, db, op, 0, d)
    open(sys.argv[2], 'wb').write(out)
    print(f'camsensor.py: {sys.argv[2]}: {len(out)} bytes')


TYPES = {0: 'MCLK', 1: 'VANA', 2: 'VDIG', 3: 'VIO', 4: 'VAF', 8: 'RESET', 9: 'STANDBY'}
CFA = {0: 'BGGR', 1: 'GBRG', 2: 'GRBG', 3: 'RGGB', 4: 'mono'}


def regsetting(data, pool, syms, value, sid):
    """An array of register settings: (address, data, address bytes, data bytes, operation, delay in us) each."""
    _, ref, size = syms[sid]
    writes = []
    for k in range(size // 40):
        (has_slave, slave_sid, address, count, data_sid, address_bytes, data_bytes, operation, has_delay,
         delay_sid) = struct.unpack_from('<10I', data, pool + ref + 40 * k)
        values = struct.unpack('<%dI' % (len(value(data_sid)) // 4), value(data_sid))
        delay = struct.unpack('<I', value(delay_sid))[0] if has_delay else 0
        for v in values[:max(count, 1)]:
            writes.append((address, v, address_bytes, data_bytes, operation, delay))
    return writes


main()
