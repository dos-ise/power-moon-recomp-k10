// Minimal runtime for the on-device pipeline proof (Phase 3). The generated game code calls
// these functions; the full runtime (SDL/Vulkan/input/...) is a later phase. Compiled together
// with the generated code so the game .so is self-contained and dlopen works. The bodies are
// placeholders - the pipeline proof only needs the symbols to exist.
#include "cpu.h"

void rt_dispatch(Cpu *c) { (void)c; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; }
uint64_t rt_ticks(void) { return 0; }

float odyssey_wide(void) { return 1.0f; }
float odyssey_wide_camera(void) { return 1.0f; }
float odyssey_wide_drawinfo(uint64_t di) { (void)di; return 1.0f; }
void odyssey_debug_hook(Cpu *c, uint64_t pc) { (void)c; (void)pc; }
_Bool odyssey_skip_extra_particle_update(void) { return 0; }
void odyssey_coop_near_zero(Cpu *c) { (void)c; }
void odyssey_coop_find_glyph(Cpu *c, GuestFn original) { (void)c; (void)original; }
void odyssey_coop_set_tutorial(Cpu *c, GuestFn original) { (void)c; (void)original; }
void odyssey_coop_set_pane(Cpu *c, GuestFn original) { (void)c; (void)original; }
