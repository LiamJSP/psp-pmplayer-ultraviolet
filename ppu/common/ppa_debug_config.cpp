#include "ppa_debug.h"
#include "../config.h"
#include <stdio.h>
static char config_path[1024];
void ppa_debug_config_sync(void)
{
    Config *c = Config::getInstance();
    if (!c) return;
    bool enabled = ppa_debug_enabled() != 0;
    if (c->getBooleanValue("config/player/debug_mode", false) != enabled) {
        c->setBooleanValue("config/player/debug_mode", enabled);
        if (config_path[0]) c->save(config_path);
    }
}
void ppa_debug_config_init(const char *path)
{
    int n = snprintf(config_path, sizeof(config_path), "%s", path ? path : "");
    if (n < 0 || n >= (int)sizeof(config_path)) config_path[0] = 0;
    Config *c = Config::getInstance();
    ppa_debug_init(c && c->getBooleanValue("config/player/debug_mode", false));
    ppa_debug_config_sync();
}
int ppa_debug_config_set(int enabled)
{
    int ok = ppa_debug_set_enabled(enabled);
    ppa_debug_config_sync();
    return ok;
}
