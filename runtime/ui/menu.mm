// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "performance.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "theme.h"
#include "i18n.h"
#include "support_qr.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../mods.h"
#include "../localization.h"
#include "../settings.h"
#include "../controller_motion.h"
#include "menu.h"
#include "setup.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_setup{false};  // the first-start setup page (setup.mm) instead of the menu
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_window_apply{false};
static std::atomic<int> g_last_fullscreen{BORDERLESS};
struct DisplayChoice { SDL_DisplayID id; char label[160]; };
static std::vector<DisplayChoice> g_displays;
static pthread_mutex_t g_displays_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<bool> g_displays_dirty{true};
#ifdef __APPLE__
static id<MTLDevice> g_mtl_dev;  // the device the menu's textures are made on (set in init)
#endif

// SDL display queries and window changes stay on the event thread. The renderer
// only consumes this snapshot to draw the settings row.
static void refresh_displays(void) {
    if (!g_displays_dirty.exchange(false)) return;
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    std::vector<DisplayChoice> choices;
    DisplayChoice automatic = {};
    snprintf(automatic.label, sizeof automatic.label, "Current display");
    choices.push_back(automatic);
    for (int i = 0; ids && i < count && i < 64; i++) {
        DisplayChoice d = {}; d.id = ids[i];
        const char *name = SDL_GetDisplayName(d.id);
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(d.id);
        snprintf(d.label, sizeof d.label, "%d: %s (%d x %d)", i + 1, name ? name : "Display", mode ? mode->w : 0, mode ? mode->h : 0);
        choices.push_back(d);
    }
    SDL_free(ids);
    pthread_mutex_lock(&g_displays_mutex);
    g_displays.swap(choices);
    pthread_mutex_unlock(&g_displays_mutex);
}

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
void menu_setup_mode(bool on) { g_setup.store(on); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
// A notice the game thread waits on in menu_notice_run, like the name entry (runtime/hle/applets.c: a system applet
// this version has nothing to show for)
static std::atomic<int> g_note{0};  // 1 shown, 2 closed
static char g_note_title[128], g_note_text[800];
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || g_note.load() == 1 || g_setup.load(); }
// (frame interpolation's presenter, runtime/nvn/vulkan/device.c, draws the overlay without the render lock) nothing open
// that takes input or runs the menu's actions, and the overlay drawn once (ImGui uploads its fonts on the first frame)
static std::atomic<bool> g_drawn;
extern "C" bool menu_overlay_lockfree(void) { return !capturing() && (!g_settings.show_fps || g_drawn.load()); }
extern "C" bool menu_stats_visible(void) { return g_settings.show_fps || g_settings.performance_overlay; }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_DISPLAY_ADDED || e->type == SDL_EVENT_DISPLAY_REMOVED) {
        g_displays_dirty.store(true);
        g_want_window_apply.store(true);
    }
    static bool back_down, start_down;
    // Controls tab waiting for a new binding: it takes the press - in the menu and on the configuration page before the
    // game (g_setup: there the prompt waited for ever, it never got a key or a click - #40)
    if ((g_open.load() || g_setup.load()) && bind_capture_event(e)) return true;
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    if (g_note.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&  // Enter / Space / Esc: OK
        (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER ||
         e->key.scancode == SDL_SCANCODE_SPACE || e->key.scancode == SDL_SCANCODE_ESCAPE)) {
        g_note = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && !g_setup.load() && (e->key.scancode == SDL_SCANCODE_F1 ||
                                                  (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.window_mode = g_settings.window_mode == WINDOWED ? g_last_fullscreen.load() : WINDOWED;
            g_want_window_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && !g_setup.load() && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_request_window_apply(void) { g_want_window_apply.store(true); }

// The window's display's own modes (event thread; the Display tab lists them): sizes (largest first) and refresh rates
struct ModeSize { int w, h; };
static std::vector<ModeSize> g_mode_sizes;
static std::vector<int> g_mode_rates;
static ModeSize g_desktop_size;
static int g_desktop_rate;
static void refresh_modes(SDL_Window *w) {
    static SDL_DisplayID last;
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    if (!d || (d == last && !g_mode_sizes.empty())) return;
    last = d;
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
    std::vector<ModeSize> sizes;
    std::vector<int> rates;
    for (int i = 0; modes && i < n; i++) {
        bool have = false;
        for (const ModeSize &m : sizes) have = have || (m.w == modes[i]->w && m.h == modes[i]->h);
        if (!have && modes[i]->w >= 640 && modes[i]->h >= 480) sizes.push_back({modes[i]->w, modes[i]->h});
        int r = (int)(modes[i]->refresh_rate + 0.5f);
        if (r >= 24 && std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
    }
    SDL_free(modes);
    std::sort(rates.begin(), rates.end());
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d);
    pthread_mutex_lock(&g_displays_mutex);
    g_mode_sizes.swap(sizes);
    g_mode_rates.swap(rates);
    g_desktop_size = dm ? ModeSize{dm->w, dm->h} : ModeSize{0, 0};
    g_desktop_rate = dm ? (int)(dm->refresh_rate + 0.5f) : 0;
    pthread_mutex_unlock(&g_displays_mutex);
}

// Auto refresh rate (exclusive fullscreen): the highest rate this size offers that is a whole multiple of the frame
// rate (60 FPS: 60 / 120 / 180 / 240 Hz; 90: 90 / 180), so every frame stays on screen equally long; the desktop's when
// there is none. 60 FPS on the desktop's 144 Hz alternated 2 and 3 refreshes a frame, and with Strict / Adaptive VSync
// that judder held the game back (owner's PC, 2026-11-15: 102 images held in ~85 s at 144 Hz).
static float smooth_rate(SDL_DisplayID d, int w, int h, float desktop_hz) {
    int fps = settings_game_hz();
    int n = 0;
    float best = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
    for (int i = 0; modes && i < n; i++) {
        if (modes[i]->w != w || modes[i]->h != h) continue;
        float r = modes[i]->refresh_rate;
        int k = (int)(r / fps + 0.5f);
        if (k >= 1 && fabsf(r - (float)(k * fps)) < 0.6f && r > best) best = r;
    }
    SDL_free(modes);
    return best > 0 ? best : desktop_hz;
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    refresh_displays();
    refresh_modes(w);
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    // Text input only while a text field is being typed into (ui_text_busy) - not whenever the menu is open: in Steam's
    // Gaming Mode turning it on pops up Steam's keyboard over the game (at the start, with the configuration page open).
    // Never on a Steam Deck: the game's own on-screen keyboard (osk.mm) types there.
    extern bool ui_text_busy(void);
    bool want_text = ui_text_busy() && !plat_is_steam_deck();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    bool apply = g_want_window_apply.exchange(false) ||
                 (first && !getenv("ODYSSEY_WINDOW") && !getenv("ODYSSEY_HIDDEN"));
    first = false;
    if (!apply) return;

    int mode = g_settings.window_mode;
    int height = g_settings.display_h, width_set = g_settings.display_w;
    SDL_DisplayID display = SDL_GetDisplayForWindow(w);
    SDL_DisplayID target = 0;
    pthread_mutex_lock(&g_displays_mutex);
    if (g_settings.monitor > 0) {
        target = g_settings.monitor < (int)g_displays.size() ? g_displays[g_settings.monitor].id : SDL_GetPrimaryDisplay();
    }
    pthread_mutex_unlock(&g_displays_mutex);
    if (target && target != display) {
        if (!SDL_SetWindowFullscreen(w, false) || !SDL_SetWindowFullscreenMode(w, NULL) || !SDL_SyncWindow(w) ||
            !SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(target), SDL_WINDOWPOS_CENTERED_DISPLAY(target)) ||
            !SDL_SyncWindow(w)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not move the game to the selected display: %s", SDL_GetError());
            return;
        }
        display = SDL_GetDisplayForWindow(w);
        SDL_Log("Game display: %s", SDL_GetDisplayName(display));
    }
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    bool applied;
    if (mode == WINDOWED) {
        applied = SDL_SetWindowFullscreen(w, false) && SDL_SetWindowFullscreenMode(w, NULL);
        if (applied && height) applied = SDL_SetWindowSize(w, width_set, height);
    } else {
        g_last_fullscreen.store(mode);
        SDL_DisplayMode selected;
        const SDL_DisplayMode *exclusive = NULL;
        if (mode == EXCLUSIVE_FULLSCREEN && desktop) {
            int width = height ? width_set : desktop->w;
            int lines = height ? height : desktop->h;
            float hz = g_settings.refresh_rate > 0 ? (float)g_settings.refresh_rate : desktop->refresh_rate;
            if (g_settings.refresh_rate == 0) hz = smooth_rate(display, width, lines, desktop->refresh_rate);
            if (SDL_GetClosestFullscreenDisplayMode(display, width, lines, hz, true, &selected)) exclusive = &selected;
        }
        applied = (mode != EXCLUSIVE_FULLSCREEN || exclusive) &&
                  SDL_SetWindowFullscreenMode(w, exclusive) && SDL_SetWindowFullscreen(w, true);
        if (!applied && mode == EXCLUSIVE_FULLSCREEN) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Exclusive fullscreen unavailable: %s. Using borderless.", SDL_GetError());
            applied = SDL_SetWindowFullscreenMode(w, NULL) && SDL_SetWindowFullscreen(w, true);
            if (applied) {
                g_last_fullscreen.store(BORDERLESS);
                g_settings.window_mode = BORDERLESS;
                settings_save();
            }
        }
    }
    if (!applied) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not apply display settings: %s", SDL_GetError());
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours (another UI would have its own context)
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Roboto Medium); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The menu opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

// The look (theme.h): charcoal, white text, red as the one accent; square-ish corners; tabs are plain words with a red
// line under the selected one (tabs_body draws it).
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 8; s.FrameRounding = 4; s.GrabRounding = 4; s.TabRounding = 0; s.ChildRounding = 6;
    s.PopupRounding = 6; s.ScrollbarRounding = 4;
    s.WindowPadding = ImVec2(32, 26); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 14; s.TabBarBorderSize = 0;
    ImVec4 *c = s.Colors, none(0, 0, 0, 0);
    // (the menu window: nearly opaque - at the theme's 0.78 the game's own menu showed through behind the rows, its
    // white plates reading as text running into them)
    c[ImGuiCol_WindowBg] = ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 0.93f);
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 0.99f);
    c[ImGuiCol_Border] = kLineV;
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kTextDimV;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Header] = c[ImGuiCol_TableHeaderBg] = kSurfaceV;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = kSurfaceHoverV;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = kAccentDeepV;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabSelectedOverline] = none;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_ScrollbarGrabActive] = kAccentV;
    c[ImGuiCol_SliderGrab] = ImVec4(kAccentV.x, kAccentV.y, kAccentV.z, 0.75f);
    c[ImGuiCol_Separator] = kLineV;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
    g_mtl_dev = dev;
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    // The menu in another language (runtime/ui/i18n.h): its script merged into each font right after that font (ImGui
    // merges into the last one added) - only the characters its translation uses (a few hundred Hangul syllables, not
    // all 11,172: the atlas stays small). Malgun Gothic on Windows; Steam's Go Noto Kurrent on a Steam Deck / Linux.
    static ImVector<ImWchar> xranges;
    static char xfont[600];
    if (const char *all = strcmp(i18n_lang(), "pt-BR") ? i18n_all_text() : NULL) {  // (Portuguese: Latin-1, already in the fonts)
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        b.AddText(all);
        b.AddText("\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");  // (the language's own name in the menu)
        b.BuildRanges(&xranges);
        char steam1[600] = "", steam2[600] = "";
        if (const char *home = getenv("HOME")) {
            snprintf(steam1, sizeof steam1, "%s/.local/share/Steam/clientui/fonts/GoNotoKurrent-Regular.ttf", home);
            snprintf(steam2, sizeof steam2, "%s/.steam/steam/clientui/fonts/GoNotoKurrent-Regular.ttf", home);
        }
        const char *cands[] = {"C:/Windows/Fonts/malgun.ttf", steam1, steam2, "/usr/share/fonts/nanum/NanumGothic.ttf",
                               "/usr/share/fonts/truetype/nanum/NanumGothic.ttf", "/System/Library/Fonts/AppleSDGothicNeo.ttc"};
        for (const char *p : cands) {
            if (!*p) continue;
            if (FILE *f = fopen(p, "rb")) { fclose(f); snprintf(xfont, sizeof xfont, "%s", p); break; }
        }
        if (xfont[0]) LOG("[i18n] menu font for %s: %s\n", i18n_lang(), xfont);
    }
    auto merge = [&](float px) {
        if (!xfont[0]) return;
        ImFontConfig m = cfg;
        m.MergeMode = true;
        m.OversampleH = 1;
        io.Fonts->AddFontFromFileTTF(xfont, px, &m, xranges.Data);
    };
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                           "/usr/share/fonts/google-noto-sans-fonts/NotoSans-Regular.ttf",
                           "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/droid-sans-fonts/DroidSans.ttf",
                           "/usr/share/fonts/TTF/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) { g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg); if (g_font) merge(34); }
        if (g_font && !g_font_big) {
            ImFontConfig b = cfg; b.FontNo = 0;
            g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b);
            if (g_font_big) merge(56);
        }
    }
    // the menu window itself: Roboto Medium (third_party/imgui/misc/fonts, Apache 2.0); the FPS counter, overlays and
    // on-screen keyboard keep the fonts above
    char menu_font[1024];
    if (const char *f = getenv("ODYSSEY_MENU_FONT")) strlcpy(menu_font, f, sizeof menu_font);  // (test) another face
    if (getenv("ODYSSEY_MENU_FONT") ||
        rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", menu_font, sizeof menu_font)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(menu_font, 36, &cfg);
        if (g_menu_font) merge(36);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(menu_font, 58, &cfg);
        if (g_menu_font_big) merge(58);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header: in the accent, capitals, as the option rows' sections (option_rows.mm opt_section).
static void header(const char *t) {
    t = tr(t);
    char up[96];
    int i = 0;
    for (; t[i] && i < (int)sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)t[i]);
    up[i] = 0;
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kAccentV, "%s", up);
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(t));
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", tr(names[*value]))) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tr(names[i]), i == *value)) { *value = i; changed = true; }
            ImGui::PopID();
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
#ifndef __APPLE__
extern "C" void vk_request_swapchain_remake(void);
#endif
extern "C" void output_scale_preview(double dw, double dh, double want, int mode, double *w, double *h);
extern "C" float odyssey_wide_shape(void);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
#ifndef __APPLE__
extern "C" bool vk_temporal_enabled(void);  // runtime/nvn/vulkan/dlss_probe.c
extern "C" bool vk_fsr2_available(void);    // runtime/nvn/vulkan/fsr2.c
#endif
#ifdef _WIN32
extern "C" bool vk_fsr3_available(void);
extern "C" bool vk_fsr4_available(void);
extern "C" bool vk_xess_available(void);
#endif

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    bool fs = g_settings.window_mode != WINDOWED;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.window_mode = fs ? BORDERLESS : WINDOWED;
        g_want_window_apply.store(true);
    }
    {   // the monitor's own modes, by their exact sizes and rates (refresh_modes)
        pthread_mutex_lock(&g_displays_mutex);
        std::vector<ModeSize> sizes = g_mode_sizes;
        std::vector<int> rates = g_mode_rates;
        ModeSize desk = g_desktop_size;
        int desk_rate = g_desktop_rate;
        pthread_mutex_unlock(&g_displays_mutex);
        static char labels[66][48];
        const char *lp[66];
        int n = 0, sel = 0;
        snprintf(labels[n], sizeof labels[n], desk.w ? "Desktop (%d x %d)" : "Desktop", desk.w, desk.h);
        lp[n] = labels[n], n++;
        for (const ModeSize &m : sizes) {
            if (n >= 66) break;
            snprintf(labels[n], sizeof labels[n], "%d x %d", m.w, m.h);
            if (m.w == g_settings.display_w && m.h == g_settings.display_h) sel = n;
            lp[n] = labels[n], n++;
        }
        bool borderless = g_settings.window_mode == BORDERLESS;
        if (borderless) opt_disable_next();
        if (opt_choice("Display resolution", &sel, lp, n, 0,
                       "The window's size, or in exclusive fullscreen the monitor's mode - the sizes your monitor offers. "
                       "Desktop: the current window size, or the desktop's resolution in exclusive fullscreen. Borderless "
                       "always uses the desktop resolution.")) {
            if (sel == 0) g_settings.display_w = g_settings.display_h = 0;
            else g_settings.display_w = sizes[sel - 1].w, g_settings.display_h = sizes[sel - 1].h;
            g_settings.display_resolution = DISPLAY_AUTO;
            g_want_window_apply.store(true);
        }
        static char rlabels[34][32];
        const char *rp[34];
        int rn = 0, rsel = 0;
        snprintf(rlabels[rn], sizeof rlabels[rn], "Auto"); (void)desk_rate;
        rp[rn] = rlabels[rn], rn++;
        for (int r : rates) {
            if (rn >= 34) break;
            snprintf(rlabels[rn], sizeof rlabels[rn], "%d Hz", r);
            if (r == g_settings.refresh_rate) rsel = rn;
            rp[rn] = rlabels[rn], rn++;
        }
        if (g_settings.window_mode != EXCLUSIVE_FULLSCREEN) opt_disable_next();
        if (opt_choice("Refresh rate", &rsel, rp, rn, 0,
                       "The monitor's refresh rate in exclusive fullscreen (windowed and borderless use the desktop's: change "
                       "it in your system's display settings). Auto: the highest rate your monitor offers that is a multiple "
                       "of the frame rate (120 Hz on a 144 Hz monitor at 60 FPS), so every frame is shown for the same time - "
                       "the smoothest picture. Other rates (144 Hz at 60 FPS) show frames for uneven times: judder.")) {
            g_settings.refresh_rate = rsel == 0 ? 0 : rates[rsel - 1];
            g_want_window_apply.store(true);
        }
    }
#ifndef __APPLE__
    {
        static const char *vs[] = {"On", "Off (tearing)", "Strict", "Adaptive"};
        if (opt_choice("VSync", &g_settings.vsync, vs, VSYNC_COUNT, VSYNC_ON,
                       "On: no tearing, the game keeps its own steady pace (recommended). Off: frames go out the moment "
                       "they're done - the lowest latency, with tearing. Strict: each frame waits for the screen's next "
                       "refresh (classic vsync; a late frame waits a whole refresh). Adaptive: as Strict, but a late frame "
                       "goes out at once (tearing only then). Where the GPU or driver lacks one, the closest it has is used.")) {
            vk_request_swapchain_remake();
        }
    }
#endif
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    opt_toggle("Performance overlay", &g_settings.performance_overlay, false,
               "Shows the frame rate and frame times with a graph, GPU / CPU / memory use, temperatures and power where "
               "your PC reports them, the upscaler and resolution, and Mario's position. Hidden while this menu is open. "
               "Choose its parts below.");
    {
        static const char *const corners[OVERLAY_CORNER_COUNT] = {"Top right", "Top left", "Bottom right", "Bottom left"};
        static const char *const sizes[3] = {"Small", "Medium", "Large"};
        if (!g_settings.performance_overlay) opt_disable_next();
        opt_choice("Overlay position", &g_settings.overlay_corner, corners, OVERLAY_CORNER_COUNT, OVERLAY_TOP_RIGHT,
                   "The corner of the picture the performance overlay sits in.");
        if (!g_settings.performance_overlay) opt_disable_next();
        opt_choice("Overlay size", &g_settings.overlay_size, sizes, 3, 1, "The size of the performance overlay's text.");
        float opacity = (float)g_settings.overlay_opacity;
        if (!g_settings.performance_overlay) opt_disable_next();
        if (opt_slider("Overlay background", &opacity, 0, 100, 10, 80, "%g%%",
                       "How solid the overlay's background is. 0% shows only the text."))
            g_settings.overlay_opacity = (int)(opacity + 0.5f);
        struct { const char *label; bool *on; const char *help; } parts[] = {
            {"Show GPU", &g_settings.overlay_gpu, "GPU use, temperature, power draw, video memory and clock speed. "
             "Temperature, power and clock need an NVIDIA card, or Linux with an AMD or Intel one."},
            {"Show CPU", &g_settings.overlay_cpu, "CPU use, the game's own share of it, and on Linux the CPU's temperature."},
            {"Show memory", &g_settings.overlay_ram, "The PC's memory in use, and the game's own."},
            {"Show frame time graph", &g_settings.overlay_graph, "A line of the last frames' times: flat when the game runs evenly, a spike at each hitch."},
            {"Show hardware names", &g_settings.overlay_hardware, "The names of the graphics card and the processor."},
            {"Show upscaler and resolution", &g_settings.overlay_render, "The upscaler and anti-aliasing in use, and the game's internal resolution next to its size on screen."},
            {"Show Mario's position", &g_settings.overlay_position, "Mario's position in the kingdom, in metres. Useful for bug reports."},
        };
        for (auto &part : parts) {
            if (!g_settings.performance_overlay) opt_disable_next();
            opt_toggle(part.label, part.on, true, part.help);
        }
    }
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_wide.c)
    {
        static const char *asp[] = {"16:9", "Fill window", "16:10", "21:9", "32:9", "4:3"};
        opt_choice("Aspect ratio", &g_settings.aspect, asp, ASPECT_COUNT, ASPECT_16_9,
                   "The picture's shape, on any screen. 16:9: the Switch's picture. Fill window: the screen's own shape. "
                   "16:10, 21:9, 32:9, 4:3: that shape - where the screen's is different, black bars fill the rest (21:9 "
                   "on a 16:9 TV: bars above and below; 16:9 on an ultrawide: bars at the sides). A wider shape shows more "
                   "of the world at the sides, a narrower one a little less; the HUD keeps its shape and stays in the "
                   "corners of the picture.");
    }
    {
        static const char *pos[] = {"Centered (16:9)", "Edges"};
        opt_choice("HUD position (wide)", &g_settings.hud_edges, pos, 2, 1,
                   "In a picture wider than 16:9 (21:9, 32:9, Fill on an ultrawide). Edges: the coin and moon counters, "
                   "the life gauge, the button guides and the mini map go to the picture's corners, as on a 16:9 screen. "
                   "Centered: the whole HUD in a 16:9 box in the middle. Menus and messages stay in the middle either way.");
    }
    {
        static const char *modes[] = {"Docked (TV)", "Handheld"};
        opt_choice("Console mode", &g_settings.console_mode, modes, 2, plat_is_steam_deck() ? CONSOLE_HANDHELD : CONSOLE_DOCKED,
                   "What the game is told it runs as. Docked: its TV rendering (1600 x 900 before scaling, docked effects). "
                   "Handheld: its own handheld rendering (1280 x 720, lighter effects), as on a Switch Lite - much less "
                   "work for the GPU, for handhelds and low-power PCs. Applies at the next start.");
    }
    opt_toggle("Dynamic resolution", &g_settings.game_dynres, !plat_is_steam_deck(),
               "On: as on the Switch - the game lowers its resolution when frames take long. Off: always its full "
               "resolution (sharper; needs a fast enough GPU or a higher power limit). Applies at the next start.");
#endif

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res_base[] = {"25%", "50%", "75%", "100%", "Match display", "Dynamic", "1x", "1440p", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_DISPLAY, RES_MATCH, RES_DYNAMIC, 1, RES_1440, 2, 3, 4};
    // each with the exact size it renders at in this window and picture shape ("Match display (2560 x 1440)")
    static char res_lab[11][48];
    const char *res[11];
    {
        ImGuiIO &io = ImGui::GetIO();
        double dw = io.DisplaySize.x * io.DisplayFramebufferScale.x, dh = io.DisplaySize.y * io.DisplayFramebufferScale.y;
#ifdef __APPLE__
        double shape = 1.0;
#else
        double shape = odyssey_wide_shape();
#endif
        for (int i = 0; i < 11; i++) {
            double rw = 0, rh = 0;
            if (dw > 0 && dh > 0) output_scale_preview(dw, dh, shape, res_mode[i], &rw, &rh);
            if (rw <= 0) snprintf(res_lab[i], sizeof res_lab[i], "%s", res_base[i]);
            else if (res_mode[i] == RES_MATCH) snprintf(res_lab[i], sizeof res_lab[i], "%.0f x %.0f", rw, rh);  // (the
            // screen's resolution, as it is - "Match display (2560 x 1440)" was cut off in the row; owner 2026-11-16)
            else if (res_mode[i] == RES_DYNAMIC) snprintf(res_lab[i], sizeof res_lab[i], "Dynamic (up to %.0f x %.0f)", rw, rh);
            else if (i < 5) snprintf(res_lab[i], sizeof res_lab[i], "%s (%.0f x %.0f)", res_base[i], rw, rh);
            else snprintf(res_lab[i], sizeof res_lab[i], "%.0f x %.0f", rw, rh);
            res[i] = res_lab[i];
        }
    }
    enum { RES_MATCH_AT = 4 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[1024];  // (400 cut the text off mid-sentence - and the Dynamic line below then wrote past the end)
        int n = snprintf(d, sizeof d, "How sharp the 3D image is - each choice shows the size it renders at in this window "
                                      "and picture shape. The default, shown as your screen's resolution: that resolution (at least the "
                                      "Switch's 1600 x 900). 100%%: exactly your screen's pixels (Steam Deck: 1280 x 800). "
                                      "Dynamic: as sharp as your GPU keeps at full speed. 75%% / 50%% / 25%%: a share of "
                                      "the display, for slower machines; the fixed sizes render at exactly that, above the "
                                      "screen's for a sharper picture (supersampling).\n\nRendering at %.0f x %.0f.",
                         1600.0 * sx, 900.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC && n >= 0 && (size_t)n < sizeof d)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
#ifndef __APPLE__  // (temporal upscaling: Windows, and FSR 2 on Linux)
        if (vk_temporal_enabled()) {
            snprintf(d, sizeof d, "Your screen's resolution (the default) lets the temporal upscaler choose its usual input size. A fixed resolution overrides that "
                     "size. At or above the display resolution, upscaling is bypassed. Native AA/DLAA applies at equal sizes.\n\n"
                     "Rendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        }
#endif
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH
#ifndef __APPLE__  // (temporal upscaling: Windows, and FSR 2 on Linux)
            && !vk_temporal_enabled()
#endif
        ) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at your screen's resolution (the default): the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Spatial upscaler", &ui, ups, 2, 1, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
#ifdef _WIN32
    opt_section("Upscaling");
    {   // Off first, off by default (settings: temporal_off): the game renders plainly at the internal resolution
        static const char *methods[] = {"Off", "NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4 (Beta)", "Intel XeSS (Beta)", "AMD FSR 2"};
        int m = g_settings.temporal_off ? 0 : g_settings.temporal_backend + 1;
        if (opt_choice("Upscale method", &m, methods, TEMPORAL_BACKEND_COUNT + 1, 0,
                       "How the picture is reconstructed and upscaled. Off: the game renders plainly at the internal "
                       "resolution (the Spatial upscaler above scales it to the window). NVIDIA DLSS, AMD FSR and Intel XeSS: "
                       "temporal upscaling and anti-aliasing - sharper at a lower internal resolution, needing the GPU and "
                       "driver for it. Each keeps its own mode and sharpening settings. Changes during play.")) {
            g_settings.temporal_off = m == 0;
            if (m > 0) g_settings.temporal_backend = m - 1;
        }
    }
    if (!g_settings.temporal_off) {  // frame generation (sl_glue.cpp: NVIDIA DLSS; AMD FSR frame generation isn't ported)
        static const char *fgn[] = {"Off", "NVIDIA DLSS (Beta)"};
        int sel = g_settings.frame_gen == FG_DLSS ? 1 : 0;
        if (opt_choice("Frame generation", &sel, fgn, 2, 0,
                       "A generated frame between each two the game renders: 120 FPS shown from the game's 60 - smoother "
                       "motion on a 120 Hz or faster display, a little more input delay (Low latency below takes most of it "
                       "back). NVIDIA DLSS: GeForce RTX 40 series or newer, with Reflex; applies the next time the game "
                       "starts. Needs Frame rate 60 FPS and 120 FPS interpolation off. The HUD can shimmer slightly on "
                       "generated frames."))
            g_settings.frame_gen = sel ? FG_DLSS : FG_OFF;
        if (g_settings.frame_gen && (settings_game_hz() != 60 || g_settings.frame_interp)) {
            opt_disable_next();
            opt_action("Frame generation status", "Off: set Frame rate 60 FPS, 120 FPS interpolation off",
                       "Frame generation waits until the game renders 60 FPS without its own extra frames (Performance tab).");
        }
    }
    {   // NVIDIA Reflex (sl_glue.cpp): the CPU waits just before a frame instead of queuing ahead - less input delay
        static const char *lln[] = {"Off", "On", "On + Boost"};
        opt_choice("Low latency (NVIDIA Reflex)", &g_settings.low_latency, lln, 3, 0,
                   "Less delay between your input and the picture: the game starts each frame as late as it can instead of "
                   "queuing frames ahead. Boost also keeps the GPU clocks up. NVIDIA GPUs; always on with DLSS frame "
                   "generation. Applies the next time the game starts.");
    }
    int backend = g_settings.temporal_backend;
    bool fsr = backend == TEMPORAL_FSR3 || backend == TEMPORAL_FSR4 || backend == TEMPORAL_FSR2;
    if (g_settings.temporal_off) {
        // (nothing more to set: the method's own rows show when one is chosen)
    } else if (backend != TEMPORAL_DLSS) {
        static const char *modes[] = {"Off", "Native AA", "Quality", "Balanced", "Performance"};
        const char *help = backend == TEMPORAL_XESS ?
            "Experimental Intel XeSS-SR. Native AA, Quality (1.7x), Balanced (2x) and Performance (2.3x). Requires a compatible Vulkan driver and Intel's runtime. Changes during play." :
            "AMD temporal reconstruction. Native AA uses display resolution; other modes render at lower input resolutions. Changes during play.";
        if (backend == TEMPORAL_FSR4 && !vk_fsr4_available()) help =
            "FSR 4 ML is unavailable on this GPU/runtime. FSR 3.1 is used when supported; otherwise the game uses regular rendering. The status below shows the active path.";
        opt_choice(backend == TEMPORAL_XESS ? "XeSS mode" : backend == TEMPORAL_FSR4 ? "FSR 4 mode" :
                   backend == TEMPORAL_FSR2 ? "FSR 2 mode" : "FSR 3.1 mode",
                   settings_temporal_mode(), modes, DLSS_MODE_COUNT, DLSS_QUALITY, help);
        const char *status = *settings_temporal_mode() == DLSS_OFF ? "Off" : backend == TEMPORAL_FSR2 ?
            (vk_fsr2_available() ? "FSR 2" : "Regular rendering") : backend == TEMPORAL_XESS ?
            (vk_xess_available() ? "XeSS" : "Regular rendering") : backend == TEMPORAL_FSR4 && vk_fsr4_available() ? "FSR 4 ML" :
            vk_fsr3_available() ? (backend == TEMPORAL_FSR4 ? "FSR 3.1 fallback" : "FSR 3.1") : "Regular rendering";
        opt_disable_next(); opt_action("Upscaler status", status, "The available rendering path. Unsupported experimental techniques do not run on incompatible hardware.");
    } else {
        static const char *modes[] = {"Off", "DLAA", "Quality", "Balanced", "Performance"};
        static const char *presets[] = {"Default", "K", "L", "M"};
        opt_choice("DLSS mode", &g_settings.dlss_mode, modes, DLSS_MODE_COUNT, DLSS_OFF,
                   "DLAA anti-aliases at display resolution. Quality, Balanced and Performance render at progressively "
                   "lower resolutions. Changes apply during play; history resets while the new mode is prepared.");
        opt_choice("DLSS preset", &g_settings.dlss_preset, presets, DLSS_PRESET_COUNT, DLSS_PRESET_DEFAULT,
                   "Model hint for NVIDIA DLSS. Default uses K for DLAA/Quality/Balanced and M for Performance. Changes during play.");
    }
    if (!g_settings.temporal_off && *settings_temporal_mode() != DLSS_OFF) {
        float strength = (float)*settings_temporal_sharpness();
        if (opt_slider(fsr ? "FSR sharpness" : backend == TEMPORAL_XESS ? "XeSS sharpness (RCAS)" : "DLSS sharpness (RCAS)",
                       &strength, 0, 100, 10, 20, "%g%%", "Reconstruction sharpening. 0% bypasses it. Saved independently for each technique; changes at once."))
            *settings_temporal_sharpness() = (int)strength;
        opt_toggle("CAS sharpening", settings_temporal_cas(), false, "Optional CAS after reconstruction, independent of the sharpening above.");
        if (!*settings_temporal_cas()) opt_disable_next();
        float amount = (float)*settings_temporal_cas_sharpness();
        if (opt_slider("CAS sharpness", &amount, 0, 100, 10, 20, "%g%%", "Optional CAS strength, saved separately for each technique. 0% bypasses the pass."))
            *settings_temporal_cas_sharpness() = (int)amount;
    } else if (g_settings.upscaler == UPSCALE_FSR1) {
        float amount = (float)g_settings.fsr1_sharpness;
        if (opt_slider("Sharpness", &amount, 0, 100, 10, 90, "%g%%", "FSR 1 RCAS sharpening. 0% bypasses the pass."))
            g_settings.fsr1_sharpness = (int)amount;
    }
#elif !defined(__APPLE__)
    // Linux / Steam Deck: AMD FSR 2, run by our own code (runtime/nvn/vulkan/fsr2.c) - the vendors' other runtimes are
    // Windows DLLs. It shares FSR 3.1's mode and sharpness settings.
    opt_section("Upscaling");
    {
        static const char *methods[] = {"Off", "AMD FSR 2"};
        int m = !g_settings.temporal_off && g_settings.temporal_backend == TEMPORAL_FSR2;
        if (opt_choice("Upscale method", &m, methods, 2, 0,
                       "How the picture is reconstructed and upscaled. Off: the game renders plainly at the internal "
                       "resolution (the Spatial upscaler above scales it to the window). AMD FSR 2: temporal upscaling and "
                       "anti-aliasing - the game renders fewer pixels and FSR 2 rebuilds the picture at the display "
                       "resolution from several frames, sharper than a plain upscale. Changes during play.")) {
            g_settings.temporal_off = m == 0;
            if (m) g_settings.temporal_backend = TEMPORAL_FSR2;
        }
    }
    if (!g_settings.temporal_off && g_settings.temporal_backend == TEMPORAL_FSR2) {
        static const char *modes[] = {"Off", "Native AA", "Quality", "Balanced", "Performance"};
        opt_choice("FSR 2 mode", &g_settings.fsr3_mode, modes, DLSS_MODE_COUNT, DLSS_QUALITY,
                   "Native AA: renders at the display resolution, anti-aliasing only. Quality (1.5x), Balanced (1.7x) and "
                   "Performance (2x): render at lower resolutions - less work for the GPU. Changes during play.");
        opt_disable_next();
        opt_action("Upscaler status", g_settings.fsr3_mode == DLSS_OFF ? "Off" : vk_fsr2_available() ? "FSR 2" : "Regular rendering",
                   "The rendering path in use.");
        float strength = (float)g_settings.fsr3_sharpness;
        if (opt_slider("FSR sharpness", &strength, 0, 100, 10, 20, "%g%%", "Reconstruction sharpening (RCAS). 0% bypasses it."))
            g_settings.fsr3_sharpness = (int)strength;
    } else if (g_settings.upscaler == UPSCALE_FSR1) {
        float amount = (float)g_settings.fsr1_sharpness;
        if (opt_slider("Sharpness", &amount, 0, 100, 10, 90, "%g%%", "FSR 1 RCAS sharpening. 0% bypasses the pass."))
            g_settings.fsr1_sharpness = (int)amount;
    }
#endif
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_120.c)
    opt_section("Frame rate");
    {
        // (listed in order; the setting keeps its numbers: FPS_* in settings.h)
        static const int order[FPS_COUNT] = {FPS_60, FPS_72, FPS_90, FPS_100, FPS_120, FPS_144, FPS_165, FPS_180, FPS_240, FPS_AUTO};
        static char autolab[48];
        snprintf(autolab, sizeof autolab, "Match display (%d FPS)", settings_fps_hz(FPS_AUTO));
        static const char *fps[FPS_COUNT] = {"60 FPS", "72 FPS", "90 FPS", "100 FPS", "120 FPS", "144 FPS", "165 FPS",
                                             "180 FPS", "240 FPS", autolab};
        int pos = 0;
        for (int i = 0; i < FPS_COUNT; i++) if (order[i] == g_settings.frame_rate) pos = i;
        opt_choice("Frame rate", &pos, fps, FPS_COUNT, 0,
                   "Above 60, the game draws that many frames per second, each a real frame showing Mario, the camera and "
                   "the world where they are at that moment (no generated in-between images); gameplay itself - physics, "
                   "enemies, timers - keeps the game's own 60 steps a second, as it was made. Choose your display's "
                   "refresh rate or a divisor of it (a 144 Hz screen: 72 or 144), and a PC that can draw that fast - "
                   "60 FPS on a 144 Hz screen can't be smooth: frames alternate 2 and 3 refreshes (a judder no VSync "
                   "setting removes; G-Sync / FreeSync does). Match display picks that rate for you. "
                   "60 FPS matches the Switch and uses the least power. Changes at once.", false);
        g_settings.frame_rate = order[pos];
    }
    if (settings_game_hz() != 60) opt_disable_next();
#ifndef __APPLE__  // (temporal upscaling: Windows, and FSR 2 on Linux)
    if (vk_temporal_enabled()) opt_disable_next();
#endif
    opt_toggle("120 FPS interpolation (experimental)", &g_settings.frame_interp, false,
               "Experimental: some players' games crashed with it on (NVIDIA and AMD) - turn it off if the game crashes. "
               "Available with Frame rate set to 60 FPS. Adds an image between game frames, targeting 120 FPS "
               "on a 120 Hz or faster display. Busy scenes can temporarily fall back to 60 FPS.");
    static const char *interp_outputs[] = {"Auto", "120 fps (VRR)"};
    int output = g_settings.interpolation_output == 120;
    if (opt_choice("Interpolation output", &output, interp_outputs, 2, 1,
                   "Auto uses the current/desktop refresh rate. 120 fps (VRR) keeps interpolation enabled when a "
                   "high-refresh display is temporarily reported at 60 Hz with VRR. Requires a mode of at least 120 Hz "
                   "at the same resolution. Game logic stays at 60 Hz; GPU load and the interpolation safety brake can "
                   "still reduce output."))
        g_settings.interpolation_output = output ? 120 : 0;
#endif
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/textures.c "Banding")
    opt_toggle("Smooth gradients", &g_settings.deband, true,
               "Removes the visible color steps (banding) in skies, fog and dark areas: the game's image keeps more "
               "precision until it reaches the screen and gets a fine dither there. Off: as on the Switch.");
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true,
               "The game's echo effect in caves and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's videos (e.g. the control tips). Applies the next time a video starts.");
    opt_end();
}

// Mods (runtime/mods.h): the folders in mods/ with game files, each on or off (applies the next time the game starts).
static void tab_mods(float scale) {
    const char *how = "Put each mod in its own folder in here, with its romfs folder inside - as mods for the Switch "
                      "emulators come: mods/<Mod name>/romfs/...\n\nMods made for version 1.0.0 of the game work best. "
                      "If two mods change the same file, the one higher in the list wins.\n\nCode mods (exefs: IPS / "
                      "pchtxt patches, Super Mario Odyssey: Online) can't be used: the game code is recompiled from the "
                      "original game on your PC.";
    opt_begin("##mods", scale);
    int n = mods_count();
    opt_section(mods_changed() ? "Mods - restart the game to apply" : "Mods");
    if (!n) {
        opt_disable_next();
        opt_action("No mods found", "", how);
    }
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_get(i);
        char d[1600];
        int k = snprintf(d, sizeof d, "mods/%s\n\n", m->id);
        if (!m->romfs) {
            snprintf(d + k, sizeof d - k, "A code mod: it changes the game code, which can't be used here - the game "
                                          "code is recompiled from the original game on your PC.");
        } else {
            k += snprintf(d + k, sizeof d - k, "%d file%s", m->files, m->files == 1 ? "" : "s");
            if (m->applied)
                k += snprintf(d + k, sizeof d - k, ": %d replace%s the game's, %d %s new.", m->replaced,
                              m->replaced == 1 ? "s" : "", m->added, m->added == 1 ? "is" : "are");
            else
                k += snprintf(d + k, sizeof d - k, ".");
            if (m->applied && m->shadowed)
                k += snprintf(d + k, sizeof d - k, " %d %s also in a mod higher in this list, which wins.", m->shadowed,
                              m->shadowed == 1 ? "is" : "are");
            if (m->applied && m->skipped)
                k += snprintf(d + k, sizeof d - k, " %d %s in %s the game doesn't have, left out.", m->skipped,
                              m->skipped == 1 ? "is" : "are", m->skipped == 1 ? "a folder" : "folders");
            if (m->code) k += snprintf(d + k, sizeof d - k, " Its code patches (exefs) can't be used here.");
            snprintf(d + k, sizeof d - k, "\n\nSwitching it on or off applies the next time the game starts.");
        }
        bool on = m->romfs && m->on;
        ImGui::PushID(i);  // (two mods may have the same name)
        if (!m->romfs) opt_disable_next();
        if (opt_toggle(m->name, &on, true, d)) mods_set_on(i, on);
        ImGui::PopID();
    }
    opt_section("Folder");
    char url[1600];
    if (opt_action("Open mods folder", "", how) && mods_dir_url(url, sizeof url)) SDL_OpenURL(url);
    opt_end();
}

// Quit: out to the desktop, after asking (progress since the last save is lost); B / Esc or Cancel backs out.
static void tab_quit(float scale, bool in_game) {
    static bool ask;
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = false;
    const char *d = in_game ? "Closes " RT_APP_NAME ".\n\nProgress since your last save is lost." : "Closes " RT_APP_NAME ".";
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (opt_action("Quit to Desktop", "", d)) ask = true;
    } else {
        if (opt_action("Yes, quit to Desktop", "", d)) {
            settings_save();
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
            ask = false;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = false;
    }
    opt_end();
}

static void camera_speed(const char *label, int *value) {
    static const int pct[] = {25, 50, 75, 100, 150, 200, 300, 400};
    static const char *const names[] = {"25%", "50%", "75%", "100%", "150%", "200%", "300%", "400%"};
    int selected = 0;
    for (int k = 0; k < 8; k++) if (pct[k] <= *value) selected = k;
    if (choice(label, &selected, names, 8)) *value = pct[selected];
}

// The menu's own language (this menu and the configuration screen), apart from the game's text language (Controls)
static void tab_language(void) {
    header("Menu language");
    // the same list as the game's language (names in English, so they show in any menu language); only Korean and
    // Brazilian Portuguese have a translation (runtime/ui/lang/): the others show the menu in English
    if (choice("Language", &g_settings.menu_language, g_language_names, MENU_LANG_COUNT)) settings_save();
    hint("This menu's own language. Auto: the game's language when the menu has a translation for it (Korean, "
         "Brazilian Portuguese), else English. Applies the next time the game starts.");
    hint("This covers this menu and the configuration screen. The game's own text language is in Controls > System language.");
}

static void tab_about(void) {
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language. Auto keeps the one your save has (the game's own Options > Language changes it); "
         "any other choice replaces it. Applies the next time the game starts.");
    if (g_settings.language == LANG_PT_BR) hint(localization_ptbr_message());
    ImGui::Spacing();
    header("Controller");
    toggle("Motion controls", &g_settings.motion);
    hint("The controller's gyro and accelerometer for the game's motion moves: shake it to throw Cappy at a target, "
         "spin him, or tilt where the game asks. Needs a controller with motion sensors (PlayStation, Switch Pro, "
         "Steam Deck).");
    {   // the shake button / key (runtime/hle/input.c hid_sixaxis_states)
        static const int pads[] = {-1, 7, 8, 16, 17, 18, 19, 20, 15};  // (PADB values: none, L3, R3, paddles, touchpad, misc)
        static const char *const pnames[] = {"None", "Left stick press", "Right stick press", "Right paddle 1",
                                             "Left paddle 1", "Right paddle 2", "Left paddle 2", "Touchpad", "Share / Capture"};
        int pi = 0;
        for (int k = 0; k < 9; k++) if (pads[k] == g_settings.motion_shake_pad) pi = k;
        if (choice("Shake button", &pi, pnames, 9)) g_settings.motion_shake_pad = pads[pi];
        static const int keys[] = {0, BIND_MOUSE_MIDDLE, BIND_MOUSE_X1, BIND_MOUSE_X2, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT,
                                   SDL_SCANCODE_R, SDL_SCANCODE_TAB, SDL_SCANCODE_V};
        static const char *const knames[] = {"None", "Middle mouse button", "Mouse button 4", "Mouse button 5",
                                             "Left Ctrl", "Left Shift", "R", "Tab", "V"};
        int ki = 0;
        for (int k = 0; k < 9; k++) if (keys[k] == g_settings.motion_shake_key) ki = k;
        if (choice("Shake key", &ki, knames, 9)) g_settings.motion_shake_key = keys[ki];
    }
    hint("Shake without moving the controller: while this button or key is held, the game sees the controller shaken - "
         "the motion cap throw (homing at a target), spins and shake moves. Works with Motion controls off and on "
         "controllers or keyboards without motion sensors. If a key is also bound to a game button, it does both.");
    toggle("Vibration", &g_settings.rumble);
    hint("The controller's rumble, as the game sends it (the Switch's HD rumble on the controller's motors).");
    toggle("HD rumble (ProCon2Bridge)", &g_settings.hd_rumble);
    hint("The Switch 2 Pro Controller's HD haptics through the ProCon2Bridge loopback (experimental). "
         "Off: the controller's regular motors.");
    {
        static const int pct[] = {25, 50, 75, 100};
        static const char *const names[] = {"25%", "50%", "75%", "100%"};
        int i = 3;
        for (int k = 0; k < 4; k++) if (pct[k] == g_settings.rumble_strength) i = k;
        if (choice("Vibration strength", &i, names, 4)) {
            g_settings.rumble_strength = pct[i];
            settings_save();
        }
    }
    hint("How strong the controller's vibration is (25..100%).");
    if (ImGui::Button("Test vibration (2 seconds)")) pm_vibration_test();
    {
        static const int pct[] = {0, 5, 10, 15, 20, 25, 30};
        static const char *const names[] = {"Off", "5%", "10%", "15%", "20%", "25%", "30%"};
        int i = 0;
        for (int k = 0; k < 7; k++) if (pct[k] <= g_settings.stick_deadzone) i = k;
        if (choice("Stick deadzone", &i, names, 7)) g_settings.stick_deadzone = pct[i];
    }
    hint("How far a controller stick moves before it counts. Raise it if Mario or the camera drifts on their own.");
    ImGui::Spacing();
    header("Stick camera");
    camera_speed("Stick horizontal speed", &g_settings.stick_camera_x);
    camera_speed("Stick vertical speed", &g_settings.stick_camera_y);
    toggle("Invert stick horizontal", &g_settings.stick_camera_invert_x);
    toggle("Invert stick vertical", &g_settings.stick_camera_invert_y);
    hint("The main gameplay camera only. Mario's movement and menu navigation keep their normal controls. "
         "The game's own camera inversion options also apply.");
    ImGui::Spacing();
    header("Mouse");
    toggle("Mouse camera", &g_settings.mouse_camera);
    hint("Moving the mouse turns the camera. While you play, the pointer stays in the window; it is free again in this "
         "menu (Esc) or when you switch to another window.");
    camera_speed("Mouse horizontal speed", &g_settings.mouse_sensitivity);
    camera_speed("Mouse vertical speed", &g_settings.mouse_sensitivity_y);
    hint("Separate speed for each mouse axis. A quick flick turns the camera faster still.");
    toggle("Invert mouse left / right", &g_settings.mouse_invert_x);
    toggle("Invert mouse up / down", &g_settings.mouse_invert_y);
    hint("Moving the mouse up turns the camera down instead.");
    {
        static const char *const names[PROMPTS_COUNT] = {"Auto", "Controller", "Keyboard and mouse", "Switch", "Xbox",
                                                         "PlayStation"};
        int i = g_settings.prompts >= 0 && g_settings.prompts < PROMPTS_COUNT ? g_settings.prompts : PROMPTS_AUTO;
        if (choice("Button prompts", &i, names, PROMPTS_COUNT)) g_settings.prompts = i;
    }
    hint("The game's button icons. Auto switches as you play: the keys you bound (and the mouse for the camera) after "
         "you use the keyboard or mouse, the controller's own buttons (Xbox, PlayStation or Switch) after you use a "
         "controller. Controller always shows the connected controller's; Switch, Xbox and PlayStation always those.");
    ImGui::Spacing();
    header("Controls");
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}

// The PayPal QR code as an ImGui texture (made once, on the render thread; the texture is
// pre-upscaled so each module is a solid block and stays crisp under linear sampling).
static ImTextureID support_qr_texture(void) {
    static ImTextureID tex = 0;
    if (tex) return tex;
    static uint8_t rgba[SUPPORT_QR_TEX_W * SUPPORT_QR_TEX_H * 4];
    support_qr_rgba(rgba);
#ifdef __APPLE__
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                width:SUPPORT_QR_TEX_W height:SUPPORT_QR_TEX_H
                                                                              mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_mtl_dev newTextureWithDescriptor:d];
    [t replaceRegion:MTLRegionMake2D(0, 0, SUPPORT_QR_TEX_W, SUPPORT_QR_TEX_H) mipmapLevel:0 withBytes:rgba
         bytesPerRow:SUPPORT_QR_TEX_W * 4];
    tex = (ImTextureID)t;
#else
    tex = (ImTextureID)vk_imgui_texture_rgba(rgba, SUPPORT_QR_TEX_W, SUPPORT_QR_TEX_H);
#endif
    return tex;
}

// Support: a PayPal QR code to donate to the project.
static void tab_support(void) {
    header("Support this project");
    ImGui::TextWrapped("This project is free and open source. If it is useful to you, a donation via PayPal is "
                       "always welcome:");
    ImGui::Spacing();
    ImTextureID tex = support_qr_texture();
    if (tex) {
        float s = 300 * ImGui::GetFontSize() / 34;  // ~1:1 with the 296 px texture at the default scale
        ImGui::Image(tex, ImVec2(s, s));
    }
    ImGui::Spacing();
    hint("Scan the QR code with your phone to open the donation page.");
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

static void notice_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    float w = fminf(io.DisplaySize.x * 0.9f, 1000 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font_big);
    ImGui::TextUnformatted(g_note_title);
    ImGui::PopFont();
    ImGui::PushFont(g_menu_font);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_note_text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static bool was_open;
    if (!was_open) ImGui::SetKeyboardFocusHere(), ImGui::SetNavCursorVisible(true);  // (the controller's cursor on OK)
    was_open = true;
    // OK: clicked, A on it, or any face button / + (only one choice: whichever the player's layout calls A)
    if (ImGui::Button("OK", ImVec2(200 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        g_note = 2;
    ImGui::PopFont();
    ImGui::End();
    if (g_note.load() != 1) was_open = false;
}

// Game thread (runtime/hle/applets.c): the notice over the last frame until the player closes it.
extern "C" void menu_notice_run(const char *title, const char *text) {
    extern void backend_represent(void);
    snprintf(g_note_title, sizeof g_note_title, "%s", title);
    snprintf(g_note_text, sizeof g_note_text, "%s", text);
    input_reset();
    g_note = 1;
    while (g_note.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot closes it)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    g_note = 0;
    input_reset();  // (the button that closed it doesn't reach the game)
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) ODYSSEY_KBD_AUTO=<text>: answer at once
    if (getenv("ODYSSEY_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("ODYSSEY_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: an extra
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[12];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (ids: the pages below; 3-7 were Link's Awakening's and stay unused, so ODYSSEY_MENU_TAB numbers keep meaning)
    static const char *names[] = {"Video", "Audio", "Mods", "Support", NULL, NULL, NULL, NULL, "Controls", "Quit", "Language"};
    static const int order[] = {8, 0, 1, 2, 3, 10, 9};  // Controls, Video, Audio, Mods, Support, Language, Quit
    for (int i : order)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            // (one space each side: with two, nine tabs didn't fit and ImGui cut "Performance" / "Language" short)
            snprintf(lbl, sizeof lbl, " %s ###tab%d", tr(t.name), t.id);
            ImGui::BeginDisabled(!t.on);
            // The tabs are their names (the shown one white, the others dim) with a red line under the shown one.
            // Focus (controller / keyboard): the tab lights up a little and gets a dim line, instead of ImGui's
            // outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, k == cur || lit ? kTextV : kTextDimV);
            int pushed = 2;
            if (lit) {
                for (ImGuiCol c : {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected}) ImGui::PushStyleColor(c, ImVec4(1, 1, 1, 0.07f));
                pushed += 3;
            }
            // (no Prelaunch in game: the cursor starts on the tab that is shown - on Controls, the first, it stayed lit
            // next to the page the menu reopened on: two tabs looked selected)
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = -100;
            // (-100: the shown tab - the one being switched to, when a switch is asked for this frame)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == (g_tab_request >= 0 ? g_tab_request : g_tab));
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(pushed);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (open || lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 3 * scale > 2 ? 3 * scale : 2, m = 10 * scale;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + m, b.y - h), ImVec2(b.x - m, b.y), open ? kAccent : kAccentDim);
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2: tab_mods(scale); break;
                case 3: tab_support(); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale, in_game); break;
                case 10: tab_language(); break;
                }
                static const char *scroll_test = getenv("ODYSSEY_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (another UI may have its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

#ifndef __APPLE__  // (pacing.c: the display's own spacing where the driver reports it; Vulkan only)
extern "C" float vk_display_frame_ms(void);
#endif
// ---- the performance overlay: rows of a coloured label and up to three values, the frame time graph, then the
// hardware's names, the upscaler and resolution, and Mario's position. Each part has its switch (Performance tab).
#define PERF_COLS 18.6f  // the overlay's width in font heights

static void perf_row(const char *label, ImVec4 color, const char *a, const char *b, const char *c) {
    float em = ImGui::GetFontSize(), x0 = ImGui::GetCursorPosX();
    ImGui::TextColored(color, "%s", label);
    if (a) { ImGui::SameLine(x0 + em * 3.3f); ImGui::TextUnformatted(a); }
    if (b) { ImGui::SameLine(x0 + em * 7.5f); ImGui::TextUnformatted(b); }
    if (c) { ImGui::SameLine(x0 + em * 12.1f); ImGui::TextUnformatted(c); }
}

// "43%" / "40 C": the value with its unit, or "-" where this PC doesn't tell
static const char *perf_num(char *buf, size_t n, int v, const char *unit) {
    if (v < 0) return "-";
    snprintf(buf, n, "%d%s", v, unit);
    return buf;
}

static void draw_performance(void) {
    // (the menu's look, theme.h: the labels dim white, the values white)
    const ImVec4 green = kTextDimV, blue = kTextDimV, pink = kTextDimV, white = kTextDimV;
    const ImVec4 dim = kTextDimV;
    char a[48], b[48], c[48];
    float em = ImGui::GetFontSize();
    // frame times of the last 180 presents
    static float times[180];
    static int at, filled;
    float dt = ImGui::GetIO().DeltaTime * 1000.0f;
#ifndef __APPLE__
    float shown = vk_display_frame_ms();  // (the display's own spacing where the driver reports it: pacing.c)
    if (shown > 0) dt = shown;
#endif
    times[at] = dt, at = (at + 1) % 180;
    if (filled < 180) filled++;
    float worst = 0, sum = 0, recent = 0;
    int n_recent = 0;
    for (int i = 0; i < filled; i++) {
        float t = times[(at - 1 - i + 360) % 180];
        if (t > worst) worst = t;
        sum += t;
        if (recent < 500.0f) recent += t, n_recent++;  // (the average over the last half second)
    }
    float ms = n_recent ? recent / n_recent : 0;
#ifndef __APPLE__
    VulkanOverlayStats s = vk_overlay_stats();
    if (g_settings.overlay_gpu) {
        char t[16], w[16];
        perf_row("GPU", green, perf_num(a, sizeof a, s.gpu_usage, "%"), perf_num(t, sizeof t, s.gpu_temp_c, " C"),
                 perf_num(w, sizeof w, s.gpu_power_w, " W"));
        if (s.vram_budget_mib) {
            snprintf(a, sizeof a, "%.2f GB", s.vram_mib / 1024.0);
            snprintf(b, sizeof b, "/ %.1f GB", s.vram_budget_mib / 1024.0);
            perf_row("VRAM", green, a, b, s.gpu_clock_mhz > 0 ? perf_num(c, sizeof c, s.gpu_clock_mhz, " MHz") : NULL);
        }
    }
    if (g_settings.overlay_cpu) {
        char t[16];
        if (s.game_cpu_usage >= 0) snprintf(c, sizeof c, "game %d%%", s.game_cpu_usage);
        perf_row("CPU", blue, perf_num(a, sizeof a, s.cpu_usage, "%"), perf_num(t, sizeof t, s.cpu_temp_c, " C"),
                 s.game_cpu_usage >= 0 ? c : NULL);
    }
    if (g_settings.overlay_ram && s.ram_total_mib) {
        snprintf(a, sizeof a, "%.1f GB", s.ram_used_mib / 1024.0);
        snprintf(b, sizeof b, "/ %.1f GB", s.ram_total_mib / 1024.0);
        snprintf(c, sizeof c, "game %.1f GB", s.game_ram_mib / 1024.0);
        perf_row("RAM", pink, a, b, s.game_ram_mib ? c : NULL);
    }
#endif
    if (ms > 0) {
        snprintf(a, sizeof a, "%.0f", 1000.0f / ms);
        snprintf(b, sizeof b, "%.1f ms", ms);
        snprintf(c, sizeof c, "max %.1f ms", worst);
        perf_row("FPS", white, a, b, c);
    } else perf_row("FPS", white, "-", NULL, NULL);
    if (g_settings.overlay_graph) {
        // the frame times as a line: flat when every frame takes as long, a spike at a hitch
        ImGui::Dummy(ImVec2(0, em * 0.25f));
        ImVec2 p = ImGui::GetCursorScreenPos(), size(em * PERF_COLS, em * 2.2f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme_u32(kSurfaceV), em * 0.3f);
        dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), theme_u32(kLineV), em * 0.3f);
        float top = filled ? sum / filled * 2.0f : 33.4f;
        if (top < worst * 1.1f) top = worst * 1.1f;
        if (top < 1) top = 1;
        ImVec2 pts[180];
        float pad = em * 0.6f;
        for (int i = 0; i < filled; i++) {
            float t = times[(at - filled + i + 360) % 180];
            pts[i] = ImVec2(p.x + pad + (size.x - 2 * pad) * (filled > 1 ? (float)i / (filled - 1) : 0),
                            p.y + size.y - pad * 0.5f - (size.y - pad) * (t / top));
        }
        if (filled > 1) dl->AddPolyline(pts, filled, IM_COL32(235, 235, 235, 230), 0, em * 0.07f < 1 ? 1 : em * 0.07f);
        ImGui::Dummy(size);
        ImGui::Dummy(ImVec2(0, em * 0.15f));
    } else ImGui::Dummy(ImVec2(em * PERF_COLS, 0));  // (the overlay keeps its width)
#ifndef __APPLE__
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * PERF_COLS);
    if (g_settings.overlay_hardware) {
        if (s.gpu_name && *s.gpu_name) ImGui::TextColored(dim, "%s", s.gpu_name);
        if (s.cpu_name && *s.cpu_name) ImGui::TextColored(dim, "%s", s.cpu_name);
    }
    if (g_settings.overlay_render) {
        const char *up = g_settings.res_mode == RES_MATCH ? "Off" : g_settings.upscaler == UPSCALE_FSR1 ? "FSR 1" : "Bilinear";
        const char *aa = g_settings.antialias == AA_SMAA ? "SMAA" : "Off";
        char name[48];
        if (vk_temporal_enabled()) {
            static const char *const modes[DLSS_MODE_COUNT] = {"", "Native AA", "Quality", "Balanced", "Performance"};
            int backend = g_settings.temporal_backend, mode = *settings_temporal_mode();
            const char *tech = "DLSS";
            if (backend == TEMPORAL_XESS) tech = "XeSS";
#ifdef _WIN32
            else if (backend == TEMPORAL_FSR4) tech = vk_fsr4_available() ? "FSR 4" : "FSR 3.1";
#endif
            else if (backend == TEMPORAL_FSR3) tech = "FSR 3.1";
            else if (backend == TEMPORAL_FSR2) tech = "FSR 2";
            if (mode < 0 || mode >= DLSS_MODE_COUNT) mode = 0;
            snprintf(name, sizeof name, "%s %s", tech, backend == TEMPORAL_DLSS && mode == DLSS_DLAA ? "DLAA" : modes[mode]);
            up = name, aa = "Temporal";
        }
        ImGui::TextColored(dim, "Upscaler: %s | Anti-aliasing: %s", up, aa);
        if (s.render_w && s.out_w) ImGui::TextColored(dim, "%u x %u -> %u x %u", s.render_w, s.render_h, s.out_w, s.out_h);
    }
    ImGui::PopTextWrapPos();
    float pos[3];
    if (g_settings.overlay_position && odyssey_player_position(pos))  // (in metres: the game's units are centimetres)
        ImGui::Text("X %.1f  Y %.1f  Z %.1f", pos[0] / 100.0f, pos[1] / 100.0f, pos[2] / 100.0f);
#endif
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load(), setup = g_setup.load();
    bool kbd = g_kbd.load() == 1, note = g_note.load() == 1;
    if (!open && !kbd && !note && !setup && !menu_stats_visible()) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    extern bool setup_config_shown(void);  // (the configuration page: the menu's tabs, a little larger than in play)
    float scale = (float)(th / 1080.0) * (setup && setup_config_shown() ? 0.8f : 1.0f);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        // (ScaleAllSizes rounds the pointer's scale down to a whole number: 0 below a 1.5x scale, so on a 1080p or
        // 1440p screen the menu's pointer was drawn at size 0 - #5, #11. Its size follows the screen, never below 1.)
        ImGui::GetStyle().MouseCursorScale = fmaxf(1.0f, scale / 0.68f);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (setup) {
        setup_draw(scale, g_menu_font, g_menu_font_big);
        open = false;
    } else if (menu_stats_visible() && (!open || !g_settings.performance_overlay)) {
        bool advanced = g_settings.performance_overlay;
        float margin = 16 * scale;
        // the overlay's corner, size and background are the player's choice; the plain FPS counter stays top right
        int corner = advanced ? g_settings.overlay_corner : OVERLAY_TOP_RIGHT;
        bool right = corner == OVERLAY_TOP_RIGHT || corner == OVERLAY_BOTTOM_RIGHT;
        bool bottom = corner == OVERLAY_BOTTOM_RIGHT || corner == OVERLAY_BOTTOM_LEFT;
        static const float sizes[3] = {0.64f, 0.80f, 1.0f};
        ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - margin : margin, bottom ? io.DisplaySize.y - margin : margin),
                                ImGuiCond_Always, ImVec2(right ? 1 : 0, bottom ? 1 : 0));
        ImGui::SetNextWindowBgAlpha(advanced ? g_settings.overlay_opacity * 0.01f : 0.5f);
        if (advanced) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 3 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1 * scale);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));  // (black, as the menu: see-through by the opacity)
            ImGui::PushStyleColor(ImGuiCol_Border, kLineV);
        }
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushFont(g_menu_font);  // (the menu's font: Roboto Medium)
        ImGui::SetWindowFontScale(advanced ? sizes[g_settings.overlay_size >= 0 && g_settings.overlay_size <= 2 ? g_settings.overlay_size : 1] : 1.0f);
        if (advanced) draw_performance();
        else ImGui::Text("%.0f fps", io.Framerate);
        ImGui::PopFont();
        ImGui::End();
        if (advanced) { ImGui::PopStyleColor(2); ImGui::PopStyleVar(4); }
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header: the crescent and the name, a close X in the corner (B / Esc too), a hairline under them
        ImGui::PushFont(g_font_big);
        {
            float lh = ImGui::GetFontSize(), sz = lh * 1.35f;
            ImVec2 lp = ImGui::GetCursorScreenPos();
            // (its disc spans the middle 2/3 of the square: the disc's left edge on the text's, centred on the line)
            theme_crescent(ImGui::GetWindowDrawList(), ImVec2(lp.x - sz / 6, lp.y + lh * 0.5f - sz * 0.5f), sz, kAccent,
                           theme_u32(kPanelV, 1));
            ImGui::Dummy(ImVec2(sz * 2 / 3, lh));
            ImGui::SameLine(0, lh * 0.4f);
            ImGui::TextUnformatted(RT_APP_NAME);
        }
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? kAccent : theme_u32(kTextV, 0.85f);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), theme_u32(kSurfaceHoverV), 4 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }
        ImGui::Separator();

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (note && !open) notice_draw(io, scale);  // (the menu opened over it: the notice waits behind it)
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
    g_drawn = true;
}
