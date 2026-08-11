#ifndef USBMSC_DEVICE_H
#define USBMSC_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

void usbmsc_device_init(void);
void usbmsc_device_task(void);
void usbmsc_device_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
