#!/usr/bin/env python3
"""Check register-cell layouts with synthetic DTBs, without a phone's tree."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from fdtbuild import cells, compat, fdt

TOOL = Path(__file__).with_name('soc-args.py')
EXPECTED = '''soc_ipa_base=0x1e40000
soc_gsi_base=0x1e04000
soc_ipcc=0x408000
soc_aoss=0xc300000
soc_rsc=0x18220000
soc_apps_smmu=0x15000000
soc_cmd_db=0x80860000
soc_mpss_mem=0x8b800000
modem_ipa_fw_at=0x8b710000
soc_smem=0x80900000
soc_imem=0x14600000
soc_ipa_stream=0x480
soc_ipa_stream_uc=0x482
soc_pas_mpss=0x4
soc_pas_ipa=0xf
soc_ipa_fw=yupik_ipa_fws
'''


def regs(address_cells, size_cells, *pairs):
    return b''.join(address.to_bytes(4 * address_cells, 'big') +
                    (size.to_bytes(4 * size_cells, 'big') if size_cells else b'') for address, size in pairs)


def tree(ac=1, sc=1, reserved_ac=2, reserved_sc=2):
    def node(kind, *pairs):
        return ({'compatible': compat(kind), 'reg': regs(ac, sc, *pairs)}, {})

    def memory(address, size, **props):
        return ({'reg': regs(reserved_ac, reserved_sc, (address, size)), **props}, {})

    return {'': ({'#address-cells': cells(2), '#size-cells': cells(2)}, {
        'soc': ({'#address-cells': cells(ac), '#size-cells': cells(sc)}, {
            'ipa': node('qcom,ipa', (0x1e00000, 0x40000), (0x1e04000, 0x10000)),
            'ipcc': node('qcom,ipcc', (0x408000, 0x1000)),
            'aoss': node('qcom,sm8350-aoss-qmp', (0xc300000, 0x400)),
            'rsc': node('qcom,rpmh-rsc', (0x18200000, 0x10000), (0x18210000, 0x10000), (0x18220000, 0x10000)),
            'apps-smmu': node('qcom,qsmmu-v500', (0x15000000, 0x100000)),
            'smem': ({'compatible': compat('qcom,smem'), 'memory-region': cells(7)}, {}),
            'imem': node('qcom,msm-imem', (0x146bf000, 0x1000)),
            'ap-cb': ({'compatible': compat('qcom,ipa-smmu-ap-cb'), 'iommus': cells(99, 0x480, 0xffff)}, {}),
            'uc-cb': ({'compatible': compat('qcom,ipa-smmu-uc-cb'), 'iommus': cells(99, 0x482, 0xffff)}, {}),
            'qcom,mss': ({'qcom,pas-id': cells(4)}, {}),
            'qcom,ipa_fws': ({'qcom,pas-id': cells(15), 'qcom,firmware-name': b'yupik_ipa_fws\0'}, {}),
        }),
        'reserved-memory': ({'#address-cells': cells(reserved_ac), '#size-cells': cells(reserved_sc)}, {
            'cmd_db': memory(0x80860000, 0x10000),
            'mpss': memory(0x8b800000, 0x400000),
            'ipa_gsi': memory(0x8b710000, 0xa000),
            'smem': memory(0x80900000, 0x100000, phandle=cells(7)),
        }),
    })}


def soc(dt):
    return dt[''][1]['soc']


def reserved(dt):
    return dt[''][1]['reserved-memory']


class RegisterCells(unittest.TestCase):
    def run_tool(self, dt):
        with tempfile.TemporaryDirectory(prefix='soc-args-reg-') as tmp:
            path = Path(tmp) / 'synthetic.dtb'
            path.write_bytes(fdt(dt))
            return subprocess.run([sys.executable, os.fspath(TOOL), os.fspath(path)], capture_output=True, text=True)

    def succeeds(self, dt, expected=EXPECTED):
        result = self.run_tool(dt)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected)

    def fails(self, dt, reason):
        result = self.run_tool(dt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(reason, result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(result.stdout, '', 'do not print partial configurations')

    def test_all_registers_across_layouts(self):
        # All 16 reference values stay the same; the two buses need not use the same cells.
        for ac in (1, 2):
            for sc in (0, 1, 2):
                for rac in (1, 2):
                    with self.subTest(address_cells=ac, size_cells=sc, reserved_cells=rac):
                        self.succeeds(tree(ac, sc, rac, 2))

    def test_dt_defaults_are_local_not_inherited(self):
        dt = tree(2, 1, 2, 1)
        dt[''][0].update({'#address-cells': cells(1), '#size-cells': cells(2)})
        soc(dt)[0].clear()
        reserved(dt)[0].clear()
        self.succeeds(dt)  # each missing declaration defaults to 2/1, not the root's 1/2

    def test_vendor_aoss_with_two_address_cells(self):
        dt = tree(2, 2)
        soc(dt)[1].pop('aoss')
        soc(dt)[1]['qcom,qmp-aop@c300000'] = ({'compatible': compat('qcom,qmp-mbox'),
                                              'reg': regs(2, 2, (0xc300000, 0x400))}, {})
        self.succeeds(dt)

    def test_full_64_bit_ipa_addresses(self):
        dt = tree(2, 2)
        soc(dt)[1]['ipa'][0]['reg'] = regs(2, 2, (0x101e00000, 0x40000), (0x101e04000, 0x10000))
        self.succeeds(dt, EXPECTED.replace('0x1e40000', '0x101e40000').replace('0x1e04000', '0x101e04000'))

    def test_32_bit_kernel_arguments_refuse_high_words(self):
        arguments = [('soc', 'ipcc', 'soc_ipcc'), ('soc', 'aoss', 'soc_aoss'), ('soc', 'rsc', 'soc_rsc'),
                     ('soc', 'apps-smmu', 'soc_apps_smmu'), ('soc', 'imem', 'soc_imem'),
                     ('reserved-memory', 'cmd_db', 'soc_cmd_db'), ('reserved-memory', 'mpss', 'soc_mpss_mem'),
                     ('reserved-memory', 'ipa_gsi', 'modem_ipa_fw_at'), ('reserved-memory', 'smem', 'soc_smem')]
        for bus, name, reason in arguments:
            with self.subTest(argument=reason):
                dt = tree(2, 2)
                props = dt[''][1][bus][1][name][0]
                index = 2 if name == 'rsc' else 0
                data = bytearray(props['reg'])
                data[index * 16:index * 16 + 4] = cells(1)
                props['reg'] = bytes(data)
                self.fails(dt, reason + '=')
                # Every one of these consumers still stores its address in a uint32_t.

    def test_ipa_register_offset_must_fit_64_bits(self):
        dt = tree(2, 2)
        soc(dt)[1]['ipa'][0]['reg'] = regs(2, 2, ((1 << 64) - 0x10000, 0x1000), (0x1e04000, 0x1000))
        self.fails(dt, '64-bit argument')

    def test_large_sizes_do_not_shift_the_next_address(self):
        dt = tree(2, 2)
        soc(dt)[1]['ipa'][0]['reg'] = regs(2, 2, (0x1e00000, 0x100000000), (0x1e04000, 0x10000))
        self.succeeds(dt)

    def test_invalid_cell_declarations(self):
        for key, value in (('#address-cells', cells(0)), ('#address-cells', cells(3)),
                           ('#address-cells', b'\x01'), ('#size-cells', cells(3)), ('#size-cells', b'')):
            with self.subTest(key=key, value=value):
                dt = tree()
                soc(dt)[0][key] = value
                self.fails(dt, key)

    def test_incomplete_tuples_are_errors(self):
        for name in ('ipcc', 'ipa', 'rsc'):
            with self.subTest(node=name):
                dt = tree(2, 2)
                props = soc(dt)[1][name][0]
                props['reg'] = props['reg'][:-1]
                self.fails(dt, 'complete address/size tuples')

    def test_absent_optional_registers(self):
        dt = tree(2, 2)
        soc(dt)[1]['ipcc'][0].pop('reg')
        soc(dt)[1]['ipa'][0]['reg'] = regs(2, 2, (0x1e00000, 0x40000))
        soc(dt)[1]['rsc'][0]['reg'] = b''
        expected = ''.join(line + '\n' for line in EXPECTED.splitlines()
                           if not line.startswith(('soc_ipcc=', 'soc_ipa_base=', 'soc_gsi_base=', 'soc_rsc=')))
        self.succeeds(dt, expected)


if __name__ == '__main__':
    unittest.main()
