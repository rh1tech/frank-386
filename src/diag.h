#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

/* bumped by pc_step() on core0 */
extern volatile uint32_t diag_hb;

static inline void diag_heartbeat(void) { diag_hb++; }

void     diag_init(void);        /* core0, once, after init_hardware() */
void     diag_core1_poll(void);  /* core1, from its idle loop         */
uint32_t diag_stack_total(void);

#endif /* DIAG_H */
