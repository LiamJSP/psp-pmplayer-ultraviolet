#include "ppa_browser_fs_cache.h"
#include "ppa_io.h"
#include "ppa_thread_policy.h"
#include "ppa_vfs.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define PPA_FS_CACHE_MAGIC                0x50465343U /* PFSC */
#define PPA_FS_CACHE_VERSION              1U
#define PPA_FS_CACHE_MAX_DIRECTORIES      128U
#define PPA_FS_CACHE_MAX_RAM_BYTES        (2U * 1024U * 1024U)
#define PPA_FS_CACHE_ROOT_ITEM_LIMIT      20
#define PPA_FS_CACHE_CHILD_ITEM_LIMIT     10
#define PPA_FS_CACHE_BUILD_DEADLINE_US    15000000ULL
#define PPA_FS_CACHE_THREAD_STACK         (48 * 1024)
#define PPA_FS_CACHE_DISK_PATH            "ms0:/PSP/SYSTEM/ppa_browser_fs_cache_v1.bin"
#define PPA_FS_CACHE_TEMP_PATH            "ms0:/PSP/SYSTEM/ppa_browser_fs_cache_v1.tmp"
#define PPA_FS_CACHE_BACKUP_PATH          "ms0:/PSP/SYSTEM/ppa_browser_fs_cache_v1.bak"
#define PPA_FS_CACHE_WRITE_CHUNK           (32U * 1024U)
/* Learn directories only through explicit navigation. Disk entries provide
 * immediate names but navigation always refreshes them in the worker, since
 * a PC can change the card while PPA is not running. Never start the spider. */
#define PPA_FS_CACHE_DISK_PERSISTENCE      1
#define PPA_FS_CACHE_STARTUP_PREFETCH      0

#if defined(__GNUC__)
#define PPA_PACKED __attribute__((packed))
#else
#define PPA_PACKED
#endif

typedef struct PPA_PACKED ppa_fs_cache_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint32_t directory_count;
    uint32_t item_count;
    uint32_t reserved;
} ppa_fs_cache_file_header;

typedef struct PPA_PACKED ppa_fs_cache_disk_directory {
    uint16_t path_bytes;
    uint16_t short_path_bytes;
    uint16_t item_count;
    uint8_t complete;
    uint8_t reserved;
    uint32_t blob_bytes;
} ppa_fs_cache_disk_directory;

typedef struct PPA_PACKED ppa_fs_cache_item_header {
    uint16_t short_name_bytes;
    uint16_t long_name_bytes;
    uint8_t filetype;
    uint8_t reserved[3];
} ppa_fs_cache_item_header;

typedef struct ppa_fs_cache_directory {
    uint32_t hash;
    uint16_t item_count;
    uint8_t complete;
    uint8_t in_use;
    uint32_t blob_bytes;
    char path[512];
    char short_path[512];
    unsigned char *blob;
} ppa_fs_cache_directory;

typedef struct ppa_fs_cache_state {
    int initialized;
    int show_hidden;
    int show_unknown;
    file_type_ext_struct *filter;
    SceUID lock;
    SceUID thread;
    volatile int stop;
    volatile int building;
    volatile int suspended;
    uint64_t build_deadline_us;
    int loaded_from_disk;
    int loaded_from_backup;
    int written_this_session;
    int ram_released_for_playback;
    int dirty;
    int no_cache_mode;
    volatile int ram_cap_hit;
    volatile unsigned int interactive_generation;
    unsigned int directory_count;
    unsigned int item_count;
    unsigned int blob_bytes;
    ppa_fs_cache_directory directories[PPA_FS_CACHE_MAX_DIRECTORIES];
} ppa_fs_cache_state;

static ppa_fs_cache_state g_fs_cache;
static uint32_t g_crc_table[256];
static int g_crc_table_ready;

static int ppa_fs_cache_lock(void)
{
    return g_fs_cache.initialized &&
           sceKernelWaitSema(g_fs_cache.lock, 1, 0) >= 0;
}

static int ppa_fs_cache_try_lock(void)
{
    return g_fs_cache.initialized &&
           sceKernelPollSema(g_fs_cache.lock, 1) >= 0;
}

static void ppa_fs_cache_unlock(void)
{
    if (g_fs_cache.initialized)
        sceKernelSignalSema(g_fs_cache.lock, 1);
}

static int ppa_fs_cache_path_supported(const char *path)
{
    if (path == 0)
        return 0;
    return path[0] == 0 ||
           strncmp(path, "ms0:", 4) == 0 ||
           strncmp(path, "ms1:", 4) == 0 ||
           strncmp(path, "ef0:", 4) == 0;
}

static int ppa_fs_cache_valid_name(const char *name, unsigned int bytes,
                                 unsigned int type, int root)
{
    unsigned int i;
    if (bytes < 2U || bytes > 256U || name[bytes - 1U] != 0 ||
        memchr(name, 0, bytes - 1U) != 0 || type > FS_UNKNOWN_FILE)
        return 0;
    if (root)
        return type == FS_DIRECTORY &&
               (strcmp(name, "ms0:") == 0 || strcmp(name, "ms1:") == 0 ||
                strcmp(name, "ef0:") == 0);
    if (strcmp(name, "..") == 0)
        return type == FS_DIRECTORY;
    if (strcmp(name, ".") == 0)
        return 0;
    for (i = 0; i + 1U < bytes; ++i)
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':')
            return 0;
    return 1;
}

static size_t ppa_fs_cache_bounded_strlen(const char *text, size_t limit)
{
    size_t length = 0;
    if (text == 0)
        return 0;
    while (length < limit && text[length] != 0)
        ++length;
    return length;
}

static uint32_t ppa_fs_cache_hash(const char *path, const char *short_path)
{
    uint32_t hash = 2166136261U;
    const unsigned char *cursor;

    cursor = (const unsigned char *)(path ? path : "");
    while (*cursor != 0) {
        unsigned char c = *cursor++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        hash ^= c;
        hash *= 16777619U;
    }
    hash ^= 0xffU;
    hash *= 16777619U;
    cursor = (const unsigned char *)(short_path ? short_path : "");
    while (*cursor != 0) {
        unsigned char c = *cursor++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        hash ^= c;
        hash *= 16777619U;
    }
    return hash ? hash : 1U;
}

static void ppa_fs_cache_crc_init(void)
{
    unsigned int i;
    if (g_crc_table_ready)
        return;
    for (i = 0; i < 256U; ++i) {
        uint32_t value = i;
        unsigned int bit;
        for (bit = 0; bit < 8U; ++bit)
            value = (value >> 1) ^ ((value & 1U) ? 0xedb88320U : 0U);
        g_crc_table[i] = value;
    }
    g_crc_table_ready = 1;
}

static uint32_t ppa_fs_cache_crc32(const void *data, unsigned int bytes)
{
    const unsigned char *cursor = (const unsigned char *)data;
    uint32_t crc = 0xffffffffU;
    unsigned int i;
    ppa_fs_cache_crc_init();
    for (i = 0; i < bytes; ++i)
        crc = (crc >> 8) ^ g_crc_table[(crc ^ cursor[i]) & 0xffU];
    return crc ^ 0xffffffffU;
}

static unsigned int ppa_fs_cache_ram_usage_locked(void)
{
    return (unsigned int)sizeof(g_fs_cache.directories) + g_fs_cache.blob_bytes;
}

static void ppa_fs_cache_clear_locked(void)
{
    unsigned int i;
    for (i = 0; i < PPA_FS_CACHE_MAX_DIRECTORIES; ++i) {
        if (g_fs_cache.directories[i].blob != 0)
            free(g_fs_cache.directories[i].blob);
        memset(&g_fs_cache.directories[i], 0,
               sizeof(g_fs_cache.directories[i]));
    }
    g_fs_cache.directory_count = 0;
    g_fs_cache.item_count = 0;
    g_fs_cache.blob_bytes = 0;
    g_fs_cache.loaded_from_disk = 0;
    g_fs_cache.loaded_from_backup = 0;
    g_fs_cache.ram_cap_hit = 0;
    g_fs_cache.interactive_generation = 0;
}

static void ppa_fs_cache_enter_no_cache_mode(const char *reason)
{
    if (ppa_fs_cache_lock()) {
        ppa_fs_cache_clear_locked();
        g_fs_cache.no_cache_mode = 1;
        g_fs_cache.dirty = 0;
        g_fs_cache.build_deadline_us = 0;
        ppa_fs_cache_unlock();
    }
}

static ppa_fs_cache_directory *ppa_fs_cache_find_locked(const char *path,
                                                         const char *short_path)
{
    uint32_t hash = ppa_fs_cache_hash(path, short_path);
    unsigned int i;
    for (i = 0; i < PPA_FS_CACHE_MAX_DIRECTORIES; ++i) {
        ppa_fs_cache_directory *entry = &g_fs_cache.directories[i];
        if (!entry->in_use || entry->hash != hash)
            continue;
        if (strcmp(entry->path, path ? path : "") == 0 &&
            strcmp(entry->short_path, short_path ? short_path : "") == 0)
            return entry;
    }
    return 0;
}

static ppa_fs_cache_directory *ppa_fs_cache_free_slot_locked(void)
{
    unsigned int i;
    for (i = 0; i < PPA_FS_CACHE_MAX_DIRECTORIES; ++i)
        if (!g_fs_cache.directories[i].in_use)
            return &g_fs_cache.directories[i];
    return 0;
}

static void ppa_fs_cache_remove_entry_locked(ppa_fs_cache_directory *entry)
{
    if (entry == 0 || !entry->in_use)
        return;
    if (entry->blob != 0)
        free(entry->blob);
    if (g_fs_cache.directory_count != 0)
        --g_fs_cache.directory_count;
    if (g_fs_cache.item_count >= entry->item_count)
        g_fs_cache.item_count -= entry->item_count;
    else
        g_fs_cache.item_count = 0;
    if (g_fs_cache.blob_bytes >= entry->blob_bytes)
        g_fs_cache.blob_bytes -= entry->blob_bytes;
    else
        g_fs_cache.blob_bytes = 0;
    memset(entry, 0, sizeof(*entry));
}

static int ppa_fs_cache_encode_items(const directory_item_struct *items,
                                     int item_count,
                                     unsigned char **blob_out,
                                     unsigned int *bytes_out)
{
    unsigned int bytes = 0;
    unsigned char *blob;
    unsigned char *cursor;
    int i;

    if (blob_out == 0 || bytes_out == 0 || item_count < 0)
        return 0;
    *blob_out = 0;
    *bytes_out = 0;
    if (item_count == 0)
        return 1;
    if (items == 0)
        return 0;

    for (i = 0; i < item_count; ++i) {
        size_t short_len;
        size_t long_len;
        if ((i & 31) == 0 && ppa_io_cancel_requested())
            return 0;
        short_len = ppa_fs_cache_bounded_strlen(items[i].shortname,
                                   sizeof(items[i].shortname)) + 1U;
        long_len = ppa_fs_cache_bounded_strlen(items[i].longname,
                                  sizeof(items[i].longname)) + 1U;
        if (short_len > 256U || long_len > 256U)
            return 0;
        if (bytes > 0xffffffffU - (unsigned int)sizeof(ppa_fs_cache_item_header) -
                    (unsigned int)short_len - (unsigned int)long_len)
            return 0;
        bytes += (unsigned int)sizeof(ppa_fs_cache_item_header) +
                 (unsigned int)short_len + (unsigned int)long_len;
    }

    blob = (unsigned char *)malloc(bytes);
    if (blob == 0)
        return 0;
    cursor = blob;
    for (i = 0; i < item_count; ++i) {
        ppa_fs_cache_item_header header;
        unsigned int short_len;
        unsigned int long_len;
        if ((i & 31) == 0 && ppa_io_cancel_requested()) {
            free(blob);
            return 0;
        }
        short_len = (unsigned int)ppa_fs_cache_bounded_strlen(items[i].shortname,
                                                        sizeof(items[i].shortname)) + 1U;
        long_len = (unsigned int)ppa_fs_cache_bounded_strlen(items[i].longname,
                                                       sizeof(items[i].longname)) + 1U;
        memset(&header, 0, sizeof(header));
        header.short_name_bytes = (uint16_t)short_len;
        header.long_name_bytes = (uint16_t)long_len;
        header.filetype = (uint8_t)items[i].filetype;
        memcpy(cursor, &header, sizeof(header));
        cursor += sizeof(header);
        memcpy(cursor, items[i].shortname, short_len);
        cursor += short_len;
        memcpy(cursor, items[i].longname, long_len);
        cursor += long_len;
    }

    *blob_out = blob;
    *bytes_out = bytes;
    return 1;
}

static int ppa_fs_cache_decode_items(const ppa_fs_cache_directory *entry,
                                     directory_item_struct **items_out,
                                     int *item_count_out)
{
    directory_item_struct *items;
    const unsigned char *cursor;
    const unsigned char *end;
    int i;

    if (entry == 0 || items_out == 0 || item_count_out == 0)
        return 0;
    *items_out = 0;
    *item_count_out = 0;
    if (entry->item_count == 0)
        return entry->blob_bytes == 0;
    if (entry->blob == 0 || entry->blob_bytes /
            (sizeof(ppa_fs_cache_item_header) + 4U) < entry->item_count)
        return 0;

    items = (directory_item_struct *)calloc(entry->item_count,
                                             sizeof(directory_item_struct));
    if (items == 0)
        return 0;
    cursor = entry->blob;
    end = entry->blob + entry->blob_bytes;
    for (i = 0; i < entry->item_count; ++i) {
        ppa_fs_cache_item_header header;
        if ((size_t)(end - cursor) < sizeof(header))
            goto invalid;
        memcpy(&header, cursor, sizeof(header));
        cursor += sizeof(header);
        if (header.short_name_bytes == 0 || header.short_name_bytes > 256U ||
            header.long_name_bytes == 0 || header.long_name_bytes > 256U ||
            (size_t)(end - cursor) <
                (size_t)header.short_name_bytes + header.long_name_bytes)
            goto invalid;
        if (!ppa_fs_cache_valid_name((const char *)cursor,
                                    header.short_name_bytes, header.filetype,
                                    entry->path[0] == 0) ||
            !ppa_fs_cache_valid_name((const char *)cursor + header.short_name_bytes,
                                    header.long_name_bytes, header.filetype,
                                    entry->path[0] == 0))
            goto invalid;
        memcpy(items[i].shortname, cursor, header.short_name_bytes);
        items[i].shortname[255] = 0;
        cursor += header.short_name_bytes;
        memcpy(items[i].longname, cursor, header.long_name_bytes);
        items[i].longname[255] = 0;
        cursor += header.long_name_bytes;
        items[i].filetype = (file_type_enum)header.filetype;
        items[i].compname = ppa_vfs_is_ntfs_path(entry->path) ?
                            items[i].longname : items[i].shortname;
        /* filesize/cdate/ctime/mdate/mtime remain intentionally zero. */
    }
    if (cursor != end)
        goto invalid;

    *items_out = items;
    *item_count_out = entry->item_count;
    return 1;

invalid:
    free(items);
    return 0;
}

int ppa_browser_fs_cache_put(const char *path,
                             const char *short_path,
                             const directory_item_struct *items,
                             int item_count,
                             int complete)
{
    unsigned char *blob = 0;
    unsigned int blob_bytes = 0;
    ppa_fs_cache_directory *entry;
    unsigned int old_blob_bytes = 0;
    unsigned int old_item_count = 0;
    unsigned int prospective;

    if (!g_fs_cache.initialized || g_fs_cache.no_cache_mode ||
        !ppa_fs_cache_path_supported(path) ||
        !ppa_fs_cache_path_supported(short_path) || item_count < 0 ||
        item_count > 65535 || strlen(path) >= sizeof(g_fs_cache.directories[0].path) ||
        strlen(short_path) >= sizeof(g_fs_cache.directories[0].short_path))
        return 0;
    if (!ppa_fs_cache_encode_items(items, item_count, &blob, &blob_bytes))
        return 0;

    if (!ppa_fs_cache_lock()) {
        free(blob);
        return 0;
    }
    entry = ppa_fs_cache_find_locked(path, short_path);
    if (entry != 0) {
        if (entry->item_count == (unsigned int)item_count &&
            entry->complete == (complete ? 1U : 0U) &&
            entry->blob_bytes == blob_bytes &&
            (blob_bytes == 0 || memcmp(entry->blob, blob, blob_bytes) == 0)) {
            ppa_fs_cache_unlock();
            free(blob);
            return 1;
        }
        old_blob_bytes = entry->blob_bytes;
        old_item_count = entry->item_count;
    }
    else {
        entry = ppa_fs_cache_free_slot_locked();
        if (entry == 0) {
            g_fs_cache.ram_cap_hit = 1;
            ppa_fs_cache_unlock();
            free(blob);
            return 0;
        }
    }

    prospective = ppa_fs_cache_ram_usage_locked() - old_blob_bytes + blob_bytes;
    if (prospective > PPA_FS_CACHE_MAX_RAM_BYTES) {
        g_fs_cache.ram_cap_hit = 1;
        ppa_fs_cache_unlock();
        free(blob);
        return 0;
    }

    if (entry->blob != 0)
        free(entry->blob);
    if (!entry->in_use)
        g_fs_cache.directory_count++;
    g_fs_cache.item_count -= old_item_count;
    g_fs_cache.blob_bytes -= old_blob_bytes;
    memset(entry, 0, sizeof(*entry));
    entry->hash = ppa_fs_cache_hash(path, short_path);
    entry->item_count = (uint16_t)item_count;
    entry->complete = complete ? 1U : 0U;
    entry->in_use = 1U;
    entry->blob_bytes = blob_bytes;
    entry->blob = blob;
    strncpy(entry->path, path, sizeof(entry->path) - 1U);
    strncpy(entry->short_path, short_path, sizeof(entry->short_path) - 1U);
    g_fs_cache.item_count += (unsigned int)item_count;
    g_fs_cache.blob_bytes += blob_bytes;
    g_fs_cache.dirty = 1;
    ppa_fs_cache_unlock();
    return 1;
}

int ppa_browser_fs_cache_get(const char *path,
                             const char *short_path,
                             directory_item_struct **items,
                             int *item_count,
                             int *complete)
{
    ppa_fs_cache_directory *entry;
    int result = 0;

    if (items == 0 || item_count == 0 || complete == 0)
        return 0;
    *items = 0;
    *item_count = 0;
    *complete = 0;
    if (!g_fs_cache.initialized || g_fs_cache.no_cache_mode ||
        !ppa_fs_cache_path_supported(path) ||
        (ppa_vfs_is_ntfs_path(path) && !ppa_vfs_ntfs_available()) ||
        !ppa_fs_cache_path_supported(short_path) || !ppa_fs_cache_try_lock())
        return 0;
    entry = ppa_fs_cache_find_locked(path, short_path);
    if (entry != 0) {
        result = ppa_fs_cache_decode_items(entry, items, item_count);
        if (result)
            *complete = entry->complete ? 1 : 0;
    }
    ppa_fs_cache_unlock();
    return result;
}

void ppa_browser_fs_cache_invalidate(const char *path,
                                     const char *short_path)
{
    int remove_disk = 0;

    if (!g_fs_cache.initialized || path == 0 || short_path == 0)
        return;
    ppa_browser_fs_cache_cancel_build(1);
    if (ppa_fs_cache_lock()) {
        ppa_fs_cache_directory *entry =
            ppa_fs_cache_find_locked(path, short_path);
        ppa_fs_cache_remove_entry_locked(entry);
        g_fs_cache.dirty = 1;

        /* The cache assumes external storage is stable for the process
         * lifetime. A mutation initiated by PPA itself is the one exception:
         * discard the known-stale snapshot and permit one replacement write. */
        remove_disk = g_fs_cache.loaded_from_disk ||
                      g_fs_cache.written_this_session;
        g_fs_cache.loaded_from_disk = 0;
        g_fs_cache.loaded_from_backup = 0;
        g_fs_cache.written_this_session = 0;
        ppa_fs_cache_unlock();
    }
    if (remove_disk) {
        (void)sceIoRemove(PPA_FS_CACHE_TEMP_PATH);
        (void)sceIoRemove(PPA_FS_CACHE_DISK_PATH);
        (void)sceIoRemove(PPA_FS_CACHE_BACKUP_PATH);
    }
}

enum ppa_fs_cache_spider_result {
    PPA_FS_CACHE_SPIDER_ERROR = 0,
    PPA_FS_CACHE_SPIDER_OK = 1,
    PPA_FS_CACHE_SPIDER_BOUNDED = 2,
    PPA_FS_CACHE_SPIDER_PREEMPTED = 3
};

static int ppa_fs_cache_cancel_reason(void)
{
    if (g_fs_cache.stop || g_fs_cache.suspended)
        return PPA_FS_CACHE_SPIDER_BOUNDED;
    if (g_fs_cache.building && g_fs_cache.build_deadline_us != 0 &&
        (uint64_t)sceKernelGetSystemTimeWide() >=
            g_fs_cache.build_deadline_us)
        return PPA_FS_CACHE_SPIDER_BOUNDED;
    if (g_fs_cache.interactive_generation != 0U)
        return PPA_FS_CACHE_SPIDER_PREEMPTED;
    return PPA_FS_CACHE_SPIDER_OK;
}

static int ppa_fs_cache_cancel_check(void *user)
{
    (void)user;
    return ppa_fs_cache_cancel_reason() != PPA_FS_CACHE_SPIDER_OK;
}

static int ppa_fs_cache_deadline_crossed(uint64_t deadline)
{
    return (uint64_t)sceKernelGetSystemTimeWide() >= deadline;
}

static int ppa_fs_cache_spider_directory(const char *path,
                                         const char *short_path,
                                         int limit,
                                         uint64_t deadline,
                                         directory_item_struct **full_items_out,
                                         int *full_count_out)
{
    directory_item_struct *items = 0;
    char mutable_short[512];
    int count;
    int cached_count;
    int complete;

    if (full_items_out != 0) *full_items_out = 0;
    if (full_count_out != 0) *full_count_out = 0;
    if (ppa_fs_cache_deadline_crossed(deadline) || ppa_fs_cache_cancel_check(0))
        return ppa_fs_cache_cancel_reason();
    if (!ppa_io_background_acquire(ppa_fs_cache_cancel_check, 0))
        return ppa_fs_cache_cancel_reason();

    strncpy(mutable_short, short_path, sizeof(mutable_short) - 1U);
    mutable_short[sizeof(mutable_short) - 1U] = 0;
    ppa_io_set_cancel_check(ppa_fs_cache_cancel_check, 0);
    count = open_directory_names_only(path, mutable_short,
                                      g_fs_cache.show_hidden,
                                      g_fs_cache.show_unknown,
                                      g_fs_cache.filter,
                                      &items);
    ppa_io_set_cancel_check(0, 0);
    ppa_io_background_release();

    if (ppa_fs_cache_cancel_check(0) ||
        ppa_fs_cache_deadline_crossed(deadline)) {
        int reason = ppa_fs_cache_cancel_reason();
        if (items != 0) free(items);
        return reason;
    }
    if (count < 0) {
        if (items != 0) free(items);
        return 0;
    }
    if (count > 0 && items == 0)
        return 0;

    cached_count = count;
    if (cached_count > limit)
        cached_count = limit;
    complete = count <= limit;
    ppa_io_set_cancel_check(ppa_fs_cache_cancel_check, 0);
    if (!ppa_browser_fs_cache_put(path, mutable_short, items,
                                  cached_count, complete)) {
        ppa_io_set_cancel_check(0, 0);
        if (ppa_fs_cache_cancel_check(0) || g_fs_cache.ram_cap_hit) {
            int reason = ppa_fs_cache_cancel_check(0) ?
                         ppa_fs_cache_cancel_reason() :
                         PPA_FS_CACHE_SPIDER_BOUNDED;
            free(items);
            return reason;
        }
        free(items);
        return 0;
    }
    ppa_io_set_cancel_check(0, 0);

    if (full_items_out != 0 && full_count_out != 0) {
        *full_items_out = items;
        *full_count_out = count;
    }
    else {
        free(items);
    }
    return PPA_FS_CACHE_SPIDER_OK;
}

static int ppa_fs_cache_spider_root(const char *root,
                                    uint64_t deadline)
{
    directory_item_struct *root_items = 0;
    int root_count = 0;
    int result;
    int i;
    int child_budget;

    result = ppa_fs_cache_spider_directory(root, root,
                                           PPA_FS_CACHE_ROOT_ITEM_LIMIT,
                                           deadline,
                                           &root_items, &root_count);
    if (result != PPA_FS_CACHE_SPIDER_OK)
        return result;

    child_budget = root_count;
    if (child_budget > PPA_FS_CACHE_ROOT_ITEM_LIMIT)
        child_budget = PPA_FS_CACHE_ROOT_ITEM_LIMIT;
    for (i = 0; i < child_budget; ++i) {
        char child_path[512];
        char child_short[512];
        const char *component;
        const char *short_component;
        int child_result;

        if (ppa_fs_cache_deadline_crossed(deadline) ||
            ppa_fs_cache_cancel_check(0)) {
            free(root_items);
            return ppa_fs_cache_cancel_reason();
        }
        if (root_items[i].filetype != FS_DIRECTORY ||
            strcmp(root_items[i].longname, "..") == 0)
            continue;
        component = root_items[i].compname && root_items[i].compname[0] ?
                    root_items[i].compname : root_items[i].longname;
        short_component = ppa_vfs_is_ntfs_path(root) ?
                          component : root_items[i].shortname;
        {
            int path_written = snprintf(child_path, sizeof(child_path),
                                        "%s%s/", root, component);
            int short_written = snprintf(child_short, sizeof(child_short),
                                         "%s%s/", root, short_component);
            if (path_written <= 0 ||
                path_written >= (int)sizeof(child_path) ||
                short_written <= 0 ||
                short_written >= (int)sizeof(child_short))
                continue;
        }

        child_result = ppa_fs_cache_spider_directory(
            child_path, child_short, PPA_FS_CACHE_CHILD_ITEM_LIMIT,
            deadline, 0, 0);
        if (child_result == PPA_FS_CACHE_SPIDER_ERROR) {
            free(root_items);
            return 0;
        }
        if (child_result != PPA_FS_CACHE_SPIDER_OK) {
            free(root_items);
            return child_result;
        }
    }

    free(root_items);
    return PPA_FS_CACHE_SPIDER_OK;
}

static int ppa_fs_cache_wait_for_interactive_io(uint64_t deadline)
{
    while (g_fs_cache.interactive_generation != 0U) {
        if (g_fs_cache.stop || g_fs_cache.suspended ||
            ppa_fs_cache_deadline_crossed(deadline))
            return 0;
        sceKernelDelayThread(5000);
    }
    return !g_fs_cache.stop && !g_fs_cache.suspended &&
           !ppa_fs_cache_deadline_crossed(deadline);
}

static int ppa_fs_cache_spider_root_resumable(const char *root,
                                               uint64_t deadline)
{
    int result;
    do {
        result = ppa_fs_cache_spider_root(root, deadline);
        if (result != PPA_FS_CACHE_SPIDER_PREEMPTED)
            return result;
    } while (ppa_fs_cache_wait_for_interactive_io(deadline));
    return PPA_FS_CACHE_SPIDER_BOUNDED;
}

static int ppa_fs_cache_builder_thread(SceSize args, void *argp)
{
    uint64_t deadline;
    int result = 1;
    (void)args;
    (void)argp;

    g_fs_cache.building = 1;
    deadline = g_fs_cache.build_deadline_us;
    if (deadline == 0)
        deadline = (uint64_t)sceKernelGetSystemTimeWide() +
                   PPA_FS_CACHE_BUILD_DEADLINE_US;
    g_fs_cache.build_deadline_us = deadline;
    result = ppa_fs_cache_spider_root_resumable("ms0:/", deadline);
    if (result == PPA_FS_CACHE_SPIDER_OK && ppa_vfs_ntfs_available() &&
        !ppa_fs_cache_deadline_crossed(deadline))
        result = ppa_fs_cache_spider_root_resumable("ms1:/", deadline);

    if (result == PPA_FS_CACHE_SPIDER_ERROR) {
        ppa_fs_cache_enter_no_cache_mode("spider_error");
    }
    else {
    }

    g_fs_cache.build_deadline_us = 0;
    g_fs_cache.building = 0;
    sceKernelExitThread(0);
    return 0;
}

static int ppa_fs_cache_read_exact(SceUID fd, void *buffer, unsigned int bytes)
{
    unsigned char *cursor = (unsigned char *)buffer;
    unsigned int total = 0;
    while (total < bytes) {
        unsigned int chunk = bytes - total;
        int got;
        if (chunk > PPA_FS_CACHE_WRITE_CHUNK)
            chunk = PPA_FS_CACHE_WRITE_CHUNK;
        got = sceIoRead(fd, cursor + total, chunk);
        if (got <= 0)
            return 0;
        total += (unsigned int)got;
    }
    return 1;
}

static int ppa_fs_cache_write_exact(SceUID fd, const void *buffer,
                                    unsigned int bytes)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    unsigned int total = 0;
    while (total < bytes) {
        unsigned int chunk = bytes - total;
        int wrote;
        if (chunk > PPA_FS_CACHE_WRITE_CHUNK)
            chunk = PPA_FS_CACHE_WRITE_CHUNK;
        wrote = sceIoWrite(fd, cursor + total, chunk);
        if (wrote <= 0)
            return 0;
        total += (unsigned int)wrote;
    }
    return 1;
}

static int ppa_fs_cache_load_file(const char *filename)
{
    SceUID fd;
    ppa_fs_cache_file_header header;
    unsigned char *payload = 0;
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned int directory_index;
    int ok = 0;

    fd = sceIoOpen(filename, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return 0;
    if (!ppa_fs_cache_read_exact(fd, &header, sizeof(header)))
        goto done;
    if (header.magic != PPA_FS_CACHE_MAGIC ||
        header.version != PPA_FS_CACHE_VERSION ||
        header.header_bytes != sizeof(header) ||
        header.payload_bytes > PPA_FS_CACHE_MAX_RAM_BYTES ||
        header.directory_count > PPA_FS_CACHE_MAX_DIRECTORIES)
        goto done;
    payload = (unsigned char *)malloc(header.payload_bytes ?
                                      header.payload_bytes : 1U);
    if (payload == 0)
        goto done;
    if (header.payload_bytes != 0 &&
        !ppa_fs_cache_read_exact(fd, payload, header.payload_bytes))
        goto done;
    if (ppa_fs_cache_crc32(payload, header.payload_bytes) !=
        header.payload_crc32)
        goto done;

    if (!ppa_fs_cache_lock())
        goto done;
    ppa_fs_cache_clear_locked();
    cursor = payload;
    end = payload + header.payload_bytes;
    for (directory_index = 0;
         directory_index < header.directory_count;
         ++directory_index) {
        ppa_fs_cache_disk_directory disk_entry;
        char path[512];
        char short_path[512];
        ppa_fs_cache_directory *entry;
        unsigned char *blob;

        if ((size_t)(end - cursor) < sizeof(disk_entry))
            goto invalid_locked;
        memcpy(&disk_entry, cursor, sizeof(disk_entry));
        cursor += sizeof(disk_entry);
        if (disk_entry.path_bytes == 0 || disk_entry.path_bytes > sizeof(path) ||
            disk_entry.short_path_bytes == 0 ||
            disk_entry.short_path_bytes > sizeof(short_path) ||
            disk_entry.blob_bytes > PPA_FS_CACHE_MAX_RAM_BYTES ||
            (size_t)(end - cursor) <
                (size_t)disk_entry.path_bytes + disk_entry.short_path_bytes +
                disk_entry.blob_bytes)
            goto invalid_locked;
        /* Validate the serialized terminators before copying; forcing the
         * destination's last byte to zero must not disguise corrupt paths. */
        if (cursor[disk_entry.path_bytes - 1U] != 0 ||
            memchr(cursor, 0, disk_entry.path_bytes - 1U) != 0 ||
            cursor[disk_entry.path_bytes + disk_entry.short_path_bytes - 1U] != 0 ||
            memchr(cursor + disk_entry.path_bytes, 0,
                   disk_entry.short_path_bytes - 1U) != 0 ||
            (disk_entry.item_count == 0 && disk_entry.blob_bytes != 0) ||
            disk_entry.blob_bytes / (sizeof(ppa_fs_cache_item_header) + 4U) <
                disk_entry.item_count)
            goto invalid_locked;
        memcpy(path, cursor, disk_entry.path_bytes);
        path[sizeof(path) - 1U] = 0;
        cursor += disk_entry.path_bytes;
        memcpy(short_path, cursor, disk_entry.short_path_bytes);
        short_path[sizeof(short_path) - 1U] = 0;
        cursor += disk_entry.short_path_bytes;
        if (path[disk_entry.path_bytes - 1U] != 0 ||
            short_path[disk_entry.short_path_bytes - 1U] != 0 ||
            !ppa_fs_cache_path_supported(path) ||
            !ppa_fs_cache_path_supported(short_path))
            goto invalid_locked;
        entry = ppa_fs_cache_free_slot_locked();
        if (entry == 0)
            goto invalid_locked;
        blob = 0;
        if (disk_entry.blob_bytes != 0) {
            blob = (unsigned char *)malloc(disk_entry.blob_bytes);
            if (blob == 0)
                goto invalid_locked;
            memcpy(blob, cursor, disk_entry.blob_bytes);
        }
        cursor += disk_entry.blob_bytes;
        memset(entry, 0, sizeof(*entry));
        entry->hash = ppa_fs_cache_hash(path, short_path);
        entry->item_count = disk_entry.item_count;
        entry->complete = disk_entry.complete ? 1U : 0U;
        entry->in_use = 1U;
        entry->blob_bytes = disk_entry.blob_bytes;
        entry->blob = blob;
        strncpy(entry->path, path, sizeof(entry->path) - 1U);
        strncpy(entry->short_path, short_path,
                sizeof(entry->short_path) - 1U);
        g_fs_cache.directory_count++;
        g_fs_cache.item_count += disk_entry.item_count;
        g_fs_cache.blob_bytes += disk_entry.blob_bytes;
        if (ppa_fs_cache_ram_usage_locked() > PPA_FS_CACHE_MAX_RAM_BYTES)
            goto invalid_locked;
    }
    if (cursor != end || g_fs_cache.item_count != header.item_count)
        goto invalid_locked;
    g_fs_cache.loaded_from_disk = 1;
    g_fs_cache.no_cache_mode = 0;
    g_fs_cache.dirty = 0;
    ppa_fs_cache_unlock();
    ok = 1;
    goto done;

invalid_locked:
    ppa_fs_cache_clear_locked();
    ppa_fs_cache_unlock();

done:
    if (payload != 0)
        free(payload);
    sceIoClose(fd);
    if (!ok)
        ;
    else
        ;
    return ok;
}

static int ppa_fs_cache_load_disk(void)
{
    if (ppa_fs_cache_load_file(PPA_FS_CACHE_DISK_PATH)) {
        g_fs_cache.loaded_from_backup = 0;
        return 1;
    }
    if (ppa_fs_cache_load_file(PPA_FS_CACHE_BACKUP_PATH)) {
        g_fs_cache.loaded_from_backup = 1;
        return 1;
    }
    return 0;
}

static int ppa_fs_cache_build_payload(unsigned char **payload_out,
                                      unsigned int *payload_bytes_out,
                                      ppa_fs_cache_file_header *header_out)
{
    unsigned int payload_bytes = 0;
    unsigned int i;
    unsigned char *payload;
    unsigned char *cursor;

    if (payload_out == 0 || payload_bytes_out == 0 || header_out == 0)
        return 0;
    *payload_out = 0;
    *payload_bytes_out = 0;
    memset(header_out, 0, sizeof(*header_out));
    if (!ppa_fs_cache_lock())
        return 0;
    if (g_fs_cache.no_cache_mode || g_fs_cache.directory_count == 0) {
        ppa_fs_cache_unlock();
        return 0;
    }
    for (i = 0; i < PPA_FS_CACHE_MAX_DIRECTORIES; ++i) {
        ppa_fs_cache_directory *entry = &g_fs_cache.directories[i];
        unsigned int path_bytes;
        unsigned int short_path_bytes;
        if (!entry->in_use)
            continue;
        path_bytes = (unsigned int)strlen(entry->path) + 1U;
        short_path_bytes = (unsigned int)strlen(entry->short_path) + 1U;
        if (payload_bytes > 0xffffffffU -
            (unsigned int)sizeof(ppa_fs_cache_disk_directory) - path_bytes -
            short_path_bytes - entry->blob_bytes) {
            ppa_fs_cache_unlock();
            return 0;
        }
        payload_bytes += (unsigned int)sizeof(ppa_fs_cache_disk_directory) +
                         path_bytes + short_path_bytes + entry->blob_bytes;
    }
    if (payload_bytes > PPA_FS_CACHE_MAX_RAM_BYTES) {
        ppa_fs_cache_unlock();
        return 0;
    }
    payload = (unsigned char *)malloc(payload_bytes ? payload_bytes : 1U);
    if (payload == 0) {
        ppa_fs_cache_unlock();
        return 0;
    }
    cursor = payload;
    for (i = 0; i < PPA_FS_CACHE_MAX_DIRECTORIES; ++i) {
        ppa_fs_cache_directory *entry = &g_fs_cache.directories[i];
        ppa_fs_cache_disk_directory disk_entry;
        unsigned int path_bytes;
        unsigned int short_path_bytes;
        if (!entry->in_use)
            continue;
        path_bytes = (unsigned int)strlen(entry->path) + 1U;
        short_path_bytes = (unsigned int)strlen(entry->short_path) + 1U;
        memset(&disk_entry, 0, sizeof(disk_entry));
        disk_entry.path_bytes = (uint16_t)path_bytes;
        disk_entry.short_path_bytes = (uint16_t)short_path_bytes;
        disk_entry.item_count = entry->item_count;
        disk_entry.complete = entry->complete;
        disk_entry.blob_bytes = entry->blob_bytes;
        memcpy(cursor, &disk_entry, sizeof(disk_entry));
        cursor += sizeof(disk_entry);
        memcpy(cursor, entry->path, path_bytes);
        cursor += path_bytes;
        memcpy(cursor, entry->short_path, short_path_bytes);
        cursor += short_path_bytes;
        if (entry->blob_bytes != 0) {
            memcpy(cursor, entry->blob, entry->blob_bytes);
            cursor += entry->blob_bytes;
        }
    }
    header_out->magic = PPA_FS_CACHE_MAGIC;
    header_out->version = PPA_FS_CACHE_VERSION;
    header_out->header_bytes = sizeof(*header_out);
    header_out->payload_bytes = payload_bytes;
    header_out->payload_crc32 = ppa_fs_cache_crc32(payload, payload_bytes);
    header_out->directory_count = g_fs_cache.directory_count;
    header_out->item_count = g_fs_cache.item_count;
    ppa_fs_cache_unlock();

    *payload_out = payload;
    *payload_bytes_out = payload_bytes;
    return 1;
}

static int ppa_fs_cache_write_disk_if_dirty(void)
{
    ppa_fs_cache_file_header header;
    unsigned char *payload = 0;
    unsigned int payload_bytes = 0;
    SceUID fd = -1;
    int ok = 0;
    int moved_old = 0;
    SceIoStat old_stat;

    /* A valid unchanged image needs no write. A subsequent playback after
     * visiting a new directory must not discard those new entries. */
    if (g_fs_cache.loaded_from_disk && !g_fs_cache.dirty)
        return 1;
    if (!ppa_fs_cache_build_payload(&payload, &payload_bytes, &header))
        return 0;

    (void)sceIoMkdir("ms0:/PSP", 0777);
    (void)sceIoMkdir("ms0:/PSP/SYSTEM", 0777);
    (void)sceIoRemove(PPA_FS_CACHE_TEMP_PATH);
    fd = sceIoOpen(PPA_FS_CACHE_TEMP_PATH,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        goto done;
    if (!ppa_fs_cache_write_exact(fd, &header, sizeof(header)) ||
        !ppa_fs_cache_write_exact(fd, payload, payload_bytes))
        goto done;
    sceIoClose(fd);
    fd = -1;
    /* Never discard the last complete CRC-checked snapshot before a new
     * snapshot is safely in place. A power cut between renames can still
     * recover the previous image through the backup path at next startup. */
    if (g_fs_cache.loaded_from_backup) {
        /* Preserve the last valid backup when the main failed validation. */
        (void)sceIoRemove(PPA_FS_CACHE_DISK_PATH);
    }
    else if (sceIoGetstat(PPA_FS_CACHE_DISK_PATH, &old_stat) >= 0) {
        if (sceIoRename(PPA_FS_CACHE_DISK_PATH,
                        PPA_FS_CACHE_BACKUP_PATH) >= 0)
            moved_old = 1;
        else {
            /* The main is still valid while an older backup is removed. */
            (void)sceIoRemove(PPA_FS_CACHE_BACKUP_PATH);
            if (sceIoRename(PPA_FS_CACHE_DISK_PATH,
                            PPA_FS_CACHE_BACKUP_PATH) >= 0)
                moved_old = 1;
        }
        if (!moved_old)
            goto done;
    }
    if (sceIoRename(PPA_FS_CACHE_TEMP_PATH, PPA_FS_CACHE_DISK_PATH) < 0) {
        if (moved_old)
            (void)sceIoRename(PPA_FS_CACHE_BACKUP_PATH,
                              PPA_FS_CACHE_DISK_PATH);
        goto done;
    }
    g_fs_cache.written_this_session = 1;
    g_fs_cache.loaded_from_disk = 1;
    g_fs_cache.loaded_from_backup = 0;
    g_fs_cache.dirty = 0;
    ok = 1;

 done:
    if (fd >= 0)
        sceIoClose(fd);
    if (!ok)
        (void)sceIoRemove(PPA_FS_CACHE_TEMP_PATH);
    if (payload != 0)
        free(payload);
    return ok;
}

int ppa_browser_fs_cache_init(int show_hidden,
                              int show_unknown,
                              file_type_ext_struct *filter)
{
    int loaded;
    if (g_fs_cache.initialized)
        return 1;
    memset(&g_fs_cache, 0, sizeof(g_fs_cache));
    g_fs_cache.lock = -1;
    g_fs_cache.thread = -1;
    g_fs_cache.show_hidden = show_hidden;
    g_fs_cache.show_unknown = show_unknown;
    g_fs_cache.filter = filter;
    g_fs_cache.lock = sceKernelCreateSema("ppa_fs_cache_lock", 0, 1, 1, 0);
    if (g_fs_cache.lock < 0)
        return 0;
    g_fs_cache.initialized = 1;
    loaded = PPA_FS_CACHE_DISK_PERSISTENCE ? ppa_fs_cache_load_disk() : 0;
    if (!loaded && PPA_FS_CACHE_STARTUP_PREFETCH)
        return ppa_browser_fs_cache_start_build();
    return 1;
}

int ppa_browser_fs_cache_start_build(void)
{
    SceUID thread;
    if (!g_fs_cache.initialized || g_fs_cache.no_cache_mode ||
        g_fs_cache.building || g_fs_cache.thread >= 0 ||
        g_fs_cache.directory_count != 0)
        return g_fs_cache.initialized && !g_fs_cache.no_cache_mode;
    g_fs_cache.stop = 0;
    g_fs_cache.suspended = 0;
    g_fs_cache.ram_cap_hit = 0;
    /* Count the hard limit from the app-init request, not from whenever this
     * deliberately low-priority thread first happens to be scheduled. */
    g_fs_cache.build_deadline_us =
        (uint64_t)sceKernelGetSystemTimeWide() +
        PPA_FS_CACHE_BUILD_DEADLINE_US;
    thread = ppa_thread_create(PPA_THREAD_BROWSER_CACHE,
                               "ppa_fs_cache_builder",
                               ppa_fs_cache_builder_thread,
                               PPA_FS_CACHE_THREAD_STACK, 0);
    if (thread < 0) {
        ppa_fs_cache_enter_no_cache_mode("thread_create");
        return 0;
    }
    g_fs_cache.thread = thread;
    if (sceKernelStartThread(thread, 0, 0) < 0) {
        sceKernelDeleteThread(thread);
        g_fs_cache.thread = -1;
        ppa_fs_cache_enter_no_cache_mode("thread_start");
        return 0;
    }
    return 1;
}

void ppa_browser_fs_cache_request_interactive_io(unsigned int generation)
{
    if (!g_fs_cache.initialized || generation == 0U)
        return;
    __sync_lock_test_and_set(&g_fs_cache.interactive_generation, generation);
}

void ppa_browser_fs_cache_finish_interactive_io(unsigned int generation)
{
    if (!g_fs_cache.initialized || generation == 0U)
        return;
    (void)__sync_bool_compare_and_swap(&g_fs_cache.interactive_generation,
                                       generation, 0U);
}

void ppa_browser_fs_cache_clear_interactive_io(void)
{
    if (g_fs_cache.initialized)
        __sync_lock_test_and_set(&g_fs_cache.interactive_generation, 0U);
}

void ppa_browser_fs_cache_cancel_build(int wait_for_idle)
{
    SceUID thread;
    if (!g_fs_cache.initialized)
        return;
    g_fs_cache.stop = 1;
    ppa_browser_fs_cache_clear_interactive_io();
    thread = g_fs_cache.thread;
    if (wait_for_idle && thread >= 0) {
        sceKernelWaitThreadEnd(thread, 0);
        sceKernelDeleteThread(thread);
        if (g_fs_cache.thread == thread)
            g_fs_cache.thread = -1;
        g_fs_cache.building = 0;
        g_fs_cache.build_deadline_us = 0;
    }
}

int ppa_browser_fs_cache_prepare_playback(void)
{
    int write_ok = 1;
    if (!g_fs_cache.initialized)
        return 0;
    g_fs_cache.ram_released_for_playback = 0;
    ppa_browser_fs_cache_cancel_build(1);
    if (PPA_FS_CACHE_DISK_PERSISTENCE &&
        !g_fs_cache.no_cache_mode && g_fs_cache.directory_count != 0)
        write_ok = ppa_fs_cache_write_disk_if_dirty();
    /* Do not destroy the only valid in-memory copy if an ms0 write failed. The
     * movie may use a little more RAM in this degraded case, but the browser
     * remains coherent and can retry on a later playback. */
    if (write_ok && ppa_fs_cache_lock()) {
        ppa_fs_cache_clear_locked();
        g_fs_cache.ram_released_for_playback = 1;
        ppa_fs_cache_unlock();
    }
    return write_ok;
}

int ppa_browser_fs_cache_restore_after_playback(void)
{
    if (!g_fs_cache.initialized)
        return 0;
    g_fs_cache.stop = 0;
    g_fs_cache.suspended = 0;
    if (!g_fs_cache.ram_released_for_playback)
        return !g_fs_cache.no_cache_mode;
    g_fs_cache.ram_released_for_playback = 0;
    if (ppa_fs_cache_lock()) {
        ppa_fs_cache_clear_locked();
        ppa_fs_cache_unlock();
    }
    /* A missing or damaged snapshot merely means demand-driven rebuilding.
     * It must never disable the in-memory cache for the rest of the session. */
    if (PPA_FS_CACHE_DISK_PERSISTENCE)
        (void)ppa_fs_cache_load_disk();
    return 1;
}

void ppa_browser_fs_cache_suspend(void)
{
    if (!g_fs_cache.initialized)
        return;
    g_fs_cache.suspended = 1;
    g_fs_cache.stop = 1;
}

void ppa_browser_fs_cache_resume(void)
{
    if (!g_fs_cache.initialized)
        return;
    ppa_browser_fs_cache_cancel_build(1);
    g_fs_cache.stop = 0;
    g_fs_cache.suspended = 0;
    if (PPA_FS_CACHE_DISK_PERSISTENCE &&
        g_fs_cache.directory_count == 0 && !g_fs_cache.no_cache_mode)
        (void)ppa_fs_cache_load_disk();
}

unsigned int ppa_browser_fs_cache_ram_bytes(void)
{
    unsigned int bytes = 0;
    if (ppa_fs_cache_lock()) {
        bytes = ppa_fs_cache_ram_usage_locked();
        ppa_fs_cache_unlock();
    }
    return bytes;
}

int ppa_browser_fs_cache_available(void)
{
    int available = 0;
    if (ppa_fs_cache_lock()) {
        available = !g_fs_cache.no_cache_mode &&
                    g_fs_cache.directory_count != 0;
        ppa_fs_cache_unlock();
    }
    return available;
}

void ppa_browser_fs_cache_shutdown(void)
{
    if (!g_fs_cache.initialized)
        return;
    ppa_browser_fs_cache_cancel_build(1);
    /* Persist recently visited directories on a clean exit as well as at
     * playback handoff. No background card scan or write runs in the menu. */
    if (PPA_FS_CACHE_DISK_PERSISTENCE && !g_fs_cache.suspended &&
        !g_fs_cache.no_cache_mode && g_fs_cache.directory_count != 0 &&
        g_fs_cache.dirty) {
        (void)ppa_fs_cache_write_disk_if_dirty();
    }
    if (ppa_fs_cache_lock()) {
        ppa_fs_cache_clear_locked();
        ppa_fs_cache_unlock();
    }
    if (g_fs_cache.lock >= 0)
        sceKernelDeleteSema(g_fs_cache.lock);
    memset(&g_fs_cache, 0, sizeof(g_fs_cache));
    g_fs_cache.lock = -1;
    g_fs_cache.thread = -1;
}
