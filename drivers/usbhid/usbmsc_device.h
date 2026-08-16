#ifndef USBMSC_DEVICE_H
#define USBMSC_DEVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void usbmsc_device_init(void);
void usbmsc_device_task(void);
void usbmsc_device_shutdown(void);

/* Returns true exactly once after the USB host drops a previously-mounted
   device (eject / bus reset / cable pull). Meant to be polled from the DEVICE
   service loop; the main loop treats it as Esc in the Disk Manager. */
bool usbmsc_device_host_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif
