/* Placeholders for the compile-time gated variant features, see ui_features.h. Firmware only. */
#include "epager/ui_features.h"

#if CONFIG_EPAGER_FEATURE_ANIMATION
void ui_anim_init(void) {}
#endif
#if CONFIG_EPAGER_FEATURE_TOUCH
void ui_touch_init(void) {}
#endif
