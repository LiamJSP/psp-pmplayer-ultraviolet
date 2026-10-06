#include "browser_text_cache.h"

#include <stdlib.h>
#include <string.h>

BrowserTextCache::BrowserTextCache() : bytes(0), clock(0)
{
    memset(entries, 0, sizeof(entries));
}

BrowserTextCache::~BrowserTextCache()
{
    clear();
}

void BrowserTextCache::evict(int slot)
{
    bytes -= entries[slot].bytes;
    free(entries[slot].pixels);
    memset(&entries[slot], 0, sizeof(entries[slot]));
}

void BrowserTextCache::clear()
{
    for (int i = 0; i < SLOT_COUNT; ++i)
        evict(i);
    clock = 0;
}

int BrowserTextCache::oldest() const
{
    int slot = -1;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (entries[i].pixels != NULL &&
            (slot < 0 || (unsigned int)(clock - entries[i].used) >
                         (unsigned int)(clock - entries[slot].used)))
            slot = i;
    }
    return slot;
}

void BrowserTextCache::draw(FtFont* font, Image* destination, int x, int y,
                            int width, int height, Color color,
                            const char* text, bool paint)
{
    if (font == NULL || destination == NULL || text == NULL || text[0] == 0)
        return;
    const int pixelSize = font->getPixelSize();
    /* Match the existing renderer's clipping. Unusual/oversized skin
     * rectangles retain the direct renderer and cannot grow this cache. */
    if (x < 0 || x >= destination->imageWidth ||
        y < -destination->imageHeight ||
        y > destination->imageHeight + pixelSize ||
        width <= 0 || width > PSP_SCREEN_WIDTH ||
        height <= 0 || height > PSP_SCREEN_HEIGHT ||
        pixelSize <= 0 || pixelSize > PSP_SCREEN_HEIGHT ||
        strlen(text) >= sizeof(entries[0].text)) {
        if (paint)
            font->printStringToImage(destination, x, y, width, height, color, text);
        return;
    }
    if (width > destination->imageWidth - x)
        width = destination->imageWidth - x;
    if (height > destination->imageHeight)
        height = destination->imageHeight;
    const unsigned int needed = (unsigned int)width * (unsigned int)height * sizeof(Color);
    if (needed > MAX_ENTRY_BYTES) {
        if (paint)
            font->printStringToImage(destination, x, y, width, height, color, text);
        return;
    }

    Entry* entry = NULL;
    int empty = -1;
    ++clock;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        Entry* candidate = &entries[i];
        if (candidate->pixels == NULL) {
            if (empty < 0) empty = i;
        } else if (candidate->font == font && candidate->width >= width &&
                   candidate->height == height && candidate->pixelSize == pixelSize &&
                   candidate->color == color && strcmp(candidate->text, text) == 0) {
            /* printStringToImage lays out from the left baseline; width
             * only clips pixels. A narrower cursor row can therefore reuse
             * the normal raster when both styles use the same color. */
            entry = candidate;
            break;
        }
    }
    if (entry == NULL) {
        while (empty < 0 || bytes + needed > MAX_BYTES) {
            const int slot = oldest();
            if (slot < 0) break;
            evict(slot);
            empty = slot;
        }
        if (empty >= 0 && bytes + needed <= MAX_BYTES) {
            entry = &entries[empty];
            entry->pixels = (Color*)calloc(1, needed);
            if (entry->pixels == NULL)
                entry = NULL;
        }
        if (entry == NULL) {
            if (paint)
                font->printStringToImage(destination, x, y, width, height, color, text);
            return;
        }
        entry->font = font;
        entry->width = width;
        entry->height = height;
        entry->pixelSize = pixelSize;
        entry->color = color;
        entry->bytes = needed;
        strcpy(entry->text, text);
        bytes += needed;

        /* Tight CPU pitch: no power-of-two texture padding and no second
         * raster allocation. FreeType applies the same shaping, fallback,
         * coverage and PSP model color profile as the direct path. */
        Image raster;
        memset(&raster, 0, sizeof(raster));
        raster.imageWidth = raster.textureWidth = width;
        raster.imageHeight = raster.textureHeight = height;
        raster.data = entry->pixels;
        font->printStringToImage(&raster, 0, pixelSize, width, height, color, text);
    }
    entry->used = clock;
    if (!paint) return;

    const int top = y - pixelSize;
    int firstRow = top < 0 ? -top : 0;
    int lastRow = height;
    if (lastRow > destination->imageHeight - top)
        lastRow = destination->imageHeight - top;
    for (int row = firstRow; row < lastRow; ++row) {
        const Color* source = entry->pixels + row * entry->width;
        Color* target = destination->data + (top + row) * destination->textureWidth + x;
        for (int column = 0; column < width; ++column) {
            /* FreeType overwrites only covered pixels. Preserve the cursor
             * fill and other layers under transparent portions of the line. */
            if (A(source[column]) != 0)
                target[column] = source[column];
        }
    }
    destination->dirty = 1;
}
