// nn::pl shared system fonts. The Switch's system fonts are not part of the game; in order of preference:
//  1. ODYSSEY_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf)
//  2. a dump from the user's own console in fonts/: nintendo_udsg-r_std_003.bfttf (Standard, Morisawa UD Shin Go)
//     and nintendo_ext_003.bfttf (NintendoExt button glyphs); plain .ttf versions of those names also work
//  3. a Japanese gothic like UD Shin Go (much closer to the game's look than Arial): Hiragino Sans W4 on macOS,
//     Yu Gothic Regular / Medium on Windows
//  4. Arial
//  5. Linux: DejaVu / Noto / Liberation / Droid Sans from the usual paths, or any TrueType sans-serif found by
//     walking the system font directories (Fedora / Bazzite don't ship DejaVu)
//  6. Roboto Medium, shipped next to the app for the menu (Apache 2.0): so a system with none of those fonts still
//     starts (Manjaro, Bazzite: #16)
// .bfttf is the console's obfuscated TTF: 8-byte header (magic, size) then every u32 XORed with a fixed key.
// .ttc collections get face 0 copied out as a standalone font (the game's font engine wants one sfnt).
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"
#include "../paths.h"

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "../hle.h"

typedef struct { uint64_t addr, size; } Font;
static Font fonts[2];  // [0] Standard (every type), [1] NintendoExt when a dump provides it

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len + 4);
    if (fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

// .bfttf -> TTF in place (returns the new length, 0 if it isn't one)
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806;
    if (len < 12) return 0;
    for (int swap = 0; swap < 2; swap++) {  // the key's byte order differs between descriptions; accept both
        uint32_t k = swap ? __builtin_bswap32(KEY) : KEY;
        uint32_t w0, w1, w2;
        memcpy(&w0, d, 4); memcpy(&w1, d + 4, 4); memcpy(&w2, d + 8, 4);
        uint32_t size = w1 ^ k, ver = w2 ^ k;
        if (size == 0 || size > len - 8 || (ver != 0x00000100u && ver != 0x4F54544Fu)) continue;  // sfnt 1.0 / OTTO
        (void)w0;
        for (size_t i = 0; i + 4 <= size; i += 4) {
            uint32_t w;
            memcpy(&w, d + 8 + i, 4);
            w ^= k;
            memcpy(d + i, &w, 4);
        }
        return size;
    }
    return 0;
}

// .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten)
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if (o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    size_t total = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);  // tag, checksum
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

static int load_one(const char *path, Font *ft) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return 0;
    size_t n = strlen(path);
    if (n > 6 && !strcasecmp(path + n - 6, ".bfttf")) {
        len = bfttf_decode(d, len);
        if (!len) { LOG("[pl] %s: not a valid .bfttf\n", path); free(d); return 0; }
    } else if (len >= 4 && !memcmp(d, "ttcf", 4)) {
        size_t l2;
        uint8_t *f0 = ttc_face0(d, len, &l2);
        free(d);
        if (!f0) { LOG("[pl] %s: bad font collection\n", path); return 0; }
        d = f0;
        len = l2;
    }
    void *p = hle_alloc(len + 16);
    memcpy(p, d, len);
    free(d);
    ft->addr = (uint64_t)p;
    ft->size = len;
    LOG("[pl] system font %s (%zu bytes)\n", path, len);
    return 1;
}

static int g_fonts_loaded;
RT_ON_RESET(pl_reset) { g_fonts_loaded = 0; memset(fonts, 0, sizeof fonts); }  // (they were in the HLE heap)

#ifdef __linux__
// Case-insensitive substring (strcasestr needs _GNU_SOURCE; this is portable).
static int str_has(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

// Fedora / Bazzite and friends don't ship DejaVu (Noto / Liberation / Droid instead), and where they live varies
// per distro, so as a last resort walk the system font directories for a TrueType sans-serif. Noto Sans CJK is CFF
// (the game's font engine crashes on it), so only .ttf files are considered.
struct font_scan { char best[4096]; int score; };

static void font_scan_dir(struct font_scan *s, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) { font_scan_dir(s, path); continue; }
        size_t n = strlen(path);
        if (n < 4 || strcasecmp(path + n - 4, ".ttf")) continue;
        // score the file name: a preferred family beats any other font, weight / style / CJK variants lose
        static const char *const fam[] = {"notosans", "liberationsans", "dejavusans", "droidsans", "freesans",
                                          "arimo", "carlito", "opensans", "roboto", "ubuntu", "cantarell", "lato"};
        static const char *const bad[] = {"bold", "italic", "oblique", "black", "light", "thin", "medium",
                                          "condensed", "mono", "cjk", "fallback", "symbol", "emoji"};
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        int score = 0;
        for (size_t i = 0; i < sizeof fam / sizeof *fam; i++)
            if (str_has(base, fam[i])) { score += 100; break; }
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            if (str_has(base, bad[i])) score -= 50;
        if (str_has(base, "regular")) score += 10;
        if (score > s->score) { s->score = score; strlcpy(s->best, path, sizeof s->best); }
    }
    closedir(d);
}

static char *find_system_font(void) {
    struct font_scan s = {.score = -1};
    font_scan_dir(&s, "/usr/share/fonts");
    font_scan_dir(&s, "/usr/local/share/fonts");
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) { char p[4096]; snprintf(p, sizeof p, "%s/fonts", xdg); font_scan_dir(&s, p); }
    if (home && *home) { char p[4096]; snprintf(p, sizeof p, "%s/.local/share/fonts", home); font_scan_dir(&s, p); }
    return s.score >= 0 ? strdup(s.best) : NULL;
}
#endif

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    const char *env = getenv("ODYSSEY_FONT");
    const char *std[] = {env, "fonts/nintendo_udsg-r_std_003.bfttf", "fonts/nintendo_udsg-r_std_003.ttf",
                         "fonts/nintendo_udsgr_std_003.ttf", "fonts/nintendo_udsgr_std_003.bfttf",
                         "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "C:/Windows/Fonts/YuGothR.ttc",
                         "C:/Windows/Fonts/YuGothM.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf",
                         "C:/Windows/Fonts/arial.ttf",
                         // Linux: DejaVu Sans (Debian / Ubuntu, Arch, Fedora), then Noto / Liberation / Droid Sans
                         // (Fedora / Bazzite, Arch, Debian / Ubuntu). Not Noto Sans CJK: CFF outlines, which the
                         // game's font engine crashes on (it wants TrueType)
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                         "/usr/share/fonts/google-noto-sans-fonts/NotoSans-Regular.ttf",
                         "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                         "/usr/share/fonts/droid-sans-fonts/DroidSans.ttf",
                         "/usr/share/fonts/TTF/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                         "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                         "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf"};
    for (size_t i = 0; i < sizeof std / sizeof *std && !fonts[0].addr; i++)
        if (std[i]) load_one(std[i], &fonts[0]);
#ifdef __linux__
    if (!fonts[0].addr) {  // (any distro layout: walk the font directories)
        char *p = find_system_font();
        if (p) { load_one(p, &fonts[0]); free(p); }
    }
#endif
    if (!fonts[0].addr) {  // (the menu's font, next to the app: always there in a release)
        char shipped[1100];
        if (rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", shipped, sizeof shipped))
            load_one(shipped, &fonts[0]);
    }
    if (!fonts[0].addr) FATAL("no usable system font substitute (and Roboto-Medium.ttf is missing next to the app)\n");
    if (!load_one("fonts/nintendo_ext_003.bfttf", &fonts[1])) load_one("fonts/nintendo_ext_003.ttf", &fonts[1]);
}

// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
static const Font *font_for(uint64_t type) {
    load_fonts();
    return type == 5 && fonts[1].addr ? &fonts[1] : &fonts[0];
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) { c->x[0] = font_for(c->x[0])->addr; }
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) { c->x[0] = font_for(c->x[0])->size; }
