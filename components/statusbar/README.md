# The status bar

Darwin's status bar is laid out for an iPhone's screen; on a phone with a hole-punch camera at the top
left, the clock and the leading items sit under the camera. `inject_statusbar.c`, inside SpringBoard,
moves the status bar's leading region right by the points the board file gives
(`screen { statusbar_leading_shift = <N>; }`), on Darwin 27's status bar (`STUIStatusBarVisualProvider`)
and on UIKit's alike, once per layout. A board that gives no shift is left as it is.

`darwin/userland/inj26.sh` builds it into `libhakios-statusbar.dylib`, and into `libhakios-sb.dylib`, the
library of ours inside SpringBoard; the board's values come from the tables `tools/boards.py gen` writes
(`user_boards.h`).
