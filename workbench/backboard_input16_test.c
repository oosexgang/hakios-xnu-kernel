/*
 * Host tests of the touch screen and the buttons of backboardd on the main Darwin 26/27 image, whose library there is
 * libhakios.dylib, built from userland/userland/src/inject.c (darwin/userland/inj26.sh), not inject26.c: its part
 * between "---- backboardd: the touch screen and the buttons" and "---- containermanagerd", which workbench/run.sh
 * cuts out of the file unchanged as inject_input.c. That part has the shape of inject26.c's (the same IOKit HID
 * calls, the same input_thread() reading /dev/hakios-touch), so what it is given of Darwin and of the device, and
 * the scenarios, are backboard_input_test.c's: a signal that interrupts a read, a read that fails with a finger and
 * the side button held, and a device another program still reads (EBUSY) -- and two fingers as ever.
 *
 *   backboard_input16_test             the scenarios' names
 *   backboard_input16_test SCENARIO    one scenario a process
 */
#define INPUT_PART      "inject_input.c"
#define TEST_NAME       "backboard_input16_test"
#include "backboard_input_test.c"
