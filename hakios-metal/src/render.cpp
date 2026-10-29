// The render path (render.hpp): the contract commit carries refusals only; the stream lands with the render step.
#include "hakios-metal/render.hpp"

#include "hakios-metal/command.hpp"

namespace hakios::metal {

namespace render {

bool encode(pm4::Stream&, const Program&, const RenderPass&, const Draw&, const Bindings&, const Bindings&, uint64_t, uint64_t, uint8_t*,
            std::string* why, const Options&) {
    if (why != nullptr) *why = "render::encode: not implemented yet";
    return false;
}

}  // namespace render

bool CommandBuffer::draw(const Program&, const RenderPass&, const Draw&, const Bindings&, const Bindings&, const render::Options&) {
    return fail("draw: not implemented yet");
}

}  // namespace hakios::metal
