#include "hi_task_policy.h"
#include "display_text.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SdCardScreen — ported from MetalioClaw4
 * main/display/screen/sd_card_screen/sd_card_screen.cc.
 *
 * Differences from the reference:
 *   - ESP-IDF esp_log/sdmmc_cmd/ff.h headers dropped; uses esp_log_shim.
 *   - SdCardManager and UsbVirtualDisk come from apps/metalio/port/ stubs.
 *     SdCardManager::GetCard() always returns nullptr on openvela, so the
 *     FatFs free-cluster capacity path is skipped and the screen shows
 *     "未支持" in the capacity line when card info is unavailable.
 *   - All file operations use POSIX (opendir/readdir/stat/unlink/fopen)
 *     which NuttX provides natively; no other behavioural changes.
 *   - HomeScreen::Create() -> HomeScreen::CreateStatic() (matches the
 *     openvela port convention used by the other screens).
 *   - lv_style_selector_t cast helper Sel() kept to silence
 *     -Wdeprecated-enum-enum-conversion (same as the reference).
 */

#include "sd_card_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"
#include "hi_music_screen/hi_music_screen.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/unistd.h>
#include <unistd.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char *TAG_SD = "SdCardScreen";
constexpr int kPanelSize = 720;
constexpr int kHeaderH = 80;
constexpr int kBackBtnSize = 72;
constexpr int kHeaderSidePad = 16;
constexpr int kPad = 16;
constexpr size_t kMaxNameLen = 256;          // POSIX NAME_MAX = 255
constexpr size_t kMaxPathLen = 512;
constexpr size_t kMaxTextPreviewBytes = 48 * 1024;

// ----- color palette -----
constexpr uint32_t kColorBg = 0x000000;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorTextSecondary = 0x9A9A9A;
constexpr uint32_t kColorHintText = 0x6E6E70;
constexpr uint32_t kColorDeleteBg = 0xE74C3C;
constexpr uint32_t kColorDeleteBgPress = 0xC0392B;
constexpr uint32_t kColorFileBg = 0x1A1A1A;
constexpr uint32_t kColorFileBgPress = 0x2A2A2A;
constexpr uint32_t kColorFileBorder = 0x333333;

// ----- UI elements that need updating -----
lv_obj_t *s_status_lbl      = nullptr;
lv_obj_t *s_capacity_lbl    = nullptr;
lv_obj_t *s_path_lbl        = nullptr;
lv_obj_t *s_file_list       = nullptr;
lv_obj_t *s_no_files_lbl    = nullptr;
lv_obj_t *s_status_dot      = nullptr;
lv_obj_t *s_usb_btn         = nullptr;
lv_obj_t *s_usb_btn_lbl     = nullptr;
lv_obj_t *s_usb_hint_lbl    = nullptr;
lv_obj_t *s_screen          = nullptr;
uint32_t s_screen_generation = 0;
bool s_delete_busy = false;
bool s_browse_busy = false;
uint32_t s_browse_request = 0;

// Full-screen preview overlay (image / text)
lv_obj_t *s_preview_overlay      = nullptr;
lv_obj_t *s_preview_img          = nullptr;
lv_obj_t *s_preview_text_scroll  = nullptr;
lv_obj_t *s_preview_text_lbl     = nullptr;
lv_obj_t *s_preview_title        = nullptr;
char      s_preview_lv_path[kMaxPathLen + 4] = {};  // "S:" + posix path

// Current browse directory (absolute path including mount point).
char s_cwd[kMaxPathLen] = {};

constexpr int kUsbBtnY  = kHeaderH + kPad + 64;
constexpr int kUsbHintY = kUsbBtnY + 56;
constexpr int kDividerY = kUsbHintY + 36;
constexpr int kPathY    = kDividerY + 8;
constexpr int kListY    = kPathY + 28;

// Cast helper to silence -Wdeprecated-enum-enum-conversion.
static inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state)
{
    return static_cast<lv_style_selector_t>(part | state);
}

// ----- helper: human-readable size (integer-only, newlib-nano safe) -----
void FormatSize(uint64_t bytes, char *buf, size_t buf_size)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        unsigned gb_int  = static_cast<unsigned>(bytes / (1024ULL * 1024 * 1024));
        unsigned gb_frac = static_cast<unsigned>(
            (bytes % (1024ULL * 1024 * 1024)) / (10ULL * 1024 * 1024));
        snprintf(buf, buf_size, "%u.%02u GB", gb_int, gb_frac);
    } else if (bytes >= 1024 * 1024) {
        unsigned mb_int  = static_cast<unsigned>(bytes / (1024 * 1024));
        unsigned mb_frac = static_cast<unsigned>((bytes % (1024 * 1024)) / (10 * 1024));
        snprintf(buf, buf_size, "%u.%02u MB", mb_int, mb_frac);
    } else if (bytes >= 1024) {
        unsigned kb_int  = static_cast<unsigned>(bytes / 1024);
        unsigned kb_frac = static_cast<unsigned>(((bytes % 1024) * 100) / 1024);
        snprintf(buf, buf_size, "%u.%02u KB", kb_int, kb_frac);
    } else {
        snprintf(buf, buf_size, "%lu B", static_cast<unsigned long>(bytes));
    }
}

bool ExtEqualsIgnoreCase(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr || ext == nullptr) {
        return false;
    }
    ++dot;
    while (*dot != '\0' && *ext != '\0') {
        if (std::tolower(static_cast<unsigned char>(*dot)) !=
            std::tolower(static_cast<unsigned char>(*ext))) {
            return false;
        }
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

bool IsImageFile(const char *name)
{
    return ExtEqualsIgnoreCase(name, "jpg") || ExtEqualsIgnoreCase(name, "jpeg") ||
           ExtEqualsIgnoreCase(name, "png") || ExtEqualsIgnoreCase(name, "sjpg");
}

bool IsTextFile(const char *name)
{
    return ExtEqualsIgnoreCase(name, "txt");
}

bool IsAudioFile(const char *name){return ExtEqualsIgnoreCase(name,"mp3")||ExtEqualsIgnoreCase(name,"wav");}
bool IsPreviewableFile(const char *name)
{
    return IsImageFile(name) || IsTextFile(name) || IsAudioFile(name);
}

bool IsPreviewOpen()
{
    return s_preview_overlay != nullptr &&
           !lv_obj_has_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ----- navigation -----
void UpdatePathLabel();
void RebuildFileList(lv_obj_t *parent);

void ResetCwdToRoot()
{
    const char *mount = SdCardManager::GetInstance().GetMountPoint();
    strlcpy(s_cwd, mount != nullptr ? mount : "/sdcard", sizeof(s_cwd));
}

bool IsAtRoot()
{
    const char *mount = SdCardManager::GetInstance().GetMountPoint();
    if (mount == nullptr) {
        return true;
    }
    return strcmp(s_cwd, mount) == 0;
}

bool GoUpOneLevel()
{
    if (IsAtRoot()) {
        return false;
    }
    const char *mount = SdCardManager::GetInstance().GetMountPoint();
    char *slash = strrchr(s_cwd, '/');
    if (slash == nullptr || mount == nullptr) {
        ResetCwdToRoot();
        return true;
    }
    // 截断到上一级；不得短于挂载点
    const size_t mount_len = strlen(mount);
    if (static_cast<size_t>(slash - s_cwd) <= mount_len) {
        strlcpy(s_cwd, mount, sizeof(s_cwd));
    } else {
        *slash = '\0';
    }
    return true;
}

void LeaveToHome()
{
    UsbVirtualDisk::GetInstance().DisableIfActive();
    HomeScreen::SwitchToHome();
}

// Forward declarations
void ClosePreview();

void OnNavigateBack()
{
    if (IsPreviewOpen()) {
        ClosePreview();
        return;
    }
    if (GoUpOneLevel()) {
        UpdatePathLabel();
        if (s_screen != nullptr) {
            RebuildFileList(s_screen);
        }
        return;
    }
    LeaveToHome();
}

void OnSwipeBack()
{
    OnNavigateBack();
}

void UpdatePathLabel()
{
    if (s_path_lbl == nullptr) {
        return;
    }
    const char *mount = SdCardManager::GetInstance().GetMountPoint();
    if (mount == nullptr || IsAtRoot()) {
        lv_label_set_text(s_path_lbl, "/");
        return;
    }
    // 相对挂载点显示，如 /photos/2024
    const size_t mount_len = strlen(mount);
    if (strncmp(s_cwd, mount, mount_len) == 0) {
        lv_label_set_text(s_path_lbl, s_cwd + mount_len);
    } else {
        lv_label_set_text(s_path_lbl, s_cwd);
    }
}

// ----- file deletion -----
struct FileEntry {
    char name[kMaxNameLen];
    char path[kMaxPathLen];
};

// Forward declarations
void UpdateStatusUI();
void RefreshUsbUi();

void OnUsbVirtualDiskClicked(lv_event_t * /*e*/)
{
    auto &vd = UsbVirtualDisk::GetInstance();
    if (!vd.IsSupported() || vd.IsBusy()) {
        return;
    }
    ClosePreview();
    vd.Toggle();
}

void OnUsbUiNotifyAsync(void * /*user_data*/)
{
    // 页面已卸载则忽略过期回调，避免碰已释放的 LVGL 对象
    if (s_screen == nullptr) {
        return;
    }
    ClosePreview();
    RefreshUsbUi();
    UpdateStatusUI();
    RebuildFileList(s_screen);
}

void OnUsbVirtualDiskNotify()
{
    lv_async_call(OnUsbUiNotifyAsync, nullptr);
}

void RefreshUsbUi()
{
    auto &vd = UsbVirtualDisk::GetInstance();
    if (s_usb_btn_lbl != nullptr) {
        const char *btn_text =
            vd.IsGadgetActive() ? I18n::T("停用虚拟 U 盘") : I18n::T("启用虚拟 U 盘");
        lv_label_set_text(s_usb_btn_lbl, btn_text);
    }
    if (s_usb_btn != nullptr) {
        const uint32_t bg =
            vd.IsGadgetActive() ? static_cast<uint32_t>(0xC44536) : 0x2F6FED;
        lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(bg), LV_PART_MAIN);
        if (vd.IsBusy() || !vd.IsSupported()) {
            lv_obj_add_state(s_usb_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_usb_btn, LV_STATE_DISABLED);
        }
    }
    if (s_usb_hint_lbl != nullptr) {
        lv_label_set_text(s_usb_hint_lbl, I18n::T(UsbVirtualDisk::HintMsgid(vd.GetUiHint())));
    }
}

void OnDeleteFile(lv_event_t *e)
{
    // 阻止冒泡到行点击（否则会打开预览）
    lv_event_stop_bubbling(e);

    auto &vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto *entry = static_cast<FileEntry *>(lv_event_get_user_data(e));
    if (entry == nullptr) {
        return;
    }

    if (s_delete_busy) return;
    struct DeleteJob { char path[kMaxPathLen]; uint32_t generation; };
    auto *job = static_cast<DeleteJob *>(std::calloc(1, sizeof(DeleteJob)));
    if (!job) return;
    strlcpy(job->path, entry->path, sizeof(job->path));
    job->generation = s_screen_generation;
    s_delete_busy = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    pthread_t thread;
    int ret = hi_task_create_legacy(&thread, &attr, [](void *arg) -> void * {
        auto *job = static_cast<DeleteJob *>(arg);
        // Remount and FAT writes can wait for SDMMC. Keep them off LVGL.
        int result = metalio_sdcard_ensure_rw();
        if (result == 0 && unlink(job->path) != 0) result = -errno;
        ESP_LOGI(TAG_SD, "Delete file result=%d", result);
        lv_lock();
        s_delete_busy = false;
        if (s_screen && job->generation == s_screen_generation) {
            UpdateStatusUI();
            RebuildFileList(s_screen);
            if (result < 0 && s_status_lbl)
                lv_label_set_text(s_status_lbl, I18n::T("删除失败，请稍后重试"));
        }
        lv_unlock();
        std::free(job);
        return nullptr;
    }, job);
    pthread_attr_destroy(&attr);
    if (ret == 0) pthread_detach(thread);
    else { s_delete_busy = false; std::free(job); }

}

void ClosePreview()
{
    if (s_preview_overlay != nullptr) {
        lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_preview_img != nullptr) {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        // 清掉 src，避免继续持有大图解码缓存
        lv_image_set_src(s_preview_img, nullptr);
    }
    if (s_preview_text_scroll != nullptr) {
        lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_preview_text_lbl != nullptr) {
        lv_label_set_text(s_preview_text_lbl, "");
    }
    if (s_preview_title != nullptr) {
        lv_label_set_text(s_preview_title, "");
    }
    s_preview_lv_path[0] = '\0';
}

void SetPreviewTitle(const char *name)
{
    if (s_preview_title == nullptr) {
        return;
    }
    lv_label_set_text(s_preview_title, (name != nullptr && name[0] != '\0') ? name : "");
}

void ShowPreviewOverlay()
{
    if (s_preview_overlay == nullptr) {
        return;
    }
    lv_obj_remove_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_preview_overlay);
}

void OpenImagePreview(const char *posix_path)
{
    if (s_preview_img == nullptr || posix_path == nullptr || posix_path[0] == '\0') {
        return;
    }
    // LVGL POSIX 驱动字母 S:，路径形如 S:/sdcard/foo.jpg（缓冲需在预览期间保持有效）
    const size_t path_len = strlen(posix_path);
    if (path_len + 3 > sizeof(s_preview_lv_path)) {
        ESP_LOGW(TAG_SD, "Image path too long: %s", posix_path);
        return;
    }
    s_preview_lv_path[0] = 'S';
    s_preview_lv_path[1] = ':';
    memcpy(s_preview_lv_path + 2, posix_path, path_len + 1);
    ESP_LOGI(TAG_SD, "Image preview: %s", s_preview_lv_path);

    if (s_preview_text_scroll != nullptr) {
        lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_src(s_preview_img, s_preview_lv_path);
    lv_obj_remove_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    ShowPreviewOverlay();
}

void OpenTextPreview(const char *posix_path)
{
    if (s_preview_text_lbl == nullptr || s_preview_text_scroll == nullptr ||
        posix_path == nullptr) {
        return;
    }

    FILE *fp = fopen(posix_path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG_SD, "Failed to open text: %s", posix_path);
        if (s_preview_img != nullptr) {
            lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }
    long file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }
    rewind(fp);

    const bool truncated = static_cast<size_t>(file_size) > kMaxTextPreviewBytes;
    size_t read_len = truncated ? kMaxTextPreviewBytes : static_cast<size_t>(file_size);
    // +80 for optional truncation note
    char *buf = new char[read_len + 128];
    size_t n = fread(buf, 1, read_len, fp);
    fclose(fp);
    buf[n] = '\0';
    // 粗略去掉中间的 NUL，避免 label 提前截断
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    if (truncated) {
        snprintf(buf + n, 128, "\n\n%s", I18n::T("文件过大，已截断显示"));
    }

    ESP_LOGI(TAG_SD, "Text preview: %s (%u bytes%s)", posix_path,
             static_cast<unsigned>(n), truncated ? ", truncated" : "");

    if (s_preview_img != nullptr) {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_preview_img, nullptr);
    }
    lv_label_set_text(s_preview_text_lbl, buf);
    delete[] buf;
    lv_obj_scroll_to_y(s_preview_text_scroll, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    ShowPreviewOverlay();
}

void OnPreviewFile(lv_event_t *e)
{
    auto &vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto *entry = static_cast<FileEntry *>(lv_event_get_user_data(e));
    if (entry == nullptr) {
        return;
    }
    if(IsAudioFile(entry->name)){
        HiMusicScreen::OpenFile(entry->path);
    } else if (IsImageFile(entry->name)) {
        SetPreviewTitle(entry->name);
        OpenImagePreview(entry->path);
    } else if (IsTextFile(entry->name)) {
        SetPreviewTitle(entry->name);
        OpenTextPreview(entry->path);
    }
}

void OnEnterDirectory(lv_event_t *e)
{
    auto &vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto *entry = static_cast<FileEntry *>(lv_event_get_user_data(e));
    if (entry == nullptr || entry->path[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG_SD, "Enter directory: %s", entry->path);
    strlcpy(s_cwd, entry->path, sizeof(s_cwd));
    UpdatePathLabel();
    if (s_screen != nullptr) {
        RebuildFileList(s_screen);
    }
}

// ----- file list builder -----
void ClearFileList()
{
    if (s_file_list == nullptr) {
        return;
    }
    lv_obj_clean(s_file_list);
}

// Directory metadata is captured on the storage worker, never under LVGL.
void BuildFileRow(const char *name, const char *path, bool is_dir, uint64_t size)
{
    // Row container
    lv_obj_t *row = lv_obj_create(s_file_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kPanelSize - 2 * kPad, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorFileBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(kColorFileBorder), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // File info column (name + size)
    lv_obj_t *info_col = lv_obj_create(row);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_size(info_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_CLICKABLE);

    // File/dir name
    lv_obj_t *name_lbl = lv_label_create(info_col);
    lv_label_set_text(name_lbl, DisplayUtf8(name?name:"").c_str());
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, 450);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &font_puhui_20_4, LV_PART_MAIN);

    // File size
    char size_str[64];
    if (is_dir) {
        snprintf(size_str, sizeof(size_str), I18n::T("目录"));
    } else {
        FormatSize(size, size_str, sizeof(size_str));
    }
    lv_obj_t *size_lbl = lv_label_create(info_col);
    lv_label_set_text(size_lbl, size_str);
    lv_obj_set_style_text_color(size_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(size_lbl, &font_puhui_20_4, LV_PART_MAIN);

    if (is_dir) {
        // 目录行可点进入
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorFileBgPress),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));

        auto *dir_ctx = new FileEntry;
        strlcpy(dir_ctx->name, name, sizeof(dir_ctx->name));
        strlcpy(dir_ctx->path, path, sizeof(dir_ctx->path));
        lv_obj_add_event_cb(row, OnEnterDirectory, LV_EVENT_CLICKED, dir_ctx);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t *ev) { delete static_cast<FileEntry *>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, dir_ctx);
    } else {
        // Delete button (only for files, not directories)
        lv_obj_t *del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, 64, 40);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorDeleteBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(del_btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorDeleteBgPress),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
        // 删除钮自身点击勿被行预览抢走；也不触发右滑返回
        screen_swipe_back_ignore(del_btn, true);

        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(del_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
        lv_obj_center(del_lbl);

        // Allocate FileEntry on heap so it outlives this function
        auto *file_ctx = new FileEntry;
        strlcpy(file_ctx->name, name, sizeof(file_ctx->name));
        strlcpy(file_ctx->path, path, sizeof(file_ctx->path));

        // CLICKED -> delete file
        lv_obj_add_event_cb(del_btn, OnDeleteFile, LV_EVENT_CLICKED, file_ctx);

        // DELETE -> free the FileEntry
        lv_obj_add_event_cb(
            del_btn,
            [](lv_event_t *ev) { delete static_cast<FileEntry *>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, file_ctx);

        // jpg/png/sjpg/txt：点行打开预览
        if (IsPreviewableFile(name)) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, lv_color_hex(kColorFileBgPress),
                                      Sel(LV_PART_MAIN, LV_STATE_PRESSED));
            auto *preview_ctx = new FileEntry;
            strlcpy(preview_ctx->name, name, sizeof(preview_ctx->name));
            strlcpy(preview_ctx->path, path, sizeof(preview_ctx->path));
            lv_obj_add_event_cb(row, OnPreviewFile, LV_EVENT_CLICKED, preview_ctx);
            lv_obj_add_event_cb(
                row,
                [](lv_event_t *ev) {
                    delete static_cast<FileEntry *>(lv_event_get_user_data(ev));
                },
                LV_EVENT_DELETE, preview_ctx);
        }
    }
}

// 拼 cwd/name；超长则返回 false（跳过该条目，避免路径截断）
bool JoinPath(char *out, size_t out_size, const char *dir, const char *name)
{
    if (out == nullptr || out_size == 0 || dir == nullptr || name == nullptr) {
        return false;
    }
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    // dir + '/' + name + '\0'
    if (dir_len + 1 + name_len + 1 > out_size) {
        return false;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len + 1);
    return true;
}

struct BrowseEntry { std::string name, path; bool directory; uint64_t size; };
struct BrowseJob {
    uint32_t generation, request;
    std::string cwd;
    std::vector<BrowseEntry> entries;
    int error=0;
    bool exported=false, root_fallback=false;
    unsigned long long total_mb=0, free_mb=0;bool free_known=false;
};

void RebuildFileList(lv_obj_t *parent)
{
    (void)parent;
    ++s_browse_request;
    if (!s_screen || s_browse_busy) return;
    if (!s_cwd[0]) ResetCwdToRoot();
    s_browse_busy=true;
    ClearFileList();
    if(s_no_files_lbl){lv_label_set_text(s_no_files_lbl,I18n::T("正在读取 SD 卡…"));lv_obj_remove_flag(s_no_files_lbl,LV_OBJ_FLAG_HIDDEN);}
    auto *job=new BrowseJob{s_screen_generation,s_browse_request,s_cwd};
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
    int ret=hi_task_create_legacy(&thread,&attr,[](void *arg)->void*{
        auto *j=static_cast<BrowseJob*>(arg);
        j->exported=UsbVirtualDisk::GetInstance().IsSdExportedToHost();
        if(j->exported)j->error=-EBUSY;
        else {
            j->error=metalio_sdcard_mount_ro();
            if(j->error==0){
                DIR *dir=opendir(j->cwd.c_str());
                if(!dir && j->cwd!="/sdcard"){
                    j->cwd="/sdcard";j->root_fallback=true;dir=opendir(j->cwd.c_str());
                }
                if(!dir)j->error=-errno;
                else {
                    errno=0;
                    while(auto *entry=readdir(dir)){
                        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
                        std::string full=j->cwd+"/"+entry->d_name;
                        if(full.size()>=kMaxPathLen)continue;
                        struct stat st{};
                        if(stat(full.c_str(),&st)==0)j->entries.push_back({entry->d_name,full,S_ISDIR(st.st_mode),(uint64_t)st.st_size});
                        errno=0;
                    }
                    if(errno)j->error=-errno;
                    closedir(dir);
                    std::sort(j->entries.begin(),j->entries.end(),[](const BrowseEntry&a,const BrowseEntry&b){
                        if(a.directory!=b.directory)return a.directory>b.directory;
                        return strcasecmp(a.name.c_str(),b.name.c_str())<0;
                    });
                }
                uint64_t total=0,available=UINT64_MAX;
                if(metalio_sdcard_capacity(&total,&available)==0){j->total_mb=total/(1024*1024);j->free_known=available!=UINT64_MAX;if(j->free_known)j->free_mb=available/(1024*1024);}
            }
        }
        // No SD operations while holding the display lock. A stale navigation
        // result is discarded and the newest folder request is serviced next.
        lv_lock();
        bool valid=s_screen && j->generation==s_screen_generation && j->request==s_browse_request;
        if(valid){
            if(j->root_fallback){strlcpy(s_cwd,j->cwd.c_str(),sizeof(s_cwd));UpdatePathLabel();}
            if(s_status_dot)lv_obj_set_style_bg_color(s_status_dot,lv_color_hex(j->error?0xFF0000:0x00CC00),LV_PART_MAIN);
            if(s_status_lbl)lv_label_set_text(s_status_lbl,I18n::T(j->exported?"SD 正被电脑占用，停用或弹出后可浏览":j->error?"SD 卡读取失败，请重试":"SD 卡已插入"));
            if(s_capacity_lbl && j->error==0){char text[96];
                if(j->free_known)snprintf(text,sizeof(text),"剩余约 %llu MB / 总容量 %llu MB",j->free_mb,j->total_mb);
                else snprintf(text,sizeof(text),"总容量 %llu MB",j->total_mb);
                lv_label_set_text(s_capacity_lbl,text);}
            ClearFileList();
            if(s_no_files_lbl){lv_label_set_text(s_no_files_lbl,I18n::T(j->exported?"SD 正被电脑占用，停用或弹出后可浏览":j->error?"SD 卡读取失败，请退出后重试":"此文件夹为空"));lv_obj_remove_flag(s_no_files_lbl,LV_OBJ_FLAG_HIDDEN);}
        }
        lv_unlock();
        // Publish small batches so a large Music/Camera folder cannot stall
        // touch handling, gestures or the global incoming-call surface.
        for(size_t at=0;valid&&at<j->entries.size();){
            lv_lock();valid=s_screen&&j->generation==s_screen_generation&&j->request==s_browse_request;
            if(valid){size_t end=std::min(at+12,j->entries.size());for(;at<end;++at){const auto&e=j->entries[at];BuildFileRow(e.name.c_str(),e.path.c_str(),e.directory,e.size);}
                if(s_no_files_lbl)lv_obj_add_flag(s_no_files_lbl,LV_OBJ_FLAG_HIDDEN);}
            lv_unlock();usleep(2000);
        }
        ESP_LOGI(TAG_SD,"browse result=%d files=%u",j->error,(unsigned)j->entries.size());
        lv_lock();s_browse_busy=false;
        if(s_screen&&(j->generation!=s_screen_generation||j->request!=s_browse_request))RebuildFileList(s_screen);
        lv_unlock();delete j;return nullptr;
    },job);
    pthread_attr_destroy(&attr);
    if(ret==0)pthread_detach(thread);
    else {s_browse_busy=false;delete job;if(s_no_files_lbl)lv_label_set_text(s_no_files_lbl,I18n::T("系统忙，请退出后重试"));}
}

// ----- header builder -----
void BuildHeader(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    auto back_fn = [](lv_event_t * /*e*/) { OnNavigateBack(); };
    lv_obj_add_event_cb(back_btn, back_fn, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("文件管理"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + kHeaderSidePad, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

// ----- status section -----
void BuildStatusSection(lv_obj_t *parent)
{
    lv_obj_t *status_row = lv_obj_create(parent);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, kPanelSize - 2 * kPad, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 12, LV_PART_MAIN);
    lv_obj_set_pos(status_row, kPad, kHeaderH + kPad);

    s_status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 12, 12);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);

    s_status_lbl = lv_label_create(status_row);
    lv_label_set_text(s_status_lbl, I18n::T("检测中..."));
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);

    s_capacity_lbl = lv_label_create(parent);
    lv_label_set_text(s_capacity_lbl, "");
    lv_obj_set_style_text_color(s_capacity_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_capacity_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(s_capacity_lbl, kPad, kHeaderH + kPad + 32);

    // Virtual USB drive toggle. UsbVirtualDisk::IsSupported() returns false
    // on openvela today, so RefreshUsbUi() will disable the button.
    s_usb_btn = lv_button_create(parent);
    lv_obj_set_size(s_usb_btn, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(s_usb_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_radius(s_usb_btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(0x2F6FED), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_usb_btn, 0, LV_PART_MAIN);
    lv_obj_align(s_usb_btn, LV_ALIGN_TOP_LEFT, kPad, kUsbBtnY);
    lv_obj_add_event_cb(s_usb_btn, OnUsbVirtualDiskClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_usb_btn, true);

    s_usb_btn_lbl = lv_label_create(s_usb_btn);
    lv_label_set_text(s_usb_btn_lbl, I18n::T("启用虚拟 U 盘"));
    lv_obj_set_style_text_color(s_usb_btn_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_btn_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_usb_btn_lbl);

    s_usb_hint_lbl = lv_label_create(parent);
    lv_obj_set_width(s_usb_hint_lbl, kPanelSize - 2 * kPad);
    lv_label_set_long_mode(s_usb_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_usb_hint_lbl, I18n::T("启用后电脑可将本机识别为 U 盘"));
    lv_obj_set_style_text_color(s_usb_hint_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_hint_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(s_usb_hint_lbl, kPad, kUsbHintY);

    lv_obj_t *divider = lv_obj_create(parent);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, kPanelSize - 2 * kPad, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorFileBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_pos(divider, kPad, kDividerY);
}

void BuildFileListSection(lv_obj_t *parent)
{
    s_path_lbl = lv_label_create(parent);
    lv_label_set_text(s_path_lbl, "/");
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_path_lbl, kPanelSize - 2 * kPad);
    lv_obj_set_style_text_color(s_path_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_path_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(s_path_lbl, kPad, kPathY);

    s_no_files_lbl = lv_label_create(parent);
    lv_label_set_text(s_no_files_lbl, "");
    lv_obj_set_style_text_color(s_no_files_lbl, lv_color_hex(kColorHintText), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_no_files_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_no_files_lbl, LV_ALIGN_CENTER, 0, 40);

    constexpr int kListH = kPanelSize - kListY - kPad;
    s_file_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_file_list);
    lv_obj_set_size(s_file_list, kPanelSize - 2 * kPad, kListH);
    lv_obj_set_pos(s_file_list, kPad, kListY);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_file_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_OFF);
}

void BuildPreviewOverlay(lv_obj_t *parent)
{
    s_preview_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_preview_overlay);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_preview_overlay, kPanelSize, kPanelSize);
    lv_obj_set_pos(s_preview_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_preview_overlay, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preview_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_preview_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *header = lv_obj_create(s_preview_overlay);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back_btn, true);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t * /*e*/) { ClosePreview(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_preview_title = lv_label_create(header);
    lv_label_set_text(s_preview_title, "");
    lv_label_set_long_mode(s_preview_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_preview_title,
                     kPanelSize - (kHeaderSidePad + kBackBtnSize + kHeaderSidePad) -
                         kHeaderSidePad);
    lv_obj_set_style_text_color(s_preview_title, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_preview_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_preview_title, LV_ALIGN_LEFT_MID,
                 kHeaderSidePad + kBackBtnSize + kHeaderSidePad, 0);
    lv_obj_remove_flag(s_preview_title, LV_OBJ_FLAG_CLICKABLE);

    s_preview_img = lv_image_create(s_preview_overlay);
    lv_obj_set_size(s_preview_img, kPanelSize, kPanelSize - kHeaderH);
    lv_obj_align(s_preview_img, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_image_set_inner_align(s_preview_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(s_preview_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);

    s_preview_text_scroll = lv_obj_create(s_preview_overlay);
    lv_obj_remove_style_all(s_preview_text_scroll);
    lv_obj_set_size(s_preview_text_scroll, kPanelSize - 2 * kPad,
                    kPanelSize - kHeaderH - kPad);
    lv_obj_set_pos(s_preview_text_scroll, kPad, kHeaderH);
    lv_obj_set_style_bg_opa(s_preview_text_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_preview_text_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_preview_text_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);

    s_preview_text_lbl = lv_label_create(s_preview_text_scroll);
    lv_obj_set_width(s_preview_text_lbl, kPanelSize - 2 * kPad);
    lv_label_set_long_mode(s_preview_text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_preview_text_lbl, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_preview_text_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_text(s_preview_text_lbl, "");
}

void UpdateStatusUI()
{
    // The following RebuildFileList request performs mount, capacity and
    // directory I/O on its worker. Never wait for a card under the UI lock.
    if(s_status_lbl)lv_label_set_text(s_status_lbl,I18n::T("正在读取 SD 卡…"));
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================
lv_obj_t *SdCardScreen::CreateStatic()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_screen = scr;
    ++s_screen_generation;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    ResetCwdToRoot();

    BuildHeader(scr);
    BuildStatusSection(scr);
    BuildFileListSection(scr);
    BuildPreviewOverlay(scr);

    auto &vd = UsbVirtualDisk::GetInstance();
    vd.Init();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    RefreshUsbUi();

    // Mount and enumerate asynchronously; reopening retries a failed mount.
    UpdateStatusUI();
    UpdatePathLabel();
    RebuildFileList(scr);

    // Right-swipe：子目录上一级，根目录回首页
    screen_attach_swipe_back(scr, OnSwipeBack);

    // The shared pointer observer preserves normal left-side taps.

    return scr;
}

lv_obj_t *SdCardScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void SdCardScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_SD, "load: sd_card_screen (mounted=%d gadget=%d cwd=%s)",
                 0,
                 UsbVirtualDisk::GetInstance().IsGadgetActive() ? 1 : 0, s_cwd);
        RefreshUsbUi();
        UpdateStatusUI();
        UpdatePathLabel();
        if (s_screen != nullptr) {
            RebuildFileList(s_screen);
        }
    } else {
        ESP_LOGI(TAG_SD, "unload: sd_card_screen");
        UsbVirtualDisk::GetInstance().DisableIfActive();
        UsbVirtualDisk::GetInstance().SetUiNotify(nullptr);
        ClosePreview();
        s_screen = nullptr;
        ++s_screen_generation;
        s_usb_btn = nullptr;
        s_usb_btn_lbl = nullptr;
        s_usb_hint_lbl = nullptr;
        s_status_lbl = nullptr;
        s_capacity_lbl = nullptr;
        s_path_lbl = nullptr;
        s_file_list = nullptr;
        s_no_files_lbl = nullptr;
        s_status_dot = nullptr;
        s_preview_overlay = nullptr;
        s_preview_img = nullptr;
        s_preview_text_scroll = nullptr;
        s_preview_text_lbl = nullptr;
        s_preview_title = nullptr;
        s_preview_lv_path[0] = '\0';
        s_cwd[0] = '\0';
    }
}
