#include "usbmsc_device.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tusb.h"
#include "ff.h"
#include "disk.h"
#include "debug.h"

#define USBMSC_BLOCK_DISK 512u

typedef struct {
    FIL *fp;              /* file-backed disk; NULL when is_sd */
    bool is_sd;           /* true = whole raw SD card (no FIL) */
    uint32_t sd_sectors;  /* SD size in 512-byte sectors (is_sd only) */
    uint16_t block_size;
} usbmsc_disk_t;

static usbmsc_disk_t usb_disk;

static void bind_first_disk(void)
{
    usb_disk.fp = NULL;
    usb_disk.is_sd = false;
    usb_disk.sd_sectors = 0;
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

    /* No floppy/ATA image attached: fall back to exporting the whole SD card,
       so the host still sees a drive (e.g. to copy images onto the card).
       Available only while the SD raw HDD option is enabled. */
    uint32_t sectors = disk_raw_sd_sectors();
    if (sectors) {
        usb_disk.is_sd = true;
        usb_disk.sd_sectors = sectors;
    }
}

/* True when LUN 0 is bound to a medium (file image or raw SD). */
static bool lun_bound(uint8_t lun)
{
    if (lun != 0)
        return false;
    if (usb_disk.is_sd)
        return usb_disk.sd_sectors != 0;
    return usb_disk.fp && usb_disk.fp->obj.fs;
}

/* File handle for the FILE-backed path only (NULL for raw SD). */
static FIL *lun_file(uint8_t lun)
{
    if (lun != 0 || usb_disk.is_sd || !usb_disk.fp || !usb_disk.fp->obj.fs)
        return NULL;
    return usb_disk.fp;
}

static bool lun_info(uint8_t lun, uint32_t *block_count,
                     uint16_t *block_size, bool *writable)
{
    if (!lun_bound(lun) || !block_count || !block_size || !writable)
        return false;

    if (usb_disk.is_sd) {
        *block_size = USBMSC_BLOCK_DISK;
        *block_count = usb_disk.sd_sectors;
        *writable = !disk_raw_sd_readonly();
        return *block_count != 0;
    }

    FIL *fp = usb_disk.fp;
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
    return lun_bound(lun);
}

static bool lun_readonly(uint8_t lun)
{
    if (!lun_bound(lun))
        return false;
    if (usb_disk.is_sd)
        return disk_raw_sd_readonly();
    return !(usb_disk.fp->flag & FA_WRITE);
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

void __not_in_flash_func(usbmsc_device_task)(void)
{
    tud_task();
}

/*
 * Host-initiated disconnect detection.
 *
 * tud_mount_cb() fires once the host has configured us; tud_umount_cb() fires
 * when the host drops the device (eject, bus reset, cable pull). We latch a
 * mounted -> unmounted transition so the main loop can treat it exactly like
 * the user pressing Esc in the Win+F12 Disk Manager. The initial
 * not-yet-mounted state is ignored (usb_was_mounted gates it), so only a real
 * host-side disconnect is reported.
 */
static volatile bool usb_was_mounted  = false;
static volatile bool usb_host_dropped  = false;

void tud_mount_cb(void)
{
    DBG_PRINT("USB MSC: tud_mount_cb (host configured device)\n");
    usb_was_mounted = true;
}

void tud_umount_cb(void)
{
    DBG_PRINT("USB MSC: tud_umount_cb (bus disconnect / unconfigure)\n");
    if (usb_was_mounted) {
        usb_was_mounted = false;
        usb_host_dropped = true;
    }
}

/*
 * Windows "Safely Remove / Eject" of a FIXED-disk MSC device usually neither
 * ejects (no START STOP UNIT) nor unconfigures (no tud_umount_cb): it just
 * selectively suspends the port, so tud_suspend_cb() is the reliable
 * host-disconnect signal here. Gated on a prior mount so an early idle-suspend
 * during enumeration cannot trigger it.
 */
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    DBG_PRINT("USB MSC: tud_suspend_cb (host stopped SOF)\n");
    if (usb_was_mounted)
        usb_host_dropped = true;
}

void tud_resume_cb(void)
{
    DBG_PRINT("USB MSC: tud_resume_cb (host resumed)\n");
}

bool usbmsc_device_host_disconnected(void)
{
    if (usb_host_dropped) {
        usb_host_dropped = false;   /* one-shot: consume the event */
        return true;
    }
    return false;
}

void usbmsc_device_shutdown(void)
{
    tud_disconnect();

    if (usb_disk.is_sd) {
        (void)disk_raw_sd_sync();
    } else if (usb_disk.fp && usb_disk.fp->obj.fs) {
        if (usb_disk.fp->flag & FA_WRITE)
            (void)f_sync(usb_disk.fp);
        (void)f_close(usb_disk.fp);
    }

    usb_disk.fp = NULL;
    usb_disk.is_sd = false;
    usb_disk.sd_sectors = 0;
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

    s = index == 1 ? "Murmulator" : index == 2 ? "FreeDOS disk" : NULL;
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
        memcpy(product_id, usb_disk.is_sd ? "sd_card" : "disk_a",
               usb_disk.is_sd ? 7 : 6);
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

    /* Host "Safely Remove / Eject" issues SCSI START STOP UNIT with
     * load_eject=1, start=0. This - not a USB bus disconnect - is how a host
     * normally drops an MSC device, so tud_umount_cb() does NOT fire. Latch it
     * as a host-initiated disconnect so the DEVICE loop runs the exit-to-host
     * cleanup (same as Esc in the Disk Manager). */
    if (load_eject && !start)
        usb_host_dropped = true;
    if (load_eject && !start)
        DBG_PRINT("USB MSC: START STOP UNIT eject (load_eject, !start)\n");

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

    if (usb_disk.is_sd && lun == 0) {
        /* TinyUSB reads a whole block per call, so offset is 0 and bufsize is a
           512-multiple. */
        if (offset != 0 || (bufsize % USBMSC_BLOCK_DISK) != 0 ||
            !disk_raw_sd_read(lba, buffer, bufsize / USBMSC_BLOCK_DISK)) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
            return -1;
        }
        return (int32_t)bufsize;
    }

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

    if (usb_disk.is_sd && lun == 0) {
        if (offset != 0 || (bufsize % USBMSC_BLOCK_DISK) != 0 ||
            !disk_raw_sd_write(lba, buffer, bufsize / USBMSC_BLOCK_DISK)) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x02);
            return -1;
        }
        return (int32_t)bufsize;
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
