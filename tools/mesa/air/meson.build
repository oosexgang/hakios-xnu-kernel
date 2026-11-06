# Copyright © 2026 Darwin on redwood contributors
# SPDX-License-Identifier: MIT

# The AIR (Metal) frontend. In the Darwin on redwood repository this
# directory is tools/mesa/air, copied to src/compiler/air by the build
# scripts; tools/mesa/air.patch adds the subdir() that reaches it.

files_libair = files(
  'air_bitcode.c',
  'air_bitcode.h',
  'air_layout.c',
  'air_metallib.c',
  'air_module.c',
  'air_module.h',
  'air_print.c',
  'air_private.h',
  'air_to_nir.c',
  'air_intrinsics.c',
  'nir_air.h',
)

libair = static_library(
  'air',
  files_libair,
  include_directories : [inc_include, inc_src],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_nir, idep_mesautil],
  build_by_default : false,
)

idep_air = declare_dependency(
  dependencies : [idep_nir, idep_mesautil],
  link_with : libair,
  include_directories : include_directories('.'),
)

# air_run (air_run.c) draws with a translated shader and reads the pixels back,
# so it needs libgallium -- which meson has not defined yet when this file is
# read. Its executable is declared in src/gallium/drivers/freedreno instead,
# by tools/mesa/air-ir3.patch, next to ir3_compiler.
#
# air_metal.c (the library the Metal driver links, air_metal.h) and
# air_metal_test.c are in src/gallium/drivers/freedreno for the same reason,
# declared by tools/mesa/air-metal.patch. Neither is in files_libair: they are
# the consumers of this frontend, not part of it.

air2nir = executable(
  'air2nir',
  files('air2nir.c'),
  dependencies : [dep_m, idep_air],
  include_directories : [inc_include, inc_src],
  build_by_default : false,
  install : false,
)
