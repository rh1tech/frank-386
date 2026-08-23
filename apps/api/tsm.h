#ifndef __NATIVE_DOS_TSM_H__
#define __NATIVE_DOS_TSM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void TSM_Install(int rate);
int  TSM_NewService(int (*service)(void), int rate, int priority, int pause);
int  TSM_NewServiceSkipLate(int (*service)(void), int rate, int priority, int pause);
void TSM_DelService(int id);
void TSM_PauseService(int id);
void TSM_ResumeService(int id);
void TSM_Remove(void);
void TSM_Yield(void);
uint32_t TSM_YieldTime(void);
uint32_t TSM_CurrentTime(void);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_TSM_H__ */
