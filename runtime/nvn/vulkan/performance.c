// The performance overlay is not ported yet: no stats.
#include "../../ui/performance.h"

VulkanOverlayStats vk_overlay_stats(void) {
    VulkanOverlayStats s = {0};
    s.cpu_usage = s.gpu_usage = -1;
    return s;
}
void vk_overlay_memory(uint64_t used, uint64_t budget) { (void)used; (void)budget; }
