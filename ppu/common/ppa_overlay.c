#include "ppa_overlay.h"
#include "ppa_utf8.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct overlay_slot {
    volatile unsigned int sequence;
    struct ppa_overlay_message message;
};

static struct overlay_slot g_slots[PPA_OVERLAY_MAX_MESSAGES];
static unsigned int g_next_slot;
static SceUID g_writer_sema = -1;

static int overlay_writer_lock(void)
{
    if (g_writer_sema < 0)
        g_writer_sema = sceKernelCreateSema("ppa_overlay_writer", 0, 1, 1, 0);
    if (g_writer_sema < 0)
        return 0;
    return sceKernelWaitSema(g_writer_sema, 1, 0) == 0;
}

static void overlay_writer_unlock(int locked)
{
    if (locked && g_writer_sema >= 0)
        (void)sceKernelSignalSema(g_writer_sema, 1);
}

static void memory_barrier(void)
{
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static uint64_t overlay_now(void) { return sceKernelGetSystemTimeWide(); }

void ppa_overlay_reset(void)
{
    int locked = overlay_writer_lock();
    if (!locked)
        return;
    memset(g_slots, 0, sizeof(g_slots));
    g_next_slot = 0;
    overlay_writer_unlock(locked);
}

static unsigned int choose_slot(unsigned int key, uint64_t now)
{
    unsigned int i;
    for (i = 0; i < PPA_OVERLAY_MAX_MESSAGES; ++i)
        if (g_slots[i].message.key == key)
            return i;
    for (i = 0; i < PPA_OVERLAY_MAX_MESSAGES; ++i) {
        uint64_t expires = g_slots[i].message.expires_us;
        if (g_slots[i].message.key == 0 || (expires != 0 && expires <= now))
            return i;
    }
    i = g_next_slot++ % PPA_OVERLAY_MAX_MESSAGES;
    return i;
}

void ppa_overlay_post(unsigned int key, unsigned int position,
                      unsigned int duration_ms, unsigned int flags,
                      const char *text)
{
    uint64_t now = overlay_now();
    unsigned int index;
    struct overlay_slot *slot;
    int locked;
    if (key == 0 || !text) return;
    locked = overlay_writer_lock();
    if (!locked)
        return;
    for (index = 0; index < PPA_OVERLAY_MAX_MESSAGES; ++index) {
        if (g_slots[index].message.key == key &&
            (g_slots[index].message.expires_us == 0 ||
             g_slots[index].message.expires_us > now)) {
            break;
        }
    }
    index = choose_slot(key, now);
    slot = &g_slots[index];
    slot->sequence++;
    memory_barrier();
    memset(&slot->message, 0, sizeof(slot->message));
    slot->message.key = key;
    slot->message.position = position;
    slot->message.flags = flags | PPA_OVERLAY_UTF8;
    slot->message.expires_us = duration_ms == 0 ? 0 :
        now + (uint64_t)duration_ms * 1000ULL;
    ppa_utf8_sanitize_copy(slot->message.text,
                           sizeof(slot->message.text), text);
    memory_barrier();
    slot->sequence++;
    overlay_writer_unlock(locked);
}

void ppa_overlay_postf(unsigned int key, unsigned int position,
                       unsigned int duration_ms, unsigned int flags,
                       const char *format, ...)
{
    char buffer[PPA_OVERLAY_TEXT_CAPACITY];
    va_list ap;
    if (!format) return;
    va_start(ap, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1U] = 0;
    ppa_overlay_post(key, position, duration_ms, flags, buffer);
}

void ppa_overlay_clear(unsigned int key)
{
    unsigned int i;
    int locked = overlay_writer_lock();
    if (!locked)
        return;
    for (i = 0; i < PPA_OVERLAY_MAX_MESSAGES; ++i) {
        struct overlay_slot *slot = &g_slots[i];
        if (slot->message.key != key) continue;
        slot->sequence++;
        memory_barrier();
        memset(&slot->message, 0, sizeof(slot->message));
        memory_barrier();
        slot->sequence++;
    }
    overlay_writer_unlock(locked);
}

unsigned int ppa_overlay_snapshot(struct ppa_overlay_message *out,
                                  unsigned int capacity, uint64_t now)
{
    unsigned int i;
    unsigned int n = 0;
    if (!out || capacity == 0) return 0;
    for (i = 0; i < PPA_OVERLAY_MAX_MESSAGES && n < capacity; ++i) {
        unsigned int before;
        unsigned int after;
        struct ppa_overlay_message temp;
        do {
            before = g_slots[i].sequence;
            if (before & 1U) continue;
            memory_barrier();
            temp = g_slots[i].message;
            memory_barrier();
            after = g_slots[i].sequence;
        } while (before != after || (after & 1U));
        if (temp.key == 0 || (temp.expires_us != 0 && temp.expires_us <= now))
            continue;
        out[n++] = temp;
    }
    return n;
}

int ppa_overlay_has_active(uint64_t now)
{
    struct ppa_overlay_message one[PPA_OVERLAY_MAX_MESSAGES];
    return ppa_overlay_snapshot(one, PPA_OVERLAY_MAX_MESSAGES, now) != 0;
}

