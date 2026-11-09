// The recompiled game code: a library built from the player's own game files (`make gen` + `make`) and loaded at
// startup (runtime/gamelib.c). Nothing from the game is part of the app or this repository.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "odyssey_game.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "odyssey_game.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "odyssey_game.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library built with another one is not loaded (`make gen`, `make`).
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 2
#define GAMELIB_MAX_MODULES 4  // main, subsdk0 (Odyssey: multimedia), sdk

typedef struct {
    const char *name;       // the exefs file it was made from ("main", "subsdk0", "sdk")
    const uint64_t *addrs;  // sorted guest addresses
    const GuestFn *fns;     // the functions at them
    const uint64_t *count;
} GameLibModule;

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    uint32_t nmodules;
    GameLibModule modules[GAMELIB_MAX_MODULES];
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "odyssey_game_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// ODYSSEY_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);

// The game code's target on this CPU. x86-64 builds the game code for x86-64-v3 (AVX2, FMA, BMI2: the Makefile's
// GENARCH) where the CPU has those, else for the compatible target (GENARCH_COMPAT: in this K10 fork AMD family 10h,
// Phenom II / Athlon II, without FMA instructions - runtime/simd_softfma.h instead, slower). Each has its own id, so a
// library is never loaded on a CPU that can't run it, and a game folder moved to a PC of the other kind gets its game
// code built again. ODYSSEY_COMPAT_CPU=1 picks the compatible target on any CPU that runs it (to compare the two).
// Other hosts have one target. (The scheme comes from Savage Wind Recomp: MIT, Revivedx and contributors.)
bool gamelib_compat(void);
const char *gamelib_id(void);  // the id a library for this CPU has (GAMELIB_ID or GAMELIB_ID_COMPAT)
// NULL when the CPU runs one of the game code's targets, else what it lacks ("a CPU with ...") - checked at startup
const char *gamelib_cpu_problem(void);

#ifdef __cplusplus
}
#endif
