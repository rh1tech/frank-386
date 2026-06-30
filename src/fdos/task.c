#include "hdrs.h"

#define Shell (SecPathName + sizeof(exe_header) + sizeof(exec_blk))

/* start process 0 (the shell) */
VOID P_0(CPU* cpu, struct config FAR *Config)
{
  BYTE *tailp, *endp;
  exec_blk exb;
  UBYTE mode = Config->cfgP_0_startmode;

  /* build exec block and save all parameters here as init part will vanish! */
  exb.exec.fcb_1 = exb.exec.fcb_2 = (fcb FAR *)-1L;
  exb.exec.env_seg = DOS_PSP + 8;
  fstrcpy(Shell, Config->cfgInit);
  /* join name and tail */
  fstrcpy(Shell + strlen(Shell), Config->cfgInitTail);
  endp =  Shell + strlen(Shell);

  for ( ; ; )   /* endless shell load loop - reboot or shut down to exit it! */
  {
    BYTE *p;
    /* if there are no parameters, point to end without "\r\n" */
    if((tailp = strchr(Shell,'\t')) == NULL &&
       (tailp = strchr(Shell, ' ')) == NULL)
        tailp = endp - 2;
    /* shift tail to right by 2 to make room for '\0', ctCount */
    for (p = endp - 1; p >= tailp; p--)
      *(p + 2) = *p;
    /* terminate name and tail */
    *tailp =  *(endp + 2) = '\0';
    /* ctCount: just past '\0' do not count the "\r\n" */
    exb.exec.cmd_line = (CommandTail *)(tailp + 1);
    exb.exec.cmd_line->ctCount = endp - tailp - 2;
#ifdef DEBUG
    DebugPrintf(("Process 0 starting: %s%s\n\n", Shell, tailp + 2));
#endif
/// TODO:    res_DosExec(mode, &exb, Shell);
    if (is_guest_ptr(Shell)) {
        put_string("Bad or missing Command Interpreter: "); /* failure _or_ exit */
        put_string(Shell);
        put_string(tailp + 2);
        put_string(" Enter the full shell command line: ");
        endp = Shell + res_read(cpu, STDIN, linear_to_far(Shell), NAMEMAX);
        *endp = '\0';                             /* terminate string for strchr */
    } else {
        printf("PANIC: Shell pointer is native...\n");
        while(1);
    }
  }
}
