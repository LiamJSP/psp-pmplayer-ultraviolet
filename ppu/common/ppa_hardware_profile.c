#include "ppa_hardware_profile.h"
#include "m33sdk.h"

#include <pspge.h>
#include <pspsysmem.h>
#include <string.h>

static struct ppa_hardware_profile g_profile;
static int g_initialized;

static enum ppa_psp_family family_from_generation(int model)
{
    switch (model) {
    case 0: return PPA_PSP_FAMILY_1000;
    case 1: return PPA_PSP_FAMILY_2000;
    case 2:
    case 3:
    case 6:
    case 7:
    case 8: return PPA_PSP_FAMILY_3000;
    case 4: return PPA_PSP_FAMILY_GO;
    case 10: return PPA_PSP_FAMILY_STREET;
    default: return PPA_PSP_FAMILY_UNKNOWN;
    }
}

static unsigned int expected_ram_mb(enum ppa_psp_family family)
{
    return family == PPA_PSP_FAMILY_1000 ? 32U :
           family == PPA_PSP_FAMILY_UNKNOWN ? 0U : 64U;
}

void ppa_hardware_profile_refresh_memory(void)
{
    unsigned int total_free;
    unsigned int max_free;

    if (!g_initialized) {
        ppa_hardware_profile_init();
        return;
    }

    total_free = (unsigned int)sceKernelTotalFreeMemSize();
    max_free = (unsigned int)sceKernelMaxFreeMemSize();
    g_profile.current_total_free_bytes = total_free;
    g_profile.current_max_free_bytes = max_free;

    /*
     * With the fixed application heap already reserved, a largest remaining
     * user-partition block of at least 8 MiB is a practical signal that the
     * Slim/Brite extended partition is actually mapped for this process.
     * Model identity alone is deliberately not treated as proof: CFW policy
     * can expose or withhold the second 32 MiB bank.
     */
    if (!g_profile.extended_user_memory_visible &&
        g_profile.expected_physical_ram_mb >= 64U &&
        max_free >= (8U * 1024U * 1024U))
        g_profile.extended_user_memory_visible = 1U;
}

void ppa_hardware_profile_init(void)
{
    int model;
    unsigned int edram;

    if (g_initialized)
        return;

    memset(&g_profile, 0, sizeof(g_profile));

    model = m33KernelGetModel();
    edram = sceGeEdramGetSize();

    g_profile.model_generation = model;
    g_profile.family = family_from_generation(model);
    g_profile.expected_physical_ram_mb = expected_ram_mb(g_profile.family);
    g_profile.ge_edram_bytes = edram;
    g_profile.cache_line_bytes = 64U;
    g_profile.startup_total_free_bytes =
        (unsigned int)sceKernelTotalFreeMemSize();
    g_profile.startup_max_free_bytes =
        (unsigned int)sceKernelMaxFreeMemSize();
    g_profile.current_total_free_bytes = g_profile.startup_total_free_bytes;
    g_profile.current_max_free_bytes = g_profile.startup_max_free_bytes;
    g_initialized = 1;

    ppa_hardware_profile_refresh_memory();
}

const struct ppa_hardware_profile *ppa_hardware_profile_get(void)
{
    if (!g_initialized)
        ppa_hardware_profile_init();
    return &g_profile;
}

int ppa_hardware_model(void)
{
    return ppa_hardware_profile_get()->model_generation;
}

enum ppa_psp_family ppa_hardware_family(void)
{
    return ppa_hardware_profile_get()->family;
}

int ppa_hardware_has_64mb_ram(void)
{
    return ppa_hardware_profile_get()->extended_user_memory_visible != 0U;
}

const char *ppa_hardware_model_name(void)
{
    switch (ppa_hardware_family()) {
    case PPA_PSP_FAMILY_1000: return "PSP-1000";
    case PPA_PSP_FAMILY_2000: return "PSP-2000";
    case PPA_PSP_FAMILY_3000: return "PSP-3000";
    case PPA_PSP_FAMILY_GO: return "PSP Go";
    case PPA_PSP_FAMILY_STREET: return "PSP Street";
    default: return "Unknown PSP";
    }
}
