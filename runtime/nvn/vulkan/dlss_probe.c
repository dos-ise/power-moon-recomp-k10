// DLSS is not ported yet: the probe reports nothing, so the renderer's DLSS paths stay off.
#include "dlss_probe.h"

#ifdef _WIN32
void vk_dlss_probe_instance(const char **ext, uint32_t *count, uint32_t capacity) {
    (void)ext; (void)count; (void)capacity;
}
void vk_dlss_probe_device(const VkExtensionProperties *have, uint32_t available,
                          const char **ext, uint32_t *count, uint32_t capacity) {
    (void)have; (void)available; (void)ext; (void)count; (void)capacity;
}
void vk_dlss_probe_run(void) {}
bool vk_dlss_enabled(void) { return false; }
double vk_dlss_scale(void) { return 1.0; }
void vk_dlss_queue_camera(const DlssCamera *camera) { (void)camera; }
void vk_dlss_set_camera(const DlssCamera *camera) { (void)camera; }
void vk_dlss_capture_depth(const TexBackend *depth, uint64_t frame) { (void)depth; (void)frame; }
void vk_dlss_viewport_jitter(const TexBackend *depth, uint64_t frame, float offset[2]) {
    (void)depth; (void)frame; offset[0] = offset[1] = 0;
}
float vk_dlss_surface_lod_bias(const TexBackend *depth) { (void)depth; return 0; }
VkImageView vk_dlss_upscale(VkCommandBuffer cb, const TexBackend *color, VkImageView view,
                            uint32_t ow, uint32_t oh, uint64_t frame) {
    (void)cb; (void)color; (void)ow; (void)oh; (void)frame; return view;
}
#endif
