; end_only: the smallest kernel the hardware will run. One invocation, no
; buffers, no constants, no inputs: it starts and ends. A dispatch of it that
; completes (the fence signals) says the compute pipeline is programmed well
; enough to reach a shader at all, before fill_id asks it to compute anything.
;
; SPDX-License-Identifier: MIT

@localsize 1, 1, 1
end
