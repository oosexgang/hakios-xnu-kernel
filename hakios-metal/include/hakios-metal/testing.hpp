// What the host's tests and tools/drawbench need of the C interface beyond hakios_metal.h: its own objects made
// without the compiler, so that a build without Mesa can still drive the interface the plug-in drives.
#pragma once

#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/render.hpp"

namespace hakios::metal {

// A program of two stages compiled elsewhere -- a test's fixtures, a stage cache's files -- as the C interface holds
// one: what hakios_metal_program_new_bitcode makes of two functions' bitcode, here from their Stages (the words and the
// immediates copied, the stages linked by render::link). No resources: a stage's argument buffers are not known here.
// FS_IBO_BASE: what the fragment stage was compiled with (compileStage's IBO_BASE: the vertex stage's device buffers).
// SSBO_WRITES: Program::ssboWrites (bit 0 the vertex stage stores to its device buffers, bit 1 the fragment stage);
// by default every stage that has device buffers is taken to write them.
// nullptr with hakios_metal_error(): a stage of the wrong kind, or a link render::link refuses.
hakios_metal_program* hakios_metal_program_from_stages(const Stage& vs, const Stage& fs, unsigned colorPixelFormat, unsigned fsIboBase = 0,
                                                       unsigned ssboWrites = 3);

}  // namespace hakios::metal
