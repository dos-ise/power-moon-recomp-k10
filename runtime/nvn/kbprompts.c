// Keyboard prompts are not ported yet: nothing is ever active.
#include "kbprompts.h"

bool kbprompts_active(void) { return false; }
bool kbprompts_candidate(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t bpb) {
    (void)w; (void)h; (void)bw; (void)bh; (void)bpb; return false;
}
bool kbprompts_identify(const uint8_t *blocks, uint32_t wb, uint32_t hb) {
    (void)blocks; (void)wb; (void)hb; return false;
}
uint32_t kbprompts_generation(void) { return 0; }
void kbprompts_apply(uint32_t lv, uint8_t *blocks, uint32_t wb, uint32_t hb) {
    (void)lv; (void)blocks; (void)wb; (void)hb;
}
void kbprompts_note_device(bool keyboard) { (void)keyboard; }

// Keyboard prompts are not ported yet: the Cappy glyph helpers pass through.
uint16_t kbprompts_cappy_code(uint16_t code) { return code; }
bool kbprompts_cappy_glyph(uint32_t code, uint32_t *original, uint16_t *source, uint16_t *index) {
    (void)code; (void)original; (void)source; (void)index; return false;
}
