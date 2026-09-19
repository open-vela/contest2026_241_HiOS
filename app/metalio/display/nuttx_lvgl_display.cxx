/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttxLvglDisplay implementation — see header for design notes.
 */

#include "nuttx_lvgl_display.h"
#include "boot_screen/boot_screen.h"
#include "home_screen/home_screen.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "chat_screen/chat_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"

#include <lvgl.h>
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

    int rc = pthread_create(&lvgl_thread_, &attr, LvglEventLoopEntry, this);
    if (rc != 0)
    {
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(&lvgl_thread_, &attr, LvglEventLoopEntry, this);
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
/* SetupUI — boot screen then transition to HomeScreen                */
/*                                                                    */
/* Mirrors LVAdapterDisplay::SetupUI() from the original MetalioClaw4 */
/* code: a boot screen is shown first, then after 2 seconds a one-shot */
/* LVGL timer replaces it with the HomeScreen.                         */
/* ------------------------------------------------------------------ */

namespace {
std::atomic<bool> s_home_transition_armed{false};
std::atomic<bool> s_home_transition_done{false};
std::atomic<bool> s_boot_min_time_reached{false};
lv_timer_t *s_home_poll_timer = nullptr;

constexpr uint32_t kBootGuideMinMs = 5000;

void DoBootToHomeTransition()
{
    if (s_home_transition_done.load())
        return;

    write(1, "HOME_GO\n", 8);
    /* Boot→home must NOT use SwitchToHome(): that path deletes the active
     * screen (boot + LVGL default layer) before CreateStatic, and the first
     * refresh then shows the default-theme primary blue. Load home on top
     * of boot, then drop boot after home is the active screen. */
    lv_obj_t *boot_scr = lv_screen_active();
    PwrKey_OnScreenLifecycle("boot", SCREEN_LIFECYCLE_UNLOAD);
    lv_obj_t *home_scr = HomeScreen::CreateStatic();
    if (home_scr == nullptr)
        return;
    lv_screen_load(home_scr);
    if (boot_scr != nullptr && boot_scr != home_scr)
        lv_obj_delete(boot_scr);
    s_home_transition_done.store(true);
    write(1, "HOME_OK\n", 8);
}

void TryBootToHomeTransition()
{
    if (s_home_transition_done.load())
        return;
    if (!s_boot_min_time_reached.load())
        return;
    if (!s_home_transition_armed.load())
        return;
    DoBootToHomeTransition();
    if (s_home_transition_done.load() && s_home_poll_timer != nullptr)
    {
        lv_timer_del(s_home_poll_timer);
        s_home_poll_timer = nullptr;
    }
}

void OnBootMinTimeTimer(lv_timer_t *t)
{
    s_boot_min_time_reached.store(true);
    lv_timer_del(t);
    TryBootToHomeTransition();
}

void OnHomeTransitionPollTimer(lv_timer_t * /*t*/)
{
    TryBootToHomeTransition();
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
    /* HomeScreen::CreateStatic must run on the LVGL thread (poll timer).
     * Do not block here — the old 10s wait made boot feel stuck after the
     * 5s guide bar finished. */
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
    }
    write(1, "SUI2\n", 5);

    /* 5s boot guide minimum (matches BootScreen progress bar), then switch
     * as soon as Application arms the transition after network bring-up. */
    s_home_transition_armed.store(false);
    s_home_transition_done.store(false);
    s_boot_min_time_reached.store(false);
    s_home_poll_timer = nullptr;

    lv_timer_t *min_timer = lv_timer_create(OnBootMinTimeTimer, kBootGuideMinMs, nullptr);
    if (min_timer != nullptr)
        lv_timer_set_repeat_count(min_timer, 1);

    s_home_poll_timer = lv_timer_create(OnHomeTransitionPollTimer, 50, nullptr);
    if (s_home_poll_timer == nullptr)
        ESP_LOGE(TAG, "lv_timer_create failed for boot→home poll");
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
    (void)timeout_ms;

    /* lv_lock()/lv_unlock() are the single recursive mutex that serialises
     * ALL LVGL access: the render thread (lv_timer_handler self-locks),
     * Application-thread DisplayLockGuard holders, and the per-screen worker
     * threads (whose LvglLock()/LvglUnlock() map to lv_lock()/lv_unlock()).
     * Using the same lock here instead of a separate std::mutex prevents the
     * worker threads from racing the Application thread on LVGL object/heap
     * state — the corruption that showed up as blue/garbled app screens. */
    lv_lock();
    return true;
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
    {"happy", "happy"},       {"laughing", "happy"},
    {"funny", "happy"},       {"silly", "happy"},
    {"winking", "happy"},     {"cool", "happy"},
    {"confident", "happy"},   {"loving", "loving"},
    {"kissy", "loving"},      {"delicious", "loving"},
    {"sad", "crying"},        {"crying", "crying"},
    {"angry", "crying"},      {"surprised", "surprised"},
    {"shocked", "surprised"}, {"embarrassed", "surprised"},
    {"thinking", "thinking"}, {"confused", "thinking"},
    {"neutral", "neutral"},   {"relaxed", "neutral"},
    {"sleepy", "neutral"},
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
    const bool chat_active = ChatScreen::IsActive();
    const bool dp_active = DigitalPeopleScreen::IsActive();

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
    if (ChatScreen::IsActive())
    {
        ChatScreen::SetEmotion(src);
        return;
    }

    /* Cache emotion for the next DP Create(). Do not call SetEmotion while
     * DP is active: SetEmotionSrc from the app/MQTT thread races LVGL. */
    if (!DigitalPeopleScreen::IsActive())
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

    if (!ChatScreen::IsActive() && !DigitalPeopleScreen::IsActive())
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
    ESP_LOGI(TAG, "LVGL event loop entered");

    while (lvgl_running_.load())
    {
        Lock();
        uint32_t idle = lv_timer_handler();
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
