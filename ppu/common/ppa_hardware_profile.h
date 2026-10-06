#ifndef PPA_HARDWARE_PROFILE_H
#define PPA_HARDWARE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

enum ppa_psp_family {
    PPA_PSP_FAMILY_UNKNOWN = 0,
    PPA_PSP_FAMILY_1000,
    PPA_PSP_FAMILY_2000,
    PPA_PSP_FAMILY_3000,
    PPA_PSP_FAMILY_GO,
    PPA_PSP_FAMILY_STREET
};

struct ppa_hardware_profile {
    int model_generation;
    enum ppa_psp_family family;
    unsigned int expected_physical_ram_mb;
    unsigned int ge_edram_bytes;
    unsigned int startup_total_free_bytes;
    unsigned int startup_max_free_bytes;
    unsigned int current_total_free_bytes;
    unsigned int current_max_free_bytes;
    unsigned int extended_user_memory_visible;
    unsigned int cache_line_bytes;
};

void ppa_hardware_profile_init(void);
void ppa_hardware_profile_refresh_memory(void);

const struct ppa_hardware_profile *ppa_hardware_profile_get(void);
int ppa_hardware_model(void);
enum ppa_psp_family ppa_hardware_family(void);
int ppa_hardware_has_64mb_ram(void);
const char *ppa_hardware_model_name(void);

#ifdef __cplusplus
}
#endif

#endif
