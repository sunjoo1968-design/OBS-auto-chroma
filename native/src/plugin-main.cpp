#include "ac-types.hpp"
#include "dock.hpp"
#include "filter.hpp"
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sunjoo-auto-chroma", "ko-KR")

MODULE_EXPORT const char *obs_module_name(void) { return "자동 크로마키"; }
MODULE_EXPORT const char *obs_module_author(void) { return ac::Author; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "GPU chroma key, stroke precision keyer and clean plate filter with an OBS dock. SunjooAn 1.4";
}
bool obs_module_load(void)
{
    ac::registerFilter();
    blog(LOG_INFO, "[sunjoo-auto-chroma] %s %s loaded for OBS %s", ac::Author, ac::Version,
         obs_get_version_string());
    return true;
}
void obs_module_post_load(void) { ac::createDock(); }
void obs_module_unload(void) { ac::destroyDock(); }
