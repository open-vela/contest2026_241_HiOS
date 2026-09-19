/*
 * Local STT command handler for chat — volume, backlight, bluetooth power,
 * and opening HiOS apps via HomeScreen::LaunchApp. No cloud MCP.
 *
 * Local LED (GPIO48) handling is intentionally disabled; use cloud MCP
 * self.led.* tools in mcp_server.cxx instead.
 */

#include "chat_local_cmd.h"
#include "application.h"
#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "settings.h"
#include "home_screen/home_screen.h"
#include "chat_screen/chat_screen.h"
#include "display.h"

#include <metalio/metalio.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#if 0  /* local LED disabled — keep MCP self.led.* only */
/* Same GPIO48 LED as mcp_server.cxx self.led.* tools. */
extern "C" {
int esp_configgpio(int pin, uint16_t attr);
void esp_gpiowrite(int pin, bool value);
bool esp_gpioread(int pin);
}
#define USER_LED_GPIO        48
#define USER_LED_ACTIVE_HIGH 1
#define USER_GPIO_ATTR_OUT   (1u << 1)
#endif

namespace {

bool Contains(const std::string &s, const char *sub)
{
    return sub != nullptr && s.find(sub) != std::string::npos;
}

/* First decimal integer in UTF-8 text (e.g. "音量调到50" → 50). */
bool ExtractInt(const std::string &s, int *out)
{
    if (out == nullptr)
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            continue;
        int v = 0;
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
        {
            v = v * 10 + (s[j] - '0');
            if (v > 9999)
                break;
            ++j;
        }
        *out = v;
        return true;
    }
    return false;
}

void Reply(const char *msg)
{
    auto *disp = Board::GetInstance().GetDisplay();
    if (disp != nullptr && msg != nullptr)
        disp->SetChatMessage("assistant", msg);
}

void SetVolumeLocal(int vol)
{
    if (vol < 0)
        vol = 0;
    if (vol > 100)
        vol = 100;
    Application::GetInstance().GetAudioService().SetVolume(vol);
    write(1, "LOC_VOL\n", 8);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "已将音量调到 %d%%", vol);
    Reply(buf);
}

void AdjustVolumeLocal(int delta)
{
    auto &as = Application::GetInstance().GetAudioService();
    int vol = as.GetVolume() + delta;
    SetVolumeLocal(vol);
}

void SetBrightnessLocal(int pct)
{
    if (pct < 5)
        pct = 5;
    if (pct > 100)
        pct = 100;
    Settings settings("display", true);
    settings.SetInt("brightness", pct);
    if (auto *bl = Board::GetInstance().GetBacklight())
        bl->SetBrightness(static_cast<uint8_t>(pct), true);
    write(1, "LOC_BL\n", 7);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "已将背光调到 %d%%", pct);
    Reply(buf);
}

void AdjustBrightnessLocal(int delta)
{
    auto *bl = Board::GetInstance().GetBacklight();
    int pct = bl ? bl->GetBrightness() : 50;
    SetBrightnessLocal(pct + delta);
}

void SetBluetoothLocal(bool on)
{
    metalio_bt_power(on);
    write(1, on ? "LOC_BT_ON\n" : "LOC_BT_OFF\n", on ? 10 : 11);
    Reply(on ? "已打开蓝牙" : "已关闭蓝牙");
}

#if 0  /* local LED disabled */
void UserLedEnsureOut()
{
    static bool inited = false;
    if (!inited)
    {
        esp_configgpio(USER_LED_GPIO, USER_GPIO_ATTR_OUT);
        inited = true;
    }
}

void SetLedLocal(bool on)
{
    UserLedEnsureOut();
    esp_gpiowrite(USER_LED_GPIO, on ? (bool)USER_LED_ACTIVE_HIGH
                                    : (bool)!USER_LED_ACTIVE_HIGH);
    write(1, on ? "LOC_LED_ON\n" : "LOC_LED_OFF\n", on ? 11 : 12);
    /* Also emit MCP markers so existing serial checks keep working. */
    write(1, on ? "MCP_LED_ON\n" : "MCP_LED_OFF\n", on ? 11 : 12);
    Reply(on ? "已开灯" : "已关灯");
}

void ReplyLedStateLocal()
{
    UserLedEnsureOut();
    bool on = (esp_gpioread(USER_LED_GPIO) == (bool)USER_LED_ACTIVE_HIGH);
    write(1, "LOC_LED_GET\n", 12);
    Reply(on ? "灯是开着的" : "灯是关着的");
}

bool IsLedPhrase(const std::string &s)
{
    /* Match 开灯/关灯 and common variants; avoid bare "灯" alone. */
    return Contains(s, "开灯") || Contains(s, "关灯") ||
           Contains(s, "打开灯") || Contains(s, "关闭灯") ||
           Contains(s, "把灯打开") || Contains(s, "把灯关掉") ||
           Contains(s, "把灯关了") || Contains(s, "灯打开") ||
           Contains(s, "灯关掉") || Contains(s, "灯亮着") ||
           Contains(s, "灯什么状态") || Contains(s, "灯的状态");
}
#endif

bool OpenAppLocal(const std::string &query)
{
    if (ChatScreen::IsActive())
        Application::GetInstance().SetVoiceUiDesired(false);
    if (!HomeScreen::LaunchApp(query.c_str()))
    {
        write(1, "LOC_APP_FAIL\n", 13);
        Reply(("未找到应用：" + query).c_str());
        return false;
    }
    write(1, "LOC_APP\n", 8);
    Reply(("正在打开" + query).c_str());
    return true;
}

/* Strip leading open-verbs; return remaining app name (may be empty). */
std::string StripOpenVerb(const std::string &s)
{
    static const char *kVerbs[] = {"打开", "开启", "启动", "进入", "去打开",
                                   "帮我打开", "请打开", nullptr};
    std::string t = s;
    while (!t.empty() && (t[0] == ' ' || t[0] == '\t'))
        t.erase(0, 1);
    for (int i = 0; kVerbs[i] != nullptr; ++i)
    {
        const size_t n = std::strlen(kVerbs[i]);
        if (t.size() >= n && t.compare(0, n, kVerbs[i]) == 0)
        {
            t = t.substr(n);
            while (!t.empty() && (t[0] == ' ' || t[0] == '\t'))
                t.erase(0, 1);
            break;
        }
    }
    return t;
}

}  // namespace

bool ChatLocalCmd_TryHandle(const std::string &raw)
{
    if (raw.empty())
        return false;

    /* Normalize: drop spaces for matching. */
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw)
    {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            s.push_back(static_cast<char>(c));
    }
    if (s.empty())
        return false;

    /* ---- Volume ---- */
    if (Contains(s, "静音") || Contains(s, "关闭声音") ||
        Contains(s, "把声音关了"))
    {
        SetVolumeLocal(0);
        return true;
    }
    if (Contains(s, "最大音量") || Contains(s, "音量最大"))
    {
        SetVolumeLocal(100);
        return true;
    }
    if (Contains(s, "大声一点") || Contains(s, "声音大一点") ||
        Contains(s, "音量大一点") || Contains(s, "再大声") ||
        (Contains(s, "大一点") &&
         (Contains(s, "音量") || Contains(s, "声音"))))
    {
        AdjustVolumeLocal(+15);
        return true;
    }
    if (Contains(s, "小声一点") || Contains(s, "声音小一点") ||
        Contains(s, "音量小一点") || Contains(s, "再小声") ||
        (Contains(s, "小一点") &&
         (Contains(s, "音量") || Contains(s, "声音"))))
    {
        AdjustVolumeLocal(-15);
        return true;
    }
    if (Contains(s, "音量") || Contains(s, "声音"))
    {
        int v = 0;
        if (ExtractInt(s, &v))
        {
            SetVolumeLocal(v);
            return true;
        }
    }

    /* ---- Backlight ---- */
    if (Contains(s, "亮一点") || Contains(s, "屏幕亮一点") ||
        Contains(s, "背光亮一点"))
    {
        AdjustBrightnessLocal(+15);
        return true;
    }
    if (Contains(s, "暗一点") || Contains(s, "屏幕暗一点") ||
        Contains(s, "背光暗一点"))
    {
        AdjustBrightnessLocal(-15);
        return true;
    }
    if (Contains(s, "亮度") || Contains(s, "背光"))
    {
        /* "打开背光" is open-app; absolute set needs a number. */
        int v = 0;
        if (ExtractInt(s, &v))
        {
            SetBrightnessLocal(v);
            return true;
        }
    }

    /* ---- Bluetooth power (before generic 打开…) ---- */
    if (Contains(s, "关闭蓝牙") || Contains(s, "关掉蓝牙") ||
        Contains(s, "蓝牙关闭") || Contains(s, "关蓝牙"))
    {
        SetBluetoothLocal(false);
        return true;
    }
    if (s == "打开蓝牙" || s == "开启蓝牙" || s == "开蓝牙" ||
        s == "蓝牙打开" || Contains(s, "打开蓝牙") ||
        Contains(s, "开启蓝牙"))
    {
        /* Exact-ish power on; "打开蓝牙设置" falls through to app open. */
        if (!Contains(s, "设置") && !Contains(s, "界面") &&
            !Contains(s, "页面"))
        {
            SetBluetoothLocal(true);
            return true;
        }
    }

    /* ---- User LED (disabled: use cloud MCP self.led.*) ---- */
#if 0
    if (IsLedPhrase(s))
    {
        if (Contains(s, "关灯") || Contains(s, "关闭灯") ||
            Contains(s, "把灯关掉") || Contains(s, "把灯关了") ||
            Contains(s, "灯关掉"))
        {
            SetLedLocal(false);
            return true;
        }
        if (Contains(s, "灯亮着") || Contains(s, "灯什么状态") ||
            Contains(s, "灯的状态"))
        {
            ReplyLedStateLocal();
            return true;
        }
        /* 请开灯 / 开灯 / 打开灯 / 把灯打开 … */
        SetLedLocal(true);
        return true;
    }
#endif

    /* ---- Open local HiOS app ---- */
    if (Contains(s, "打开") || Contains(s, "开启") || Contains(s, "启动") ||
        Contains(s, "进入"))
    {
        std::string name = StripOpenVerb(s);
        if (name.empty())
            return false;
        /* Avoid treating pure volume/brightness phrases as apps. */
        if (name == "音量" || name == "声音")
        {
            OpenAppLocal("设置");
            return true;
        }
#if 0  /* local LED disabled */
        if (name == "灯" || name == "LED" || name == "led")
            return false;
#endif
        return OpenAppLocal(name);
    }

    return false;
}
