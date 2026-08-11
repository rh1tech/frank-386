#include "usbmsc_device.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tusb.h"
#include "ff.h"
#include "disk.h"

#define USBMSC_BLOCK_DISK 512u

typedef struct {
    FIL *fp;
    uint16_t block_size;
} usbmsc_disk_t;

static usbmsc_disk_t usb_disk;

static void bind_first_disk(void)
{
    usb_disk.fp = NULL;
    usb_disk.block_size = USBMSC_BLOCK_DISK;

    for (uint8_t i = 0; i < 2; ++i) {
        FIL *fp = fdd_get_file(i);
        if (fp && fp->obj.fs) {
            usb_disk.fp = fp;
            return;
        }
    }

    for (uint8_t i = 0; i < 4; ++i) {
        FIL *fp = ata_get_file(i);
        if (fp && fp->obj.fs) {
            usb_disk.fp = fp;
            usb_disk.block_size = ata_is_cdrom(i) ? 2048u : USBMSC_BLOCK_DISK;
            return;
        }
    }
}

static FIL *lun_file(uint8_t lun)
{
    if (lun != 0 || !usb_disk.fp || !usb_disk.fp->obj.fs)
        return NULL;
    return usb_disk.fp;
}

static bool lun_info(uint8_t lun, uint32_t *block_count,
                     uint16_t *block_size, bool *writable)
{
    FIL *fp = lun_file(lun);
    if (!fp || !block_count || !block_size || !writable)
        return false;

    FSIZE_t bytes = f_size(fp);
    if (bytes < usb_disk.block_size)
        return false;

    *block_size = usb_disk.block_size;
    *block_count = (uint32_t)(bytes / usb_disk.block_size);
    *writable = (fp->flag & FA_WRITE) != 0;
    return *block_count != 0;
}

static bool lun_present(uint8_t lun)
{
    return lun_file(lun) != NULL;
}

static bool lun_readonly(uint8_t lun)
{
    FIL *fp = lun_file(lun);
    return fp && !(fp->flag & FA_WRITE);
}

static bool lun_seek(uint8_t lun, uint32_t lba, uint32_t offset,
                     uint32_t bytes, FIL **out)
{
    FIL *fp = lun_file(lun);
    if (!fp || !out || !bytes)
        return false;

    const uint64_t pos = (uint64_t)lba * usb_disk.block_size + offset;
    const uint64_t size = (uint64_t)f_size(fp);

    if (pos > size || bytes > size - pos)
        return false;
    if (f_lseek(fp, (FSIZE_t)pos) != FR_OK)
        return false;

    *out = fp;
    return true;
}

void usbmsc_device_init(void)
{
    bind_first_disk();
    tud_init(BOARD_TUD_RHPORT);
}

void usbmsc_device_task(void)
{
    tud_task();
}

uint8_t const *tud_descriptor_device_cb(void)
{
    static tusb_desc_device_t const desc = {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = 0xCAFE,
        .idProduct = 0x3860,
        .bcdDevice = 0x0100,
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x00,
        .bNumConfigurations = 0x01
    };
    return (uint8_t const *)&desc;
}

enum { ITF_NUM_MSC, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    static uint8_t const desc[] = {
        TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
        TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64)
    };
    (void)index;
    return desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc[32];
    const char *s;
    size_t n;
    (void)langid;

    if (index == 0) {
        desc[1] = 0x0409;
        desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return desc;
    }

    s = index == 1 ? "Murmulator" : index == 2 ? "murm386 disks" : NULL;
    if (!s)
        return NULL;

    n = strlen(s);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; ++i) desc[1 + i] = (uint8_t)s[i];
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1u;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    memset(vendor_id, ' ', 8);
    memcpy(vendor_id, "MURM", 4);
    memset(product_id, ' ', 16);
    if (lun == 0)
        memcpy(product_id, "disk_a", 6);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (lun_present(lun)) {
        /* TinyUSB keeps one sense state for the whole MSC interface, not
         * one per LUN.  Clear a NOT READY left by probing another empty
         * LUN before reporting this medium ready. */
        tud_msc_set_sense(lun, 0, 0, 0);
        return true;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    bool writable;
    if (!lun_info(lun, block_count, block_size, &writable)) {
        *block_count = 0;
        *block_size = USBMSC_BLOCK_DISK;
        return;
    }

    /* See TEST UNIT READY above: a successful capacity probe belongs to this
     * LUN and must not inherit sense from a different, empty LUN. */
    tud_msc_set_sense(lun, 0, 0, 0);
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return lun_present(lun) && !lun_readonly(lun);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)power_condition;
    (void)start;
    (void)load_eject;

    /* START STOP UNIT is not a write-cache flush request.  Returning a FatFs
     * f_sync() error here makes Windows treat an otherwise readable LUN as
     * not ready.  pico-xt acknowledges this command unconditionally. */
    if (lun_present(lun))
        tud_msc_set_sense(lun, 0, 0, 0);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    FIL *fp;
    UINT done = 0;

    if (!lun_seek(lun, lba, offset, bufsize, &fp) ||
        f_read(fp, buffer, (UINT)bufsize, &done) != FR_OK ||
        done != (UINT)bufsize) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
        return -1;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    FIL *fp;
    UINT done = 0;

    if (lun_readonly(lun)) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }

    if (!lun_seek(lun, lba, offset, bufsize, &fp) ||
        f_write(fp, buffer, (UINT)bufsize, &done) != FR_OK ||
        done != (UINT)bufsize) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x02);
        return -1;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
