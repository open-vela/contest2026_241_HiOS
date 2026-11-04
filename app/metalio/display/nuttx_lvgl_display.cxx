#include "hi_task_policy.h"
#include "sms_screen/sms_screen.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttxLvglDisplay implementation — see header for design notes.
 */

#include "nuttx_lvgl_display.h"
#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "boot_screen/boot_screen.h"
#include "panel_screen/panel_screen.h"
#include "home_screen/home_screen.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "chat_screen/chat_screen.h"
#include "call_screen/call_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "idle_power_policy/idle_power_policy.h"
#include "digital_people_screen/digital_people_screen.h"
#include "system_chrome/system_chrome.h"
#include "esp_log_shim.h"
#include "settings.h"
#include "esp_timer_shim.h"
#include "input_method/hi_input_method.h"
#include "screen/clock_screen/clock_screen.h"
#include "screen/digital_people_screen/xiaobai_resources.h"

#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "src/core/lv_global.h"
#include <time.h>
#if LV_USE_OS != LV_OS_PTHREAD
#error "The NuttX display and screen workers require LVGL pthread locking"
#endif
#include <atomic>
#include <cstdlib>
#include <cstring>

/* The NuttX LVGL framebuffer / touchscreen drivers are part of the LVGL
 * library (compiled by apps/graphics/lvgl).  We declare the prototypes
 * here to avoid depending on internal LVGL header paths. */
extern "C" {
lv_display_t * lv_nuttx_fbdev_create(void);
int lv_nuttx_fbdev_set_file(lv_display_t * disp, const char * file);
lv_indev_t * lv_nuttx_touchscreen_create(const char * dev_path);
void lvgl_assets_fs_init(void);
void metalio_fonts_init(void);
void metalio_boot_pwr_vibrate_once(void);
}

#include <cstring>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <nuttx/config.h>
#include <nuttx/video/fb.h>

#define TAG "NuttxDisplay"

namespace {
lv_obj_t *s_boot_screen_waiting_for_first_frame = nullptr;

void on_boot_display_refresh_ready(lv_event_t * /*event*/)
{
    lv_obj_t *boot_scr = s_boot_screen_waiting_for_first_frame;
    if (boot_scr == nullptr)
        return;
    if (lv_screen_active() == boot_scr)
        return;

    /* DSI scanout is asynchronous. Keep the boot frame alive until LVGL has
     * rendered and flushed the first destination frame, avoiding an unpainted frame
     * during the transition. */
    s_boot_screen_waiting_for_first_frame = nullptr;
    write(1, "PANEL_FIRST_FRAME\n", 18);
    lv_obj_delete_async(boot_scr);
}
}  // namespace

/* ------------------------------------------------------------------ */
/* LVGL tick source                                                   */
/*                                                                    */
/* LVGL timers (lv_timer_create / lv_timer_handler) depend on         */
/* lv_tick_get() advancing.  Nothing on NuttX calls lv_tick_inc()     */
/* from an ISR, so without a custom tick callback the 2s boot→home     */
/* transition timer never fires.  Wire lv_tick_get() to a real        */
/* millisecond clock.                                                  */
/* ------------------------------------------------------------------ */
static uint32_t lvgl_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

NuttxLvglDisplay::NuttxLvglDisplay()
{
    /* 1. LVGL core init (idempotent) */
    lv_init();
    write(1, "DD1\n", 4);

    /* Provide a real tick source so LVGL timers can fire. */
    lv_tick_set_cb(lvgl_monotonic_ms);

    /* 34 launcher icons need about 2.2 MiB decoded. Keep them and the
     * current app's assets across page changes; the 1 MiB default evicted
     * icons during a two-page swipe and repeatedly decoded PNGs. This is
     * an upper bound, allocated lazily by the image cache. */
    lv_image_cache_resize(4 * 1024 * 1024, false);
    lv_image_header_cache_resize(192, false);

    /* Load PuHui CBin fonts into font_puhui_* symbols before any screen
     * (boot/home) creates labels that reference them. */
    metalio_fonts_init();

    /* Register the read-only "A:" filesystem that serves the embedded
     * MetalioClaw4 icon PNGs.  Must be registered before any screen
     * references "A:ic_*.spng" so the LODEPNG decoder can resolve them. */
    lvgl_assets_fs_init();
    write(1, "DD2\n", 4);

    /* 2. Create the display via the NuttX framebuffer driver (/dev/fb0). */
    display_ = lv_nuttx_fbdev_create();
    if (display_ == nullptr)
    {
        ESP_LOGE(TAG, "lv_nuttx_fbdev_create() returned NULL");
        return;
    }
    write(1, "DD3\n", 4);

    if (lv_nuttx_fbdev_set_file(display_, "/dev/fb0") != 0)
    {
        ESP_LOGE(TAG, "lv_nuttx_fbdev_set_file(\"/dev/fb0\") failed — "
                      "display will be inactive");
        return;
    }
    lv_display_add_event_cb(display_, on_boot_display_refresh_ready,
                            LV_EVENT_REFR_READY, nullptr);
    /* Keep DIRECT (set by lv_nuttx_fbdev). FULL redraw on every home
     * pager scroll_by was too heavy and made swipe flash worse; stale
     * tiles are fixed by full-page FBIO_UPDATE before FBIOPAN. */
    write(1, "DD4\n", 4);

    /* The LVGL default theme is initialized in DARK mode inside
     * lv_nuttx_fbdev_create() -> lv_display_create(), controlled by
     * CONFIG_LV_THEME_DEFAULT_DARK=y. With the light theme, lv_display_create
     * auto-applies blue primary buttons + white cards + light grey screens to
     * any widget without an explicit background, which is the "blue screen"
     * seen on incompletely-styled screens. Dark mode makes those fall back to
     * dark grey instead of blue/white, matching the MetalioClaw4 UI.
     *
     * Do NOT call lv_theme_default_init() here: re-initializing the theme
     * after lv_display_create() re-applies styles to the already-created
     * default screen/layers and hangs right after DD5 (in
     * lv_nuttx_touchscreen_create). The config flag is the correct fix. */
    write(1, "DD5\n", 4);

    /* Default screen is the theme primary (blue) until a real screen
     * covers it. Paint it black so a gap between loads cannot flash blue. */
    if (lv_obj_t *base = lv_screen_active())
    {
        lv_obj_set_style_bg_color(base, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (lv_obj_t *bottom = lv_layer_bottom())
    {
        lv_obj_set_style_bg_color(bottom, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, LV_PART_MAIN);
    }

    /* 3. Touchscreen input device */
    lv_nuttx_touchscreen_create("/dev/input0");
    write(1, "DD6\n", 4);

    /* 4. Read display resolution */
    int32_t w = lv_display_get_horizontal_resolution(display_);
    int32_t h = lv_display_get_vertical_resolution(display_);
    width_  = (w > 0) ? w : 720;
    height_ = (h > 0) ? h : 720;

    /* LVGL event-loop thread is deferred to StartLvglThread(),
     * called from Application::Start() after board init completes. */
    lvgl_running_.store(false);
    write(1, "DD7\n", 4);

    /* 6. Build the initial UI (boot screen → home screen).  This must
     *    happen AFTER the event-loop thread starts so that lv_timer
     *    callbacks (used for the boot→home transition) are serviced.  */
    write(1, "DD7a\n", 5);
    bool locked = Lock(30000);
    write(1, "DD7b\n", 5);
    if (!locked)
    {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for SetupUI");
        return;
    }
    SetupUI();
    Unlock();
    write(1, "DD8\n", 4);
    SettingsPrimeFlash();
    StartLvglThread();
    write(1, "DD9\n", 4);
}

void NuttxLvglDisplay::StartLvglThread()
{
    if (lvgl_thread_created_)
        return;
    lvgl_running_.store(true);

    /* The LVGL render thread needs a large stack: 720x720 RGB565 rendering
     * (lv_timer_handler -> lv_display_refr_timer -> layout/draw) far exceeds
     * the NuttX default pthread stack (CONFIG_PTHREAD_STACK_DEFAULT=2048).
     * A 2KB stack overflows and corrupts adjacent heap, which showed up as
     * random hangs in the render path.  Give it the same 128KB the app task
     * uses (CONFIG_METALIO_APP_STACKSIZE). */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 131072);

    /* SCHED_FIFO above the app task keeps touch alive while Activate()/TLS
     * polls on the app (or OTA worker).  HPWORK is 224; stay below that. */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = CONFIG_METALIO_APP_PRIORITY + 10;
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    int rc = hi_task_create_legacy(&lvgl_thread_, &attr, LvglEventLoopEntry, this);
    if (rc != 0)
    {
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = hi_task_create_legacy(&lvgl_thread_, &attr, LvglEventLoopEntry, this);
    }
    pthread_attr_destroy(&attr);
    if (rc == 0)
    {
        lvgl_thread_created_ = true;
        ESP_LOGI(TAG, "LVGL event loop thread started");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create LVGL event loop thread");
        lvgl_running_.store(false);
    }
}

NuttxLvglDisplay::~NuttxLvglDisplay()
{
    lvgl_running_.store(false);
    if (lvgl_thread_created_)
        pthread_join(lvgl_thread_, nullptr);
    lvgl_thread_created_ = false;
}

/* ------------------------------------------------------------------ */
/* SetupUI — boot screen then transition to the smart-home panel        */
/*                                                                    */
/* Mirrors LVAdapterDisplay::SetupUI() from the original MetalioClaw4 */
/* code: a boot screen is shown first, then after 2 seconds a one-shot */
/* LVGL timer replaces it with the smart-home panel.                   */
/* ------------------------------------------------------------------ */

namespace {
std::atomic<bool> s_home_transition_armed{false};
std::atomic<bool> s_home_transition_done{false};
std::atomic<bool> s_boot_min_time_reached{false};
uint32_t s_boot_start_tick = 0;

constexpr uint32_t kBootGuideMinMs = 3000;
constexpr uint32_t kBootForceHomeMs = 8000;

void DoBootToPanelTransition()
{
    if (s_home_transition_done.load())
        return;

    write(1, "PANEL_GO\n", 9);
    /* Boot→panel must NOT use SwitchToHome(): that path deletes the active
     * screen (boot + LVGL default layer) before CreateStatic, and the first
     * refresh then shows the default-theme primary blue. Load panel on top
     * of boot, then drop boot after home is the active screen. */
    lv_obj_t *boot_scr = lv_screen_active();
    PwrKey_OnScreenLifecycle("boot", SCREEN_LIFECYCLE_UNLOAD);
    lv_obj_t *panel_scr = PanelScreen::CreateStatic();
    if (panel_scr == nullptr)
        return;
    lv_screen_load(panel_scr);
    if (boot_scr != nullptr && boot_scr != panel_scr)
        s_boot_screen_waiting_for_first_frame = boot_scr;
    /* Match the safe launcher/panel transitions: another overlay may have
     * paused invalidation before boot completed. Re-enable it and invalidate
     * the full opaque destination so the first scanout cannot keep a stale
     * framebuffer frame or expose the theme's blue fallback. */
    if (lv_display_t *disp = lv_display_get_default())
    {
        lv_display_enable_invalidation(disp, true);
        lv_timer_t *refr_timer = lv_display_get_refr_timer(disp);
        if (refr_timer != nullptr)
        {
            lv_timer_resume(refr_timer);
            lv_timer_ready(refr_timer);
        }
    }
    lv_obj_invalidate(panel_scr);
    /* The normal LVGL refresh loop paints the destination after this timer
     * callback returns. Keep the boot screen alive until that refresh is
     * complete so no empty/default-theme frame is exposed. */
    s_home_transition_done.store(true);
    BluetoothScreen::InitAudioObserver();
    HiInputMethod_InitGlobal();
    ClockScreen::InitGlobal();
    SmsScreen::InitGlobal();
    IdlePower_Attach(IdlePowerSession::App, true);
    write(1, "PANEL_OK\n", 9);
#ifdef HI_STABILITY_PROBE
    lv_timer_create([](lv_timer_t *timer) {
        HomeScreen::OpenGrid();
        lv_timer_delete(timer);
    }, 5000, nullptr);
#endif
    XiaobaiResources::EnsureAsync();
    // Phone initialization also initializes SMS and reads persistent SD
    // stores. Let resource provisioning establish its writable mount first,
    // then start the global call observer on this same LVGL thread.
    // The panel and audio tasks remain live throughout provisioning.
    lv_timer_create([](lv_timer_t *timer) {
        const int status = XiaobaiResources::Status();
        if (status == 0 || status == 1) return;
        CallScreen::InitGlobal();
        lv_timer_delete(timer);
    }, 50, nullptr);
}

void TryBootToHomeTransition()
{
    if (s_home_transition_done.load())
        return;
    if (!s_boot_min_time_reached.load())
        return;
    const uint32_t elapsed =
        s_boot_start_tick != 0 ? lv_tick_elaps(s_boot_start_tick) : 0;
    const bool force_home =
        elapsed >= kBootForceHomeMs && s_boot_min_time_reached.load();
    if (!s_home_transition_armed.load() && !force_home)
        return;
    DoBootToPanelTransition();
}

void OnBootMinTimeTimer(lv_timer_t *t)
{
    s_boot_min_time_reached.store(true);
    if (!s_home_transition_armed.load())
        s_home_transition_armed.store(true);
    lv_timer_del(t);
}
}  // namespace

bool NuttxLvglDisplay::IsHomeReady() const
{
    return s_home_transition_done.load();
}

void NuttxLvglDisplay::ArmHomeTransition()
{
    if (s_home_transition_done.load())
        return;

    write(1, "HOME_ARM\n", 9);
    s_home_transition_armed.store(true);
    /* The LVGL event loop performs the transition on its next pass. Do not
     * block here — the old wait made boot feel stuck after the guide bar. */
}

void NuttxLvglDisplay::SetupUI()
{
    write(1, "SUI0\n", 5);
    ESP_LOGI(TAG, "SetupUI — creating boot screen");

    lv_obj_t *boot_scr = BootScreen::CreateStatic();
    write(1, "SUI1\n", 5);
    if (boot_scr != nullptr)
    {
        lv_screen_load(boot_scr);
        PwrKey_OnScreenLifecycle("boot", SCREEN_LIFECYCLE_LOAD);
        /* Pixels first, then backlight, then the buzz. The motor shares
         * the LEDC clock with the backlight, so this order is the one
         * that shows the guide for the whole vibration. */
        lv_refr_now(nullptr);
        if (Backlight *bl = Board::GetInstance().GetBacklight())
            bl->RestoreBrightness();
        metalio_boot_pwr_vibrate_once();
    }
    write(1, "SUI2\n", 5);

    /* 3s boot guide minimum (matches BootScreen progress bar), then switch
     * as soon as Application arms the transition after network bring-up. */
    s_home_transition_armed.store(false);
    s_home_transition_done.store(false);
    s_boot_min_time_reached.store(false);
    s_boot_start_tick = lv_tick_get();

    lv_timer_t *min_timer = lv_timer_create(OnBootMinTimeTimer, kBootGuideMinMs, nullptr);
    if (min_timer != nullptr)
        lv_timer_set_repeat_count(min_timer, 1);
}

/* ------------------------------------------------------------------ */
/* LVGL event loop entry — static trampoline for pthread_create       */
/* ------------------------------------------------------------------ */

void *NuttxLvglDisplay::LvglEventLoopEntry(void *arg)
{
    static_cast<NuttxLvglDisplay *>(arg)->LvglEventLoop();
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Lock / Unlock — std::mutex backed, with optional timeout           */
/* ------------------------------------------------------------------ */

bool NuttxLvglDisplay::Lock(int timeout_ms)
{
    /* lv_lock()/lv_unlock() are the single recursive mutex that serialises
     * ALL LVGL access: the render thread (lv_timer_handler self-locks),
     * Application-thread DisplayLockGuard holders, and the per-screen worker
     * threads (whose LvglLock()/LvglUnlock() map to lv_lock()/lv_unlock()).
     * Using the same lock here instead of a separate std::mutex prevents the
     * worker threads from racing the Application thread on LVGL object/heap
     * state — the corruption that showed up as blue/garbled app screens. */
    if (timeout_ms <= 0) { lv_lock(); return true; }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_mutex_timedlock(&LV_GLOBAL_DEFAULT()->lv_general_mutex,
                                  &deadline) == 0;
}

void NuttxLvglDisplay::Unlock()
{
    lv_unlock();
}

/* ------------------------------------------------------------------ */
/* Xiaozhi chat / emotion routing (from LVAdapterDisplay)             */
/* ------------------------------------------------------------------ */

namespace {

struct EmoteCategoryEntry {
    const char *emote;
    const char *category;
};

constexpr EmoteCategoryEntry kEmoteCategoryMap[] = {
    {"happy", "happy"},       {"laughing", "laughing"},
    {"funny", "happy"},       {"silly", "happy"},
    {"winking", "happy"},     {"cool", "happy"},
    {"confident", "happy"},   {"loving", "loving"},
    {"kissy", "loving"},      {"delicious", "loving"},
    {"sad", "crying"},        {"crying", "crying"},
    {"angry", "angry"},      {"surprised", "surprised"},
    {"shocked", "surprised"}, {"embarrassed", "surprised"},
    {"thinking", "thinking"}, {"confused", "thinking"},
    {"neutral", "neutral"},   {"relaxed", "neutral"},
    {"sleepy", "thinking"},
};

const char *GetEmoteCategory(const char *emote)
{
    if (emote == nullptr)
        return "neutral";
    for (const auto &e : kEmoteCategoryMap)
    {
        if (std::strcmp(e.emote, emote) == 0)
            return e.category;
    }
    return "neutral";
}

} /* namespace */

void NuttxLvglDisplay::SetStatus(const char *status)
{
    (void)status;
}

void NuttxLvglDisplay::ShowNotification(const char *notification,
                                        int duration_ms)
{
    (void)notification;
    (void)duration_ms;
}

void NuttxLvglDisplay::UpdateStatusBar(bool update_all)
{
    (void)update_all;
}

namespace {

/* STT/TTS bubbles: marshal onto the LVGL thread. Calling AddMessage under
 * DisplayLockGuard from Application/MQTT races the render thread and has
 * left solid blue frames on this port. */

struct PendingChatMsg
{
    char role[16];
    char content[1]; /* flexible tail */
};

/* Drop invalid / truncated UTF-8 so LVGL does not draw □ tofu boxes.
 * Also reject overlong forms and UTF-16 surrogates. */
void SanitizeUtf8InPlace(char *s)
{
    if (s == nullptr)
        return;
    unsigned char *p = reinterpret_cast<unsigned char *>(s);
    unsigned char *w = p;
    while (*p)
    {
        if (*p < 0x80)
        {
            *w++ = *p++;
            continue;
        }
        int need = 0;
        uint32_t cp = 0;
        if ((*p & 0xE0) == 0xC0)
        {
            need = 1;
            cp = *p & 0x1F;
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            need = 2;
            cp = *p & 0x0F;
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            need = 3;
            cp = *p & 0x07;
        }
        else
        {
            ++p;
            continue;
        }
        bool ok = true;
        for (int i = 1; i <= need; ++i)
        {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        /* Overlong / surrogate / non-character → skip lead byte. */
        if (ok)
        {
            if (need == 1 && cp < 0x80)
                ok = false;
            else if (need == 2 && cp < 0x800)
                ok = false;
            else if (need == 3 && cp < 0x10000)
                ok = false;
            else if (cp >= 0xD800 && cp <= 0xDFFF)
                ok = false;
            else if (cp > 0x10FFFF || cp == 0xFFFE || cp == 0xFFFF)
                ok = false;
        }
        if (!ok)
        {
            ++p;
            continue;
        }
        for (int i = 0; i <= need; ++i)
            *w++ = *p++;
    }
    *w = '\0';
}

void OnSetChatMessageAsync(void *user_data)
{
    auto *msg = static_cast<PendingChatMsg *>(user_data);
    if (msg == nullptr)
        return;

    SanitizeUtf8InPlace(msg->content);
    if (msg->content[0] == '\0')
    {
        free(msg);
        return;
    }

    write(1, "UI_MSG\n", 7);
    const bool is_user = (std::strcmp(msg->role, "user") == 0);
    const bool chat_active = ChatScreen::IsForeground();
    const bool dp_active = DigitalPeopleScreen::IsForeground();

    if (chat_active)
    {
        ChatScreen::AddMessage(msg->content,
                               is_user ? ChatMsgDir::Right : ChatMsgDir::Left);
    }
    else if (dp_active)
    {
        if (is_user)
            DigitalPeopleScreen::ShowUserMessage(msg->content);
        else
            DigitalPeopleScreen::ShowSystemMessage(msg->content);
    }
    if (SystemChrome_FloatAiOpen())
        SystemChrome_FloatAiAppend(msg->role, msg->content);

    free(msg);
}

} /* namespace */

void NuttxLvglDisplay::SetEmotion(const char *emotion)
{
    const char *src = (emotion != nullptr && emotion[0] != '\0')
                          ? emotion
                          : "neutral";

    /* HARD RULE: while chat is foreground, never take DisplayLockGuard or
     * touch DigitalPeople widgets — that path paints solid blue on wake.
     * ChatScreen::SetEmotion only caches the name (LVGL apply is async). */
    if (ChatScreen::IsForeground())
    {
        ChatScreen::SetEmotion(src);
        return;
    }

    // Thread-safe cache only; the owning UI timer applies the image.
    DigitalPeopleScreen::SetEmotion(GetEmoteCategory(src));
}

void NuttxLvglDisplay::SetChatMessage(const char *role, const char *content)
{
    if (role == nullptr || content == nullptr || content[0] == '\0')
        return;

    /* Do not gate on IsWakeConnectInflight(): that flag stayed true across
     * OpenAudioChannel and swallowed STT/TTS bubbles (TTS_TXT with no UI_MSG).
     * MQTT JSON is already dropped via JSON_SKIP while the channel opens. */

    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_bot = (std::strcmp(role, "assistant") == 0 ||
                         std::strcmp(role, "system") == 0);
    if (!is_user && !is_bot)
        return;

    if (!ChatScreen::IsActive() && !DigitalPeopleScreen::IsActive() &&
        !SystemChrome_FloatAiOpen())
        return;

    const size_t content_len = std::strlen(content);
    const size_t bytes = sizeof(PendingChatMsg) + content_len; /* +1 in flex */
    auto *msg = static_cast<PendingChatMsg *>(malloc(bytes));
    if (msg == nullptr)
        return;
    std::memset(msg, 0, bytes);
    std::strncpy(msg->role, role, sizeof(msg->role) - 1);
    std::memcpy(msg->content, content, content_len + 1);

    if (lv_async_call(OnSetChatMessageAsync, msg) != LV_RESULT_OK)
    {
        free(msg);
        write(1, "UI_MSG_FAIL\n", 12);
    }
}

/* ------------------------------------------------------------------ */
/* LVGL event loop — runs in a dedicated thread                       */
/*                                                                    */
/* With CONFIG_LV_OS_PTHREAD, lv_lock()/lv_unlock() are a real        */
/* recursive mutex and lv_timer_handler() self-locks.  Lock()/Unlock()*/
/* here delegate to the same mutex, so this thread, Application-thread */
/* DisplayLockGuard holders, and per-screen worker threads all         */
/* serialise on one LVGL lock.                                        */
/* ------------------------------------------------------------------ */

void NuttxLvglDisplay::LvglEventLoop()
{
    SettingsBindUiThread();
    ESP_LOGI(TAG, "LVGL event loop entered");
    bool panel_return_probe_written = false;
    bool panel_begin_probe_written = false;

    while (lvgl_running_.load())
    {
        Lock();
        /* Change screens before entering LVGL's timer-list traversal. The
         * page-loaded callback creates its own timers; doing that inside a
         * timer callback can leave the iterator spinning before the first
         * panel frame is rendered. */
        TryBootToHomeTransition();
        if (!panel_begin_probe_written && s_home_transition_done.load())
        {
            panel_begin_probe_written = true;
            write(1, "PANEL_HANDLER_BEGIN\n", 20);
        }
        uint32_t idle = lv_timer_handler();
        if (!panel_return_probe_written && s_home_transition_done.load())
        {
            panel_return_probe_written = true;
            write(1, "PANEL_TIMER_RETURNED\n", 21);
        }
        Unlock();

        /* lv_timer_handler returns the suggested sleep time in ms.
         * Sleep with nanosleep() (the working NuttX sleep primitive used
         * by vTaskDelayMs) so the main Application event loop also gets
         * CPU time.  A busy-wait here would starve it. */
        if (idle == 0)
            idle = 1;
        /* Clamp to avoid a pathological LV_NO_TIMER_READY (0xFFFFFFFF) or a
         * mis-computed huge delay from blocking the loop forever. */
        if (idle > 100)
            idle = 100;
        struct timespec ts;
        ts.tv_sec = idle / 1000;
        ts.tv_nsec = (idle % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }

    ESP_LOGI(TAG, "LVGL event loop exited");
}
