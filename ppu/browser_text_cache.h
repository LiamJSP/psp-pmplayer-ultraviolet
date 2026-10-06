#ifndef PPU_BROWSER_TEXT_CACHE_H
#define PPU_BROWSER_TEXT_CACHE_H

#include "ftfont.h"

/* UI-thread-only raster cache. No font/GE ownership crosses a thread. Font
 * users must clear it before changing/replacing faces or their raster style.
 * The retained pixels are CPU snippets, never GE textures. */
class BrowserTextCache : private boost::noncopyable {
public:
    BrowserTextCache();
    ~BrowserTextCache();
    void clear();
    void draw(FtFont* font, Image* destination, int x, int y,
              int width, int height, Color color, const char* text,
              bool paint = true);

private:
    enum { SLOT_COUNT = 64, MAX_BYTES = 512 * 1024,
           MAX_ENTRY_BYTES = 128 * 1024 };
    struct Entry {
        FtFont* font;
        Color* pixels;
        Color color;
        int width, height, pixelSize;
        unsigned int bytes, used;
        char text[512];
    } entries[SLOT_COUNT];
    unsigned int bytes, clock;
    int oldest() const;
    void evict(int slot);
};

#endif
