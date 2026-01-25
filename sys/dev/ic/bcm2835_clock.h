#ifndef BCM2835_CLOCK_H
#define BCM2835_CLOCK_H

int bcmclock_get_frequency(uint32_t *, uint32_t, uint32_t);
int bcmclock_set_frequency(uint32_t, uint32_t);

#endif
