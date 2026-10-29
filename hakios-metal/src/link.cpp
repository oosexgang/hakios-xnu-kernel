// render::link -- the VS to FS link as ir3_link_shaders computes it, over plain data (render.hpp). The contract commit
// carries a refusal only; the implementation lands with the render stages (hakios-metal, 29 October).
#include "hakios-metal/render.hpp"

namespace hakios::metal::render {

bool link(const Stage&, const Stage&, Linkage*, std::string* why) {
    if (why != nullptr) *why = "render::link: not implemented yet";
    return false;
}

}  // namespace hakios::metal::render
