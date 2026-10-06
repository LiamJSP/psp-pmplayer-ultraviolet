#include "PlaybackUi.h"
#include "ui_i18n.h"
#include "ftfont.h"
#include "common/ppa_playback_session.h"
#include "common/ppa_cache.h"
#include "common/mem64.h"
#include "mod/cpu_clock.h"
#include "mod/gu_draw.h"
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspthreadman.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

static UiI18n *g_ui;
static Image *g_health, *g_health_assets, *g_message;
/* A/V uses 16-bit pixel words; audio-only keeps its 8888 static blit.
 * Neither mode retains both output representations. GE reads only after CPU
 * publication and its existing producer fence protects the next update. */
enum { HEALTH_PITCH = 512, HEALTH_HEIGHT = 96, HEALTH_MAX_RECTS = 120 };
static uint16_t *g_health_texture, *g_health_assets_texture;
struct HealthRect { short x0, y0, x1, y1; };
static HealthRect g_health_rects[HEALTH_MAX_RECTS];
static int g_health_rect_count;
static uint64_t g_next_health_us;
static int g_message_drawn;

/* All shaping/font I/O happens before the movie's workers start. The producer
 * composes a bounded snapshot from these immutable rows and digit cells; it
 * never measures, shapes, allocates or outlines text in the playback loop. */
enum { HEALTH_ROW = 22, HEALTH_REASON_COUNT = 7, HEALTH_DIGITS_Y = 220 };
struct HealthField { int x, y, cells; };
struct HealthRun { char text[256]; int field, width; };
static HealthField g_fields[5];
static int g_digit_width;
static const char * const g_reason_keys[HEALTH_REASON_COUNT] = {
    "health.paused", "health.audio_only", "health.waiting",
    "health.decode_pressure", "health.sync_pressure",
    "health.compatibility", "health.healthy"
};
static const char * const g_reason_fallbacks[HEALTH_REASON_COUNT] = {
    "Paused; resume with Square", "Audio only; video decoder is off",
    "Waiting for audio output", "Decode exceeds the frame budget",
    "Audio and video are drifting apart", "H264X conversion may reduce fidelity",
    "Playback is within current budgets"
};

const char *ppa_playback_ui_text(const char *key, const char *fallback)
{ return g_ui ? g_ui->getText(key, fallback) : fallback; }

static const char *tr(const char *key, const char *fallback)
{ return ppa_playback_ui_text(key, fallback); }

static void outlined(Image *image, int y, int height, const char *text,
                     FtTextAlign align, int size)
{
    FtFont *font = g_ui->getFont();
    font->drawStringInRect(image, 8, y, 464, height, 0xffffffffU,
                            text, align, size, 14);
}

/* Build a black one-pixel outline after shaping each line just once. Black
 * pixels never seed the outline, so this in-place pass cannot grow a halo. */
static void add_outline(Image *image)
{
    for (int y = 1; y < image->imageHeight - 1; ++y) {
        for (int x = 1; x < image->imageWidth - 1; ++x) {
            Color *pixel = image->data + y * image->textureWidth + x;
            if (*pixel != 0U) continue;
            bool ink = false;
            for (int dy = -1; dy <= 1 && !ink; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (pixel[dy * image->textureWidth + dx] & 0x00ffffffU) {
                        ink = true; break;
                    }
            if (ink) *pixel = 0xff000000U;
        }
    }
}

/* The shipped translations use only %d/%u (and optional literal %%). Reject
 * an unexpected format rather than interpreting a translated printf string
 * in the real-time path. Each number retains a fixed, independently LTR box
 * when the surrounding translated runs are arranged right-to-left. */
static int health_runs(const char *format, HealthRun *runs, int first, int count)
{
    int run = 0, used = 0, field = 0;
    memset(runs, 0, sizeof(HealthRun) * 7);
    runs[0].field = -1;
    for (const char *p = format; p && *p; ++p) {
        if (*p == '%' && p[1] != '%') {
            if ((p[1] != 'd' && p[1] != 'u') || field >= count || run + 2 >= 7)
                return 0;
            ++run;
            runs[run].field = first + field++;
            ++run;
            runs[run].field = -1;
            used = 0;
            ++p;
        } else {
            if (*p == '%') ++p;
            if (used + 1 >= (int)sizeof(runs[run].text)) return 0;
            runs[run].text[used++] = *p;
        }
    }
    return field == count ? run + 1 : 0;
}

static int health_line_width(FtFont *font, HealthRun *runs, int count, int cell)
{
    static const int cells[5] = {3, 5, 5, 6, 6};
    int width = 0;
    for (int i = 0; i < count; ++i) {
        runs[i].width = runs[i].field < 0 ? font->measureShapedString(runs[i].text) :
                                         cells[runs[i].field] * cell;
        width += runs[i].width;
    }
    return width;
}

static void bake_health_line(FtFont *font, HealthRun *runs, int count, int row, int size)
{
    static const int cells[5] = {3, 5, 5, 6, 6};
    int x = g_ui->isRtl() ? 472 : 8;
    for (int i = 0; i < count; ++i) {
        if (g_ui->isRtl()) x -= runs[i].width;
        if (runs[i].field >= 0) {
            HealthField *f = &g_fields[runs[i].field];
            f->x = x; f->y = row; f->cells = cells[runs[i].field];
        } else if (runs[i].width > 0) {
            font->drawStringInRect(g_health_assets, x, row + 1,
                runs[i].width, HEALTH_ROW - 2, 0xffffffffU, runs[i].text,
                g_ui->isRtl() ? FT_TEXT_ALIGN_RIGHT : FT_TEXT_ALIGN_LEFT, size, size);
        }
        if (!g_ui->isRtl()) x += runs[i].width;
    }
}

static uint16_t health_pixel_4444(Color c)
{
    /* PSP ABGR8888 -> ABGR4444. Only diagnostic glyph edge coverage is
     * quantized; decoded video/framebuffer format remains unchanged. */
    return (uint16_t)(((c >> 16) & 0xf000U) | ((c >> 12) & 0x0f00U) |
                      ((c >> 8) & 0x00f0U) | ((c >> 4) & 0x000fU));
}

static int prepare_health_assets(void)
{
    HealthRun metrics[7], sync[7];
    FtFont *font = g_ui->getFont();
    int old_size = font->getPixelSize();
    int nm = health_runs(tr("health.metrics", "CPU %d MHz | Last decode %u/%u ms"), metrics, 0, 3);
    int ns = health_runs(tr("health.sync", "A/V %d ms | Skipped %u"), sync, 3, 2);
    if (!nm || !ns) return 0;
    if (ppa_session_audio_only()) g_health = createImage(480, HEALTH_HEIGHT);
    else {
        g_health_texture = static_cast<uint16_t *>(malloc_64(HEALTH_PITCH * HEALTH_HEIGHT * sizeof(uint16_t)));
        if (g_health_texture) memset(g_health_texture, 0, HEALTH_PITCH * HEALTH_HEIGHT * sizeof(uint16_t));
    }
    g_health_assets = createImage(480, 256);
    if ((!g_health && !g_health_texture) || !g_health_assets) return 0;
    int size;
    for (size = 17; size >= 9; --size) {
        font->setPixelSize(size);
        g_digit_width = font->measureShapedString("8") + 2;
        if (g_digit_width < 6) g_digit_width = 6;
        if (g_digit_width > 20) continue;
        int wm = health_line_width(font, metrics, nm, g_digit_width);
        int ws = health_line_width(font, sync, ns, g_digit_width);
        if (wm <= 464 && ws <= 464) break;
    }
    if (size < 9) { font->setPixelSize(old_size); return 0; }
    bake_health_line(font, metrics, nm, 22, size);
    bake_health_line(font, sync, ns, 44, size);
    FtTextAlign align = g_ui->isRtl() ? FT_TEXT_ALIGN_RIGHT : FT_TEXT_ALIGN_LEFT;
    font->drawStringInRect(g_health_assets, 8, 1, 464, 20, 0xffffffffU,
        tr("health.title", "Playback health"), align, 18, 12);
    for (int i = 0; i < HEALTH_REASON_COUNT; ++i)
        font->drawStringInRect(g_health_assets, 8, 67 + i * HEALTH_ROW, 464, 20,
            0xffffffffU, tr(g_reason_keys[i], g_reason_fallbacks[i]), align, 16, 10);
    static const char digits[] = "0123456789-+";
    for (int i = 0; digits[i]; ++i) {
        char glyph[2] = {digits[i], 0};
        font->drawStringInRect(g_health_assets, i * g_digit_width, HEALTH_DIGITS_Y + 1,
            g_digit_width, 20, 0xffffffffU, glyph, FT_TEXT_ALIGN_CENTER, size, size);
    }
    add_outline(g_health_assets);
    font->setPixelSize(old_size);
    if (g_health_texture) {
        /* Convert immutable assets once before workers start. Runtime panel
         * assembly remains row memcpy, with no recurring pixel conversion. */
        g_health_assets_texture = static_cast<uint16_t *>(malloc_64(HEALTH_PITCH * 256 * sizeof(uint16_t)));
        if (!g_health_assets_texture) return 0;
        for (int i = 0; i < HEALTH_PITCH * 256; ++i)
            g_health_assets_texture[i] = health_pixel_4444(g_health_assets->data[i]);
        freeImage(g_health_assets);
        g_health_assets = 0;
    }
    return 1;
}

static void health_copy_span(int x, int y, int src_x, int src_y, int width)
{
    if (g_health)
        memcpy(g_health->data + y * HEALTH_PITCH + x,
            g_health_assets->data + src_y * HEALTH_PITCH + src_x, width * sizeof(Color));
    else
        memcpy(g_health_texture + y * HEALTH_PITCH + x,
            g_health_assets_texture + src_y * HEALTH_PITCH + src_x, width * sizeof(uint16_t));
}

static void copy_health_rows(int dst_y, int src_y, int height)
{
    for (int row = 0; row < height; ++row)
        health_copy_span(0, dst_y + row, 0, src_y + row, 480);
}

static void health_number(int index, const char *number)
{
    const HealthField *f = &g_fields[index];
    int n = (int)strlen(number);
    if (n > f->cells) return;
    int x = f->x + (f->cells - n) * g_digit_width;
    for (int i = 0; i < n; ++i) {
        int digit = number[i] == '-' ? 10 : number[i] == '+' ? 11 : number[i] - '0';
        if (digit < 0 || digit > 11) return;
        for (int row = 0; row < HEALTH_ROW; ++row)
            health_copy_span(x, f->y + 2 + row, digit * g_digit_width,
                HEALTH_DIGITS_Y + row, g_digit_width);
        x += g_digit_width;
    }
}

void ppa_playback_ui_release(void)
{
    if (g_health) freeImage(g_health);
    if (g_health_assets) freeImage(g_health_assets);
    if (g_message) freeImage(g_message);
    if (g_health_texture) free_64(g_health_texture);
    if (g_health_assets_texture) free_64(g_health_assets_texture);
    g_health_texture = g_health_assets_texture = 0;
    g_health_rect_count = 0;
    g_health = g_health_assets = g_message = 0;
    g_ui = 0;
    g_next_health_us = 0;
    g_message_drawn = 0;
}

int ppa_playback_ui_prepare(UiI18n *ui)
{
    ppa_playback_ui_release();
    if (!ppa_session_audio_only() && !ppa_session_health_enabled()) return 1;
    if (!ui || !ui->getFont()) return 0;
    g_ui = ui; /* borrowed: browser releases it only after playback joins */
    if (ppa_session_health_enabled() && !prepare_health_assets()) {
        freeImage(g_health); freeImage(g_health_assets);
        g_health = g_health_assets = 0;
        if (g_health_texture) free_64(g_health_texture);
        if (g_health_assets_texture) free_64(g_health_assets_texture);
        g_health_texture = g_health_assets_texture = 0;
    }
    if (ppa_session_audio_only()) {
        g_message = createImage(480, 64);
        if (!g_message) { ppa_playback_ui_release(); return 0; }
        clearImage(g_message, 0);
        outlined(g_message, 2, 60,
            tr("playback.audio_only", "Audio Only Mode is Enabled\nin Config Menu"),
            FT_TEXT_ALIGN_CENTER, 19);
        add_outline(g_message);
    }
    /* Health is optional under memory pressure; the required audio-only
     * notice fails cleanly if its small texture cannot be created. */
    return 1;
}

static void health_build_rects(void)
{
    g_health_rect_count = 0;
    /* Once per snapshot, bound ink inside disjoint 16x24 cells. Empty cells
     * are absent from the GE batch; the rest exclude transparent margins.
     * This bounds list size at 120 sprites without per-frame pixel scans. */
    for (int by = 0; by < HEALTH_HEIGHT; by += 24) {
        for (int bx = 0; bx < 480; bx += 16) {
            int x0 = bx + 16, x1 = bx, y0 = by + 24, y1 = by;
            for (int y = by; y < by + 24; ++y)
                for (int x = bx; x < bx + 16; ++x)
                    if (g_health_texture[y * HEALTH_PITCH + x] & 0xf000U) {
                        if (x < x0) x0 = x;
                        if (x + 1 > x1) x1 = x + 1;
                        if (y < y0) y0 = y;
                        if (y + 1 > y1) y1 = y + 1;
                    }
            if (x0 < x1 && y0 < y1) {
                HealthRect *r = &g_health_rects[g_health_rect_count++];
                r->x0 = x0; r->x1 = x1; r->y0 = y0; r->y1 = y1;
            }
        }
    }
}

static int update_health(void)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    struct ppa_session_snapshot s;
    char number[16];
    int reason;
    if ((!g_health && !g_health_texture) || now < g_next_health_us) return 0;
    if (!ppa_session_snapshot(&s)) return 0;
    g_next_health_us = now + 1000000ULL;
    copy_health_rows(2, 0, 66);
    /* Query once per snapshot, not per frame. CFW can quantize a request;
     * fall back to the governor request only if firmware gives no valid CPU. */
    int cpu = scePowerGetCpuClockFrequencyInt();
    if (cpu <= 0 || cpu > 333) cpu = cpu_clock_get_current_cpu();
    snprintf(number, sizeof(number), "%d", cpu);
    health_number(0, number);
    unsigned int ms = s.decode_us / 1000U;
    snprintf(number, sizeof(number), "%u", ms > 99999U ? 99999U : ms);
    health_number(1, number);
    ms = s.budget_us / 1000U;
    snprintf(number, sizeof(number), "%u", ms > 99999U ? 99999U : ms);
    health_number(2, number);
    int64_t delta = s.audio_valid && s.video_ms >= 0 ? s.audio_ms - s.video_ms : 0;
    if (delta > 99999) delta = 99999;
    if (delta < -99999) delta = -99999;
    snprintf(number, sizeof(number), "%d", (int)delta);
    health_number(3, number);
    if (s.skipped > 99999U) strcpy(number, "99999+");
    else snprintf(number, sizeof(number), "%u", s.skipped);
    health_number(4, number);
    if (s.paused) reason = 0;
    else if (s.audio_only) reason = 1;
    else if (!s.audio_valid) reason = 2;
    else if (s.budget_us && s.decode_us >= s.budget_us)
        reason = 3;
    else if (delta > 100 || delta < -100)
        reason = 4;
    else if (s.compatibility || s.weights_disabled)
        reason = 5;
    else reason = 6;
    copy_health_rows(68, 66 + reason * HEALTH_ROW, HEALTH_ROW);
    /* Publish only the 96 rows owned by the output texture. The nominal GE
     * height is 128; nearest-filtered rectangles never sample its extra rows. */
    if (g_health_texture) {
        health_build_rects();
        ppa_cache_cpu_wrote_ge_will_read(g_health_texture,
            HEALTH_PITCH * HEALTH_HEIGHT * sizeof(uint16_t));
    }
    return 1;
}

int ppa_playback_ui_has_overlay(void)
{ return g_health_texture != 0; }

void ppa_playback_ui_draw(void)
{
    struct Vertex { float u, v; short x, y, z; };
    if (!ppa_playback_ui_has_overlay()) return;
    update_health();
    if (!g_health_rect_count) return;
    unsigned int left, top, width, height;
    ppa_gu_output_rect(&left, &top, &width, &height);
    Vertex *v = static_cast<Vertex *>(sceGuGetMemory(2 * g_health_rect_count * sizeof(Vertex)));
    if (!v) return;
    for (int i = 0; i < g_health_rect_count; ++i) {
        const HealthRect *r = &g_health_rects[i];
        Vertex *pair = v + 2 * i;
        pair[0].u = (float)r->x0; pair[0].v = (float)r->y0;
        pair[0].x = (short)(left + r->x0 * width / 480U);
        pair[0].y = (short)(top + r->y0 * height / 272U);
        pair[0].z = 0;
        pair[1].u = (float)r->x1; pair[1].v = (float)r->y1;
        pair[1].x = (short)(left + r->x1 * width / 480U);
        pair[1].y = (short)(top + r->y1 * height / 272U);
        pair[1].z = 0;
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_FALSE);
    sceGuTexImage(0, HEALTH_PITCH, 128, HEALTH_PITCH, g_health_texture);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_16BIT |
                     GU_TRANSFORM_2D, 2 * g_health_rect_count, 0, v);
}

static void static_blit(Image *image, Color *frame, int pitch, int y)
{
    if (!image) return;
    unsigned int left, top, width, height;
    ppa_gu_output_rect(&left, &top, &width, &height);
    if (!width || !height || left + width > (unsigned int)pitch) return;
    if (ppa_gu_scanout_width() == 720U) {
        /* Audio-only has no GE/video writer. Scale only these small cached
         * panels at the existing 1 Hz cadence, into the native field layout. */
        unsigned int first = top + (unsigned int)y * height / 272U;
        unsigned int last = top + (unsigned int)(y + image->imageHeight) * height / 272U;
        if (last > 480U) last = 480U;
        for (unsigned int row = first; row < last; ++row) {
            unsigned int source_row = (row - top) * 272U / height;
            source_row = source_row > (unsigned int)y ? source_row - y : 0U;
            if (source_row >= (unsigned int)image->imageHeight)
                source_row = image->imageHeight - 1;
            unsigned int scan_row = ppa_gu_scanout_interlaced() ?
                (row / 2U + ((row & 1U) ? 0U : 262U)) : row;
            const Color *source = image->data + source_row * image->textureWidth;
            Color *destination = frame + scan_row * pitch + left;
            for (unsigned int x = 0; x < width; ++x)
                destination[x] = source[x * 480U / width];
        }
        return;
    }
    /* The background is known black and the framebuffer has no device writer.
     * Uncached row writes avoid a full-frame cache sweep or a second GE list. */
    for (int row = 0; row < image->imageHeight; ++row)
        memcpy(frame + (y + row) * pitch,
               image->data + row * image->textureWidth, 480 * sizeof(Color));
}

void ppa_playback_ui_tick_audio_only(void)
{
    void *frame = 0;
    int pitch, format;
    if (!ppa_session_audio_only() || !g_message) return;
    int changed = update_health();
    if (g_message_drawn && !changed) return;
    if (sceDisplayGetFrameBuf(&frame, &pitch, &format,
            PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !frame || pitch < 480 ||
            format != PSP_DISPLAY_PIXEL_FORMAT_8888) return;
    Color *pixels = static_cast<Color *>(ppa_uncached_ptr(frame));
    if (!g_message_drawn) static_blit(g_message, pixels, pitch, 104);
    if (changed) static_blit(g_health, pixels, pitch, 0);
    g_message_drawn = 1;
}
