#include "usbmsc_device.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tusb.h"
#include "disk.h"

#define USBMSC_LUN_COUNT 6u
#define USBMSC_BLOCK_DISK 512u

static const char *const lun_names[USBMSC_LUN_COUNT] = {
    "disk_a", "disk_b", "disk_c", "disk_d", "disk_e", "disk_f"
};

static bool lun_info(uint8_t lun, uint32_t *block_count,
                     uint16_t *block_size, bool *writable)
{
    return lun < USBMSC_LUN_COUNT &&
           disk_raw_slot_info(lun, block_count, block_size, writable);
}

static bool lun_present(uint8_t lun)
{
    uint32_t blocks;
    uint16_t block_size;
    bool writable;
    return lun_info(lun, &blocks, &block_size, &writable);
}

static bool lun_readonly(uint8_t lun)
{
    uint32_t blocks;
    uint16_t block_size;
    bool writable;
    return lun_info(lun, &blocks, &block_size, &writable) && !writable;
}

void usbmsc_device_init(void)
{
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
    return USBMSC_LUN_COUNT;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    memset(vendor_id, ' ', 8);
    memcpy(vendor_id, "MURM", 4);
    memset(product_id, ' ', 16);
    if (lun < USBMSC_LUN_COUNT) {
        size_t n = strlen(lun_names[lun]);
        if (n > 16) n = 16;
        memcpy(product_id, lun_names[lun], n);
    }
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (lun_present(lun))
        return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    bool writable;
    if (!lun_info(lun, block_count, block_size, &writable)) {
        *block_count = 0;
        *block_size = USBMSC_BLOCK_DISK;
    }
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
    if (lun_present(lun) && !lun_readonly(lun))
        return disk_raw_slot_sync(lun);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    if (!disk_raw_slot_read(lun, lba, offset, buffer, bufsize)) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    if (lun_readonly(lun)) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }
    if (!disk_raw_slot_write(lun, lba, offset, buffer, bufsize)) {
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
