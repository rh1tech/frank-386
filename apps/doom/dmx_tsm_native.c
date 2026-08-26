#include <stdint.h>
#include <string.h>

#include "dmx.h"
#include "dos_yield.h"
#include "tsr_callback.h"

/*
 * Native replacement for the DMX Timer Service Manager used by DOS DOOM.
 *
 * DMX is an application/library compatibility layer, not part of the native
 * DOS API.  The platform supplies only the core0 timer hook.  TSM builds the
 * original master/service scheduler on top of that hook.
 *
 * murm386 currently dispatches TSR0 from the existing 44.1 kHz core0 timer.
 * The phase accumulators below intentionally avoid assuming that a requested
 * DMX rate divides 44100 exactly, although DOOM's normal 140 Hz master does.
 */
#define NATIVE_TSM_SOURCE_HZ 44100u
#define NATIVE_TSM_SLOTS     16

typedef struct native_tsm_slot
{
    int (*service)(void);
    uint32_t rate;
    uint32_t phase;
    int priority;
    unsigned char in_use;
    unsigned char paused;
} native_tsm_slot;

static native_tsm_slot native_tsm_slots[NATIVE_TSM_SLOTS];
static tsr_callback_t native_tsm_previous_tsr0;
static uint32_t native_tsm_master_rate;
static uint32_t native_tsm_master_phase;
static unsigned char native_tsm_installed;
static int native_tsm_max_priority;
static unsigned native_tsm_update_depth;
static volatile unsigned char native_tsm_dispatch_blocked;

static void TSM_DispatchMasterTick(void)
{
    int priority;
    int id;

    /* DMX priority 0 is the highest priority. */
    for (priority = 0; priority <= native_tsm_max_priority; ++priority)
    {
        for (id = 0; id < NATIVE_TSM_SLOTS; ++id)
        {
            native_tsm_slot *slot = &native_tsm_slots[id];
            int (*service)(void);

            if (!__atomic_load_n(&slot->in_use, __ATOMIC_ACQUIRE) ||
                slot->paused || slot->priority != priority)
                continue;

            slot->phase += slot->rate;
            while (slot->phase >= native_tsm_master_rate)
            {
                slot->phase -= native_tsm_master_rate;
                service = slot->service;
                if (service)
                    service();

                /* The callback is allowed to delete/pause its own service. */
                if (!__atomic_load_n(&slot->in_use, __ATOMIC_ACQUIRE) ||
                    slot->paused)
                    break;
            }
        }
    }
}

static void TSM_TimerCallback(void)
{
    if (native_tsm_installed && native_tsm_master_rate != 0 &&
        !native_tsm_dispatch_blocked)
    {
        native_tsm_master_phase += native_tsm_master_rate;
        while (native_tsm_master_phase >= NATIVE_TSM_SOURCE_HZ)
        {
            native_tsm_master_phase -= NATIVE_TSM_SOURCE_HZ;
            TSM_DispatchMasterTick();
        }
    }

    /* Preserve the displaced platform/DOS timer chain on every source tick. */
    if (native_tsm_previous_tsr0)
        native_tsm_previous_tsr0();
}

void TSM_Install(int rate)
{
    if (rate <= 0 || rate > (int)NATIVE_TSM_SOURCE_HZ)
        return;

    if (native_tsm_installed)
        TSM_Remove();

    memset(native_tsm_slots, 0, sizeof(native_tsm_slots));
    native_tsm_master_rate = (uint32_t)rate;
    native_tsm_master_phase = 0;
    native_tsm_max_priority = 0;
    native_tsm_update_depth = 0;
    native_tsm_dispatch_blocked = 0;
    native_tsm_previous_tsr0 = set_tsr0_callback(TSM_TimerCallback);
    native_tsm_installed = 1;
}

int TSM_NewService(int (*service)(void), int rate, int priority, int pause)
{
    int id;

    if (!native_tsm_installed || !service || rate <= 0 ||
        rate > (int)native_tsm_master_rate || priority < 0 || priority > 255)
        return -1;

    for (id = 0; id < NATIVE_TSM_SLOTS; ++id)
        if (!__atomic_load_n(&native_tsm_slots[id].in_use, __ATOMIC_ACQUIRE))
            break;
    if (id == NATIVE_TSM_SLOTS)
        return -1;

    native_tsm_slots[id].service = service;
    native_tsm_slots[id].rate = (uint32_t)rate;
    native_tsm_slots[id].phase = 0;
    native_tsm_slots[id].priority = priority;
    native_tsm_slots[id].paused = pause ? 1 : 0;
    if (priority > native_tsm_max_priority)
        native_tsm_max_priority = priority;
    __atomic_store_n(&native_tsm_slots[id].in_use, 1, __ATOMIC_RELEASE);
    return id;
}

void TSM_DelService(int id)
{
    if (id < 0 || id >= NATIVE_TSM_SLOTS)
        return;

    __atomic_store_n(&native_tsm_slots[id].in_use, 0, __ATOMIC_RELEASE);
    native_tsm_slots[id].service = 0;
    native_tsm_slots[id].rate = 0;
    native_tsm_slots[id].phase = 0;
    native_tsm_slots[id].priority = 0;
    native_tsm_slots[id].paused = 0;

    if (native_tsm_max_priority > 0)
    {
        int i;
        int max_priority = 0;
        for (i = 0; i < NATIVE_TSM_SLOTS; ++i)
            if (__atomic_load_n(&native_tsm_slots[i].in_use, __ATOMIC_ACQUIRE) &&
                native_tsm_slots[i].priority > max_priority)
                max_priority = native_tsm_slots[i].priority;
        native_tsm_max_priority = max_priority;
    }
}

void TSM_Remove(void)
{
    tsr_callback_t previous;

    if (!native_tsm_installed)
        return;

    previous = native_tsm_previous_tsr0;
    native_tsm_installed = 0;
    set_tsr0_callback(previous);
    native_tsm_previous_tsr0 = 0;
    native_tsm_master_rate = 0;
    native_tsm_master_phase = 0;
    native_tsm_max_priority = 0;
    native_tsm_update_depth = 0;
    native_tsm_dispatch_blocked = 0;
    memset(native_tsm_slots, 0, sizeof(native_tsm_slots));
}

void TSM_Lock(void)
{
    if (native_tsm_update_depth == 0)
        native_tsm_dispatch_blocked = 1;
    ++native_tsm_update_depth;
}

void TSM_Unlock(void)
{
    if (native_tsm_update_depth == 0)
        return;

    --native_tsm_update_depth;
    if (native_tsm_update_depth == 0)
        native_tsm_dispatch_blocked = 0;
}

void TSM_Yield(void)
{
    /* Compatibility entry point: timer services are asynchronous now. */
    (void)dos_yield();
}
