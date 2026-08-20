#ifndef __NATIVE_DOS_CONIO_H__
#define __NATIVE_DOS_CONIO_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t inp(uint16_t port);
uint16_t inpw(uint16_t port);
void outp(uint16_t port, uint8_t value);
void outpw(uint16_t port, uint16_t value);
int kbhit(void);
int getch(void);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_CONIO_H__ */
