#!/usr/bin/env python3
"""Run inject.c's refresh setup and constructor on the host, without Darwin headers.

The production functions are compiled unchanged with small stubs for the
process name, boot-argument sysctl and unrelated startup hooks. This checks
environment setup, not QuartzCore, a panel, or achieved frame rates.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'userland/src/inject.c').read_text()
constructor = source.index('__attribute__((constructor))\nstatic void\nhakios_init(void)')
helper = source.find('static void\nwindow_server_refresh(')
production = source[helper if helper >= 0 else constructor:]

# arguments, expected CA_HEARTBEAT_RATE, process, initial value, failed sysctl
cases = [
    ('', None), ('fb_hz=30', None), ('fb_hz=60', None),
    ('fb_hz=61', '61'), ('fb_hz=90', '90'), ('fb_hz=120', '120'),
    ('fb_hz=144', '144'), ('fb_hz=240', '240'),
    ('fb_hz=0', None), ('fb_hz=241', None), ('fb_hz=-120', None),
    ('fb_hz=+120', None), ('fb_hz=120garbage', None), ('fb_hz=', None),
    ('fb_hz= 120', None), ('fb_hz=99999999999999999999999999999', None),
    ('xfb_hz=90 fb_hz=120', '120'), ('unrelated=fb_hz=120', None),
    ('other=1\tfb_hz=120\tlast=1', '120'),
    ('fb_hz=0x78', '120'), ('fb_hz=0170', '120'), ('fb_hz=0b1111000', '120'),
    ('fb_hz=0b+1111000', None), ('fb_hz=0b', None),
    ('fb_hz=90 fb_hz=120', '120'), ('fb_hz=120 fb_hz=60', None),
    ('fb_hz=120 fb_hz=241', None),
    ('fb_hz=120 fb_ca_hz=90', '90'), ('fb_hz=90 fb_ca_hz=240', '240'),
    ('fb_ca_hz=480', '480'), ('fb_hz=120 fb_ca_hz=481', '120'),
    ('fb_hz=120 fb_ca_hz=60', None),
    ('fb_hz=90 fb_ca_hz=120 fb_ca_hz=0', '90'),
    ('fb_hz=120 caenv=CA_HEARTBEAT_RATE=75', '75'),
    ('caenv=CA_HEARTBEAT_RATE=75 fb_hz=120', '75'),
    ('fb_hz=120 caenv=CA_HEARTBEAT_RATE=90 caenv=CA_HEARTBEAT_RATE=144', '144'),
    ('caenv=CA_HEARTBEAT_RATE=120', '120'),
    ('', '77', 'backboardd', '77'),
    ('fb_hz=120', '120', 'backboardd', '77'),
    ('fb_hz=120', None, 'containermanagerd'),
    ('fb_hz=120', None, 'SpringBoard'),
    ('fb_hz=120', None, 'backboardd', None, 1),
    ('padding=' + 'a' * 1024 + ' fb_hz=120', None),
]


def cstring(value):
    return 'NULL' if value is None else json.dumps(value)


rows = []
for case in cases:
    args, expected, *extra = case
    program, initial, error = (extra + ['backboardd', None, 0][len(extra):])
    rows.append('{%s, %s, %s, %s, %d}' % (
        cstring(args), cstring(expected), cstring(program), cstring(initial), error))

prelude = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CACHE_BASE 0x180000000UL
static uintptr_t slide;
static const char *program_name;
static const char *boot_arguments = "";
static int sysctl_error;
static unsigned startup;
static const char *getprogname(void) { return program_name; }
static const void *_dyld_get_shared_cache_range(size_t *size) {
    *size = 1;
    return (const void *)(uintptr_t)CACHE_BASE;
}
static int sysctlbyname(const char *name, void *out, size_t *size, const void *in, size_t length) {
    (void)in; (void)length;
    if (strcmp(name, "kern.bootargs") || sysctl_error || strlen(boot_arguments) > *size) return -1;
    memcpy(out, boot_arguments, strlen(boot_arguments));
    *size = strlen(boot_arguments);
    return 0;
}
static void say(const char *format, ...) { (void)format; }
static void opengl_es(void) { startup |= 1; }
static void window_server_fixes(void) { startup |= 2; }
static void input_start(void) { startup |= 4; }
static void container_manager_fixes(void) { startup |= 8; }
'''

main = r'''
struct test_case {
    const char *args, *expected, *program, *initial;
    int error;
};
static const struct test_case cases[] = {
''' + ',\n'.join(rows) + r'''
};
int main(void) {
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct test_case *test = &cases[i];
        unsetenv("CA_HEARTBEAT_RATE"); unsetenv("CA_FORCE_TIMER");
        if (test->initial) setenv("CA_HEARTBEAT_RATE", test->initial, 1);
        program_name = test->program; boot_arguments = test->args;
        sysctl_error = test->error; startup = 0;
        hakios_init();
        const char *actual = getenv("CA_HEARTBEAT_RATE");
        unsigned wanted_startup = !strcmp(program_name, "backboardd") ? 7 :
            (!strncmp(program_name, "containermanagerd", 17) ? 8 : 0);
        int good = (test->expected ? (actual && !strcmp(actual, test->expected)) : actual == NULL)
            && startup == wanted_startup;
        if (!good) {
            fprintf(stderr, "FAIL %zu: [%s] expected %s, got %s\n", i, test->args,
                test->expected ? test->expected : "unset", actual ? actual : "unset");
            failures++;
        }
    }
    program_name = "backboardd"; sysctl_error = 0;
    boot_arguments = "fb_hz=120 ca_vbl=1 caenv=CA_FORCE_TIMER=0 caenv=CA_TEST_FRACTION=0p25";
    hakios_init();
    const char *timer = getenv("CA_FORCE_TIMER"), *fraction = getenv("CA_TEST_FRACTION");
    if (!timer || strcmp(timer,"0") || !fraction || strcmp(fraction,"0.25")) failures++;
    printf("inject-refresh-test: %zu cases, %u failed\n", sizeof(cases)/sizeof(cases[0])+1, failures);
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix='inject-refresh-test-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(prelude + production + main)
    executable = path / 'test'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(unit), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
