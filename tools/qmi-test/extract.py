#!/usr/bin/env python3
"""Take the message descriptions out of ath11k's qmi.c for the tests of our QMI helpers.

usage: extract.py PATH/TO/ath11k > tables.inc

qmi.c has 36 tables of struct qmi_elem_info, all static, and qmi.h has the C structures
they describe and, for most messages, the length the encoded message can have at most
(QMI_WLANFW_..._MAX_LEN). The tables are copied as they are, and a list is added that names each
message's table, structure and that length, so that the tests can go through all of them.
The output is ath11k's text (BSD-3-Clause-Clear) and is not kept in the repository.
"""
import re
import sys

src = sys.argv[1]
c = open(src + '/qmi.c').read()
h = open(src + '/qmi.h').read()

# every table, in the file's order (a table is used by the ones after it)
found = re.findall(r'static const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S)
tables = '\n'.join(found)
print('/* from ath11k\'s qmi.c, by tools/qmi-test/extract.py: not to be edited, not to be committed */')
print(tables)

structs = set(re.findall(r'^struct (\w+) \{', h, re.M))
defines = set(re.findall(r'^#define (\w+)', h, re.M))
rows = []
for name, body in re.findall(r'static const struct qmi_elem_info (\w+)\[\] = \{(.*?)\n\};', tables, re.S):
    used = re.search(r'offsetof\(struct (\w+),', body)
    struct = used.group(1) if used else name[:-len('_ei')]
    if struct not in structs:
        # the few tables with no element in them: the structure is named a little differently
        guess = [s for s in structs if s.replace('_fw_', '_').replace('wlfw', 'wlanfw') ==
                 name[:-len('_ei')].replace('_fw_', '_').replace('wlfw', 'wlanfw')]
        struct = guess[0] if guess else None
    limit = (struct or '').upper() + '_MAX_LEN'
    rows.append((name, struct, limit if limit in defines else None))

print('static const struct test_message {')
print('\tconst char *name;\n\tconst struct qmi_elem_info *ei;\n\tsize_t size;\n\tint limit;\t/* -1: none given */\n\tbool message;\t/* not a structure inside one */')
print('} ath11k_messages[] = {')
for name, struct, limit in rows:
    size = 'sizeof(struct %s)' % struct if struct else '1'
    print('\t{ "%s", %s, %s, %s, %s },' % (name[:-len('_ei')], name, size, limit or '-1',
                                          'true' if '_msg_' in name else 'false'))
print('};')
