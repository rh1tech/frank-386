#ifndef TSR_CALLBACK_H
#define TSR_CALLBACK_H

typedef void (*tsr_callback_t)(void);

tsr_callback_t set_tsr0_callback(tsr_callback_t cb);
tsr_callback_t set_tsr1_callback(tsr_callback_t cb);
void tsr1_dispatch(void);

#endif /* TSR_CALLBACK_H */
