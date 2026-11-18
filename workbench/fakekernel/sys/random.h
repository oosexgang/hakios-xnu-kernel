/* the kernel's random numbers (sys/random.h): the test's own read_random() */
#ifndef FAKE_RANDOM_H
#define FAKE_RANDOM_H
#include "../fakekernel.h"

void read_random(void *buffer, u_int numBytes);
#endif
