/*
 * Entry points of the variant features that are compiled in only when their CONFIG_EPAGER_FEATURE_*
 * option is on (components/core/Kconfig). Placeholders until the LCD variant (P1) exists: they
 * prove that a disabled feature leaves no symbol in the image (tools/feature_check.sh).
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_EPAGER_FEATURE_ANIMATION
void ui_anim_init(void);
#endif
#if CONFIG_EPAGER_FEATURE_TOUCH
void ui_touch_init(void);
#endif

#ifdef __cplusplus
}
#endif
