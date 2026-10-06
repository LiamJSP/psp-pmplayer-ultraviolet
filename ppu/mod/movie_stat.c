#include "movie_stat.h"
#include "mp4_play.h"
#include "mkv_play.h"

#include <pspiofilemgr.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_STAT_MAGIC   0x50504153U /* PPAS */
#define MOVIE_STAT_VERSION 2U

#define TIMECODE_FILENAME "ms0:/ppa_timecodes.xml"
#define TIMECODE_TEMP_FILENAME "ms0:/ppa_timecodes.xml.tmp"
#define TIMECODE_BACKUP_FILENAME "ms0:/ppa_timecodes.xml.bak"
#define TIMECODE_MAX 10
#define TIMECODE_XML_BUFFER_SIZE 16384
#define TIMECODE_PATH_SIZE 512

struct movie_stat_record_legacy {
    char hash[16];
    int resume_pos;
    int audio_stream;
    int volume_boost;
    int aspect_ratio;
    int zoom;
    int luminosity_boost;
    int subtitle;
    int subtitle_format;
    int subtitle_fontcolor;
    int subtitle_bordercolor;
};

struct movie_stat_record {
    char hash[16];
    int32_t resume_pos;
    int32_t audio_stream;
    int32_t volume_boost;
    int32_t aspect_ratio;
    int32_t zoom;
    int32_t luminosity_boost;
    int32_t subtitle;
    int32_t subtitle_format;
    int32_t subtitle_fontcolor;
    int32_t subtitle_bordercolor;
    uint32_t generation;
};

struct movie_stat_header {
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t record_count;
    uint32_t checksum;
};

struct movie_stat_view {
    char *hash;
    const char *movie_file;
    unsigned int *resume_pos;
    unsigned int *last_keyframe_pos;
    int *current_timestamp;
    unsigned int duration_ms;
    unsigned int *audio_stream;
    int audio_stream_count;
    unsigned int *volume_boost;
    unsigned int *aspect_ratio;
    unsigned int *zoom;
    unsigned int *luminosity_boost;
    unsigned int *subtitle;
    int subtitle_count;
    unsigned int *subtitle_format;
    unsigned int *subtitle_fontcolor;
    unsigned int *subtitle_bordercolor;
};

struct timecode_record {
    unsigned char hash[16];
    int32_t position_ms;
    uint32_t generation;
    char path[TIMECODE_PATH_SIZE];
    int used;
};

static char stat_filename[1024];
static char timecode_xml_buffer[TIMECODE_XML_BUFFER_SIZE];

struct active_resume_record {
    unsigned char hash[16];
    char path[TIMECODE_PATH_SIZE];
    volatile int *current_timestamp;
    volatile int active;
};

static struct active_resume_record active_resume;

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static uint32_t stat_checksum(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 2166136261U;
    size_t i;
    for (i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 16777619U;
    }
    return hash;
}

static int read_exact(SceUID fd, void *buffer, unsigned int size)
{
    uint8_t *p = (uint8_t *)buffer;
    unsigned int done = 0;
    while (done < size) {
        int got = sceIoRead(fd, p + done, size - done);
        if (got <= 0) return 0;
        done += (unsigned int)got;
    }
    return 1;
}

static int write_exact(SceUID fd, const void *buffer, unsigned int size)
{
    const uint8_t *p = (const uint8_t *)buffer;
    unsigned int done = 0;
    while (done < size) {
        int put = sceIoWrite(fd, p + done, size - done);
        if (put <= 0) return 0;
        done += (unsigned int)put;
    }
    return 1;
}

static void convert_legacy(struct movie_stat_record *dst,
                           const struct movie_stat_record_legacy *src)
{
    int i;
    for (i = 0; i < MAX_MOVIE_STAT; ++i) {
        memcpy(dst[i].hash, src[i].hash, sizeof(dst[i].hash));
        dst[i].resume_pos = src[i].resume_pos;
        dst[i].audio_stream = src[i].audio_stream;
        dst[i].volume_boost = src[i].volume_boost;
        dst[i].aspect_ratio = src[i].aspect_ratio;
        dst[i].zoom = src[i].zoom;
        dst[i].luminosity_boost = src[i].luminosity_boost;
        dst[i].subtitle = src[i].subtitle;
        dst[i].subtitle_format = src[i].subtitle_format;
        dst[i].subtitle_fontcolor = src[i].subtitle_fontcolor;
        dst[i].subtitle_bordercolor = src[i].subtitle_bordercolor;
        dst[i].generation = (uint32_t)i + 1U;
    }
}

static int read_records(struct movie_stat_record records[MAX_MOVIE_STAT])
{
    SceUID fd;
    struct movie_stat_header header;
    int ok = 0;
    memset(records, 0, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT);
    if (stat_filename[0] == 0) return 0;
    fd = sceIoOpen(stat_filename, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;

    {
        int32_t file_size = sceIoLseek32(fd, 0, PSP_SEEK_END);
        const int32_t current_size = (int32_t)(sizeof(header) +
            sizeof(struct movie_stat_record) * MAX_MOVIE_STAT);
        const int32_t legacy_size = (int32_t)(
            sizeof(struct movie_stat_record_legacy) * MAX_MOVIE_STAT);
        if (sceIoLseek32(fd, 0, PSP_SEEK_SET) != 0)
            file_size = -1;

        if (file_size == current_size &&
            read_exact(fd, &header, sizeof(header)) &&
            header.magic == MOVIE_STAT_MAGIC &&
            header.version == MOVIE_STAT_VERSION &&
            header.record_size == sizeof(struct movie_stat_record) &&
            header.record_count == MAX_MOVIE_STAT &&
            read_exact(fd, records, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT) &&
            header.checksum == stat_checksum(records, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT)) {
            ok = 1;
        }
        else if (file_size == legacy_size) {
            struct movie_stat_record_legacy legacy[MAX_MOVIE_STAT];
            if (sceIoLseek32(fd, 0, PSP_SEEK_SET) == 0 &&
                read_exact(fd, legacy, sizeof(legacy))) {
                convert_legacy(records, legacy);
                ok = 1;
            }
        }
    }
    sceIoClose(fd);
    if (!ok) memset(records, 0, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT);
    return ok;
}

static int write_records_atomic(const struct movie_stat_record records[MAX_MOVIE_STAT])
{
    char temp[1060];
    char backup[1060];
    struct movie_stat_header header;
    SceUID fd;
    int ok;
    if (stat_filename[0] == 0) return 0;
    if (snprintf(temp, sizeof(temp), "%s.tmp", stat_filename) >= (int)sizeof(temp) ||
        snprintf(backup, sizeof(backup), "%s.bak", stat_filename) >= (int)sizeof(backup))
        return 0;
    header.magic = MOVIE_STAT_MAGIC;
    header.version = MOVIE_STAT_VERSION;
    header.record_size = sizeof(struct movie_stat_record);
    header.record_count = MAX_MOVIE_STAT;
    header.checksum = stat_checksum(records, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT);

    (void)sceIoRemove(temp);
    fd = sceIoOpen(temp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return 0;
    ok = write_exact(fd, &header, sizeof(header)) &&
         write_exact(fd, records, sizeof(struct movie_stat_record) * MAX_MOVIE_STAT);
    sceIoClose(fd);
    if (!ok) { (void)sceIoRemove(temp); return 0; }

    (void)sceIoRemove(backup);
    (void)sceIoRename(stat_filename, backup);
    if (sceIoRename(temp, stat_filename) < 0) {
        (void)sceIoRename(backup, stat_filename);
        (void)sceIoRemove(temp);
        return 0;
    }
    (void)sceIoRemove(backup);
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hash_from_hex(unsigned char hash[16], const char *text)
{
    int i;
    if (!text) return 0;
    for (i = 0; i < 16; ++i) {
        int hi = hex_value(text[i * 2]);
        int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        hash[i] = (unsigned char)((hi << 4) | lo);
    }
    return text[32] == 0;
}

static void hash_to_hex(char out[33], const unsigned char hash[16])
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; ++i) {
        out[i * 2] = digits[(hash[i] >> 4) & 15];
        out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[32] = 0;
}

static int xml_attribute(const char *entry, const char *entry_end,
                         const char *name, char *out, unsigned int out_size)
{
    char pattern[48];
    const char *p;
    const char *q;
    size_t n;
    if (!entry || !entry_end || !name || !out || out_size == 0U)
        return 0;
    if (snprintf(pattern, sizeof(pattern), "%s=\"", name) >= (int)sizeof(pattern))
        return 0;
    p = strstr(entry, pattern);
    if (!p || p >= entry_end) return 0;
    p += strlen(pattern);
    q = strchr(p, '"');
    if (!q || q > entry_end) return 0;
    n = (size_t)(q - p);
    if (n >= out_size) n = out_size - 1U;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static void xml_unescape(char *out, unsigned int out_size, const char *in)
{
    unsigned int used = 0;
    if (!out || out_size == 0U) return;
    while (in && *in && used + 1U < out_size) {
        char c = *in++;
        if (c == '&') {
            if (strncmp(in, "amp;", 4) == 0) { c = '&'; in += 4; }
            else if (strncmp(in, "quot;", 5) == 0) { c = '"'; in += 5; }
            else if (strncmp(in, "apos;", 5) == 0) { c = '\''; in += 5; }
            else if (strncmp(in, "lt;", 3) == 0) { c = '<'; in += 3; }
            else if (strncmp(in, "gt;", 3) == 0) { c = '>'; in += 3; }
        }
        out[used++] = c;
    }
    out[used] = 0;
}

static int timecode_read(struct timecode_record records[TIMECODE_MAX])
{
    SceUID fd;
    int got;
    const char *cursor;
    int count = 0;
    memset(records, 0, sizeof(struct timecode_record) * TIMECODE_MAX);
    fd = sceIoOpen(TIMECODE_FILENAME, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    got = sceIoRead(fd, timecode_xml_buffer, TIMECODE_XML_BUFFER_SIZE - 1);
    sceIoClose(fd);
    if (got <= 0) return 0;
    timecode_xml_buffer[got] = 0;
    cursor = timecode_xml_buffer;
    while (count < TIMECODE_MAX) {
        const char *entry = strstr(cursor, "<entry ");
        const char *end;
        char hash_text[40];
        char pos_text[32];
        char gen_text[32];
        char path_text[2048];
        struct timecode_record *record;
        if (!entry) break;
        end = strstr(entry, "/>");
        if (!end) break;
        record = &records[count];
        if (xml_attribute(entry, end, "hash", hash_text, sizeof(hash_text)) &&
            xml_attribute(entry, end, "position_ms", pos_text, sizeof(pos_text)) &&
            hash_from_hex(record->hash, hash_text)) {
            long pos = strtol(pos_text, 0, 10);
            if (pos < 0) pos = 0;
            if (pos > 0x7fffffffL) pos = 0x7fffffffL;
            record->position_ms = (int32_t)pos;
            if (xml_attribute(entry, end, "generation", gen_text, sizeof(gen_text))) {
                unsigned long gen = strtoul(gen_text, 0, 10);
                record->generation = gen > 0xffffffffUL ? 0xffffffffU : (uint32_t)gen;
            }
            if (record->generation == 0U)
                record->generation = (uint32_t)count + 1U;
            if (xml_attribute(entry, end, "path", path_text, sizeof(path_text)))
                xml_unescape(record->path, sizeof(record->path), path_text);
            record->used = 1;
            ++count;
        }
        cursor = end + 2;
    }
    return count;
}

static int xml_write_escaped(SceUID fd, const char *text)
{
    const char *p = text ? text : "";
    while (*p) {
        const char *entity = 0;
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&apos;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        default: break;
        }
        if (entity) {
            if (!write_exact(fd, entity, (unsigned int)strlen(entity))) return 0;
        }
        else if (!write_exact(fd, p, 1U)) return 0;
        ++p;
    }
    return 1;
}

static int timecode_write_atomic(const struct timecode_record records[TIMECODE_MAX])
{
    static const char header[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ppa_timecodes version=\"1\" max_entries=\"10\">\n";
    static const char footer[] = "</ppa_timecodes>\n";
    SceUID fd;
    int ok = 1;
    int i;
    (void)sceIoRemove(TIMECODE_TEMP_FILENAME);
    fd = sceIoOpen(TIMECODE_TEMP_FILENAME,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return 0;
    ok = write_exact(fd, header, sizeof(header) - 1U);
    for (i = 0; ok && i < TIMECODE_MAX; ++i) {
        char hash_text[33];
        char prefix[160];
        if (!records[i].used) continue;
        hash_to_hex(hash_text, records[i].hash);
        snprintf(prefix, sizeof(prefix),
                 "  <entry hash=\"%s\" position_ms=\"%ld\" generation=\"%lu\" path=\"",
                 hash_text, (long)records[i].position_ms,
                 (unsigned long)records[i].generation);
        ok = write_exact(fd, prefix, (unsigned int)strlen(prefix)) &&
             xml_write_escaped(fd, records[i].path) &&
             write_exact(fd, "\"/>\n", 4U);
    }
    if (ok) ok = write_exact(fd, footer, sizeof(footer) - 1U);
    sceIoClose(fd);
    if (!ok) { (void)sceIoRemove(TIMECODE_TEMP_FILENAME); return 0; }

    (void)sceIoRemove(TIMECODE_BACKUP_FILENAME);
    (void)sceIoRename(TIMECODE_FILENAME, TIMECODE_BACKUP_FILENAME);
    if (sceIoRename(TIMECODE_TEMP_FILENAME, TIMECODE_FILENAME) < 0) {
        (void)sceIoRename(TIMECODE_BACKUP_FILENAME, TIMECODE_FILENAME);
        (void)sceIoRemove(TIMECODE_TEMP_FILENAME);
        return 0;
    }
    (void)sceIoRemove(TIMECODE_BACKUP_FILENAME);
    return 1;
}

static int timecode_path_matches(const char *stored_path, const char *movie_path)
{
    if (!stored_path || stored_path[0] == 0)
        return 1;
    if (!movie_path)
        return 0;
    return strcmp(stored_path, movie_path) == 0;
}

static int timecode_lookup(const unsigned char hash[16], const char *movie_path)
{
    struct timecode_record records[TIMECODE_MAX];
    int count = timecode_read(records);
    int i;
    int result = 0;
    uint32_t newest = 0;
    for (i = 0; i < count; ++i) {
        if (records[i].used && memcmp(records[i].hash, hash, 16) == 0 &&
            timecode_path_matches(records[i].path, movie_path) &&
            records[i].generation >= newest) {
            newest = records[i].generation;
            result = records[i].position_ms;
        }
    }
    return result;
}

static void timecode_save(const unsigned char hash[16], const char *path,
                          int position_ms)
{
    struct timecode_record records[TIMECODE_MAX];
    int count = timecode_read(records);
    int slot = -1;
    int free_slot = -1;
    int oldest = -1;
    uint32_t oldest_generation = 0xffffffffU;
    uint32_t max_generation = 0;
    int i;

    if (position_ms < 0) position_ms = 0;
    for (i = 0; i < TIMECODE_MAX; ++i) {
        if (i >= count || !records[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (records[i].generation > max_generation)
            max_generation = records[i].generation;
        if (records[i].generation < oldest_generation) {
            oldest_generation = records[i].generation;
            oldest = i;
        }
        if (memcmp(records[i].hash, hash, 16) == 0 &&
            timecode_path_matches(records[i].path, path))
            slot = i;
    }
    if (slot < 0) slot = free_slot >= 0 ? free_slot : oldest;
    if (slot < 0) slot = 0;
    if (max_generation == 0xffffffffU) {
        max_generation = 0;
        for (i = 0; i < TIMECODE_MAX; ++i) {
            if (records[i].used)
                records[i].generation = ++max_generation;
        }
    }
    memset(&records[slot], 0, sizeof(records[slot]));
    memcpy(records[slot].hash, hash, 16);
    records[slot].position_ms = (int32_t)position_ms;
    records[slot].generation = max_generation + 1U;
    if (path) {
        strncpy(records[slot].path, path, sizeof(records[slot].path) - 1U);
        records[slot].path[sizeof(records[slot].path) - 1U] = 0;
    }
    records[slot].used = 1;
    (void)timecode_write_atomic(records);
}

static int validated_resume(const struct movie_stat_view *view, int resume)
{
    if (resume <= 0) return 0;
    if (view->duration_ms != 0) {
        uint32_t value = (uint32_t)resume;
        if (value >= view->duration_ms) return 0;
        if (view->duration_ms - value <= 3000U) return 0;
    }
    return resume;
}

static void load_view(const struct movie_stat_view *view)
{
    struct movie_stat_record records[MAX_MOVIE_STAT];
    int i;
    if (!view || !view->hash) return;
    (void)read_records(records);
    for (i = 0; i < MAX_MOVIE_STAT; ++i) {
        const struct movie_stat_record *record = &records[i];
        if (memcmp(view->hash, record->hash, sizeof(record->hash)) != 0) continue;
        *view->audio_stream = clamp_int(record->audio_stream, 0,
            view->audio_stream_count > 0 ? view->audio_stream_count - 1 : 0);
        *view->volume_boost = clamp_int(record->volume_boost, 0, 6);
        *view->aspect_ratio = clamp_int(record->aspect_ratio, 0,
            number_of_aspect_ratios > 0 ? number_of_aspect_ratios - 1 : 0);
        *view->zoom = clamp_int(record->zoom, 100, 200);
        *view->luminosity_boost = clamp_int(record->luminosity_boost, 0,
            number_of_luminosity_boosts > 0 ? number_of_luminosity_boosts - 1 : 0);
        *view->subtitle = clamp_int(record->subtitle, 0, view->subtitle_count);
        *view->subtitle_format = clamp_int(record->subtitle_format, 0, 1);
        *view->subtitle_fontcolor = clamp_int(record->subtitle_fontcolor, 0,
            NUMBER_OF_FONTCOLORS > 0 ? NUMBER_OF_FONTCOLORS - 1 : 0);
        *view->subtitle_bordercolor = clamp_int(record->subtitle_bordercolor, 0,
            NUMBER_OF_BORDERCOLORS > 0 ? NUMBER_OF_BORDERCOLORS - 1 : 0);
        break;
    }
    *view->resume_pos = (unsigned int)validated_resume(
        view, timecode_lookup((const unsigned char *)view->hash, view->movie_file));
}

static void save_view(const struct movie_stat_view *view)
{
    struct movie_stat_record records[MAX_MOVIE_STAT];
    uint32_t max_generation = 0;
    int slot = -1;
    int empty = -1;
    int oldest = 0;
    int current_ms;
    int i;
    if (!view || !view->hash) return;
    current_ms = view->current_timestamp ? *view->current_timestamp : 0;
    if (current_ms < 0) current_ms = 0;
    (void)read_records(records);
    for (i = 0; i < MAX_MOVIE_STAT; ++i) {
        if (records[i].generation > max_generation) max_generation = records[i].generation;
        if (memcmp(view->hash, records[i].hash, sizeof(records[i].hash)) == 0) slot = i;
        if (records[i].generation == 0 && empty < 0) empty = i;
        if (records[i].generation < records[oldest].generation) oldest = i;
    }
    if (slot < 0) slot = empty >= 0 ? empty : oldest;
    memset(&records[slot], 0, sizeof(records[slot]));
    memcpy(records[slot].hash, view->hash, sizeof(records[slot].hash));
    records[slot].resume_pos = (int32_t)validated_resume(view, current_ms);
    records[slot].audio_stream = (int32_t)*view->audio_stream;
    records[slot].volume_boost = (int32_t)*view->volume_boost;
    records[slot].aspect_ratio = (int32_t)*view->aspect_ratio;
    records[slot].zoom = (int32_t)*view->zoom;
    records[slot].luminosity_boost = (int32_t)*view->luminosity_boost;
    records[slot].subtitle = (int32_t)*view->subtitle;
    records[slot].subtitle_format = (int32_t)*view->subtitle_format;
    records[slot].subtitle_fontcolor = (int32_t)*view->subtitle_fontcolor;
    records[slot].subtitle_bordercolor = (int32_t)*view->subtitle_bordercolor;
    records[slot].generation = max_generation == 0xffffffffU ? 1U : max_generation + 1U;
    (void)write_records_atomic(records);
    timecode_save((const unsigned char *)view->hash, view->movie_file, current_ms);
}

void init_movie_stat(const char *path)
{
    if (!path) { stat_filename[0] = 0; return; }
    strncpy(stat_filename, path, sizeof(stat_filename) - 1U);
    stat_filename[sizeof(stat_filename) - 1U] = 0;
}

void movie_stat_active_begin(const char hash[16], const char *movie_file,
                             volatile int *current_timestamp)
{
    active_resume.active = 0;
    active_resume.current_timestamp = 0;
    memset(active_resume.hash, 0, sizeof(active_resume.hash));
    memset(active_resume.path, 0, sizeof(active_resume.path));
    if (hash == 0 || current_timestamp == 0)
        return;
    memcpy(active_resume.hash, hash, sizeof(active_resume.hash));
    if (movie_file != 0) {
        strncpy(active_resume.path, movie_file, sizeof(active_resume.path) - 1U);
        active_resume.path[sizeof(active_resume.path) - 1U] = 0;
    }
    active_resume.current_timestamp = current_timestamp;
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#endif
    active_resume.active = 1;
}

void movie_stat_active_end(volatile int *current_timestamp)
{
    if (active_resume.active &&
        active_resume.current_timestamp == current_timestamp) {
        active_resume.active = 0;
#if defined(__mips__)
        __asm__ volatile("sync" ::: "memory");
#endif
        active_resume.current_timestamp = 0;
    }
}

void movie_stat_emergency_save(void)
{
    unsigned char hash[16];
    char path[TIMECODE_PATH_SIZE];
    volatile int *timestamp_ptr;
    int position_ms;

    if (!active_resume.active)
        return;
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#endif
    timestamp_ptr = active_resume.current_timestamp;
    if (timestamp_ptr == 0)
        return;
    memcpy(hash, active_resume.hash, sizeof(hash));
    memcpy(path, active_resume.path, sizeof(path));
    path[sizeof(path) - 1U] = 0;
    position_ms = *timestamp_ptr;
    if (position_ms < 0)
        position_ms = 0;
    timecode_save(hash, path, position_ms);
}

#define DEFINE_STAT_ADAPTER(prefix, type) \
static struct movie_stat_view prefix##_view(struct type *p) \
{ \
    struct movie_stat_view view = { \
        p->hash, p->movie_file, &p->resume_pos, &p->last_keyframe_pos, \
        &p->current_timestamp, p->decoder.reader.file.duration_ms, \
        &p->audio_stream, p->decoder.reader.file.audio_tracks, \
        &p->volume_boost, &p->aspect_ratio, &p->zoom, \
        &p->luminosity_boost, &p->subtitle, p->subtitle_count, \
        &p->subtitle_format, &p->subtitle_fontcolor, &p->subtitle_bordercolor \
    }; \
    return view; \
} \
void prefix##_stat_load(struct type *p) \
{ \
    struct movie_stat_view view; if (!p) return; view = prefix##_view(p); load_view(&view); \
} \
void prefix##_stat_save(struct type *p) \
{ \
    struct movie_stat_view view; if (!p) return; view = prefix##_view(p); save_view(&view); \
}

DEFINE_STAT_ADAPTER(mp4, mp4_play_struct)
DEFINE_STAT_ADAPTER(mkv, mkv_play_struct)
