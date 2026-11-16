#pragma once
#include "vk.h"

typedef struct {
    VkImage color, depth, motion, output;
    VkFormat color_format;
    uint32_t w, h, ow, oh;
    float jitter[2], dt_ms, near_z, far_z, fov_y, sharpness;
    bool reset, ml;
} Fsr3Frame;
#ifdef _WIN32
void vk_fsr3_init(void);
bool vk_fsr3_available(void);
bool vk_fsr4_available(void);
void vk_fsr3_release(void); // caller must wait for the last dispatch's submission
bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *frame);
#else  // (AMD's runtimes are Windows DLLs: elsewhere FSR 3.1 / 4 are never available)
static inline bool vk_fsr3_available(void) { return false; }
static inline bool vk_fsr4_available(void) { return false; }
static inline void vk_fsr3_release(void) {}
static inline bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f) { (void)cb, (void)f; return false; }
#endif

