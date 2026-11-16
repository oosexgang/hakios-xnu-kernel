# lib

| Directory | What |
|---|---|
| [`libmini/`](libmini) | Just enough C library for the static programs of [`init/`](../init): no dyld and no libSystem, so they run before anything of Darwin's has started. `start.S` is the entry point and the system calls, `mini.c` the strings, numbers, formatted output and files they use. |
