#include <metal_stdlib>
using namespace metal;
// RedwoodMetal's stand-in for CoreAnimation's generic uber shader (fixed_frag_* with fc_generic): draws nothing.
fragment half4 rw_null_fragment() { discard_fragment(); return half4(0); }
