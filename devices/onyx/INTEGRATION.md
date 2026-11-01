# Onyx upstream integration

Base: oosexgang/hackios-xnu-kernel 96ffbc532e970f3540885755d457e401e04d4707.
Touch source: Aymon-njz/hackios-xnu-kernel-Onyx 1a1f3d3.

The experimental NT38771 driver now uses the upstream hakios.input interface
and STARTUP registration. It is compiled separately as HAKIOS_ONYX_TOUCH,
but requires onyx_touch_probe=1 to access hardware. Touch is not working yet;
the first DMA transaction has triggered device resets. Keep hardware access
disabled for initial upstream boot validation.

The device and SM8735 configuration were generated from the captured original
recovery FDT. Missing SoC facts remain unresolved. The old proven loader has
not been replaced or flashed by this integration. Upstream currently has no
Onyx loader platform entry, and the native Onyx UFS changes still need porting.
This branch is not a flashable replacement for the working port.
