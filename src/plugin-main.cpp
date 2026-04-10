#include <obs-module.h>

#include "dfn3_filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-dfn3-noise-suppress", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
    return "DFN3 Noise Suppress";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "DeepFilterNet3-based native C++ noise suppression filter for OBS.";
}

bool obs_module_load(void)
{
    obs_register_source(&dfn3_noise_suppress_filter_info);
    blog(LOG_INFO, "[DFN3 Noise Suppress] Module loaded.");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[DFN3 Noise Suppress] Module unloaded.");
}
