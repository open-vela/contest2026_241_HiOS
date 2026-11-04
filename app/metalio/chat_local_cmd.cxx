/*
 * Local STT command handler for chat — volume, backlight, bluetooth power,
 * and opening HiOS apps via HomeScreen::LaunchApp. No cloud MCP.
 *
 * Local LED (GPIO48) handling is intentionally disabled; use cloud MCP
 * self.led.* tools in mcp_server.cxx instead.
 */

#include "chat_local_cmd.h"
#include "application.h"
#include "system_chrome/system_chrome.h"
#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "settings.h"
#include "home_screen/home_screen.h"
#include "panel_screen/panel_screen.h"
#include "smart_home/smart_home.h"
#include "chat_screen/chat_screen.h"
#include "display.h"
#include "camera_screen/camera_screen.h"
#include "hi_music_screen/hi_music_screen.h"
#include "call_screen/call_screen.h"
#include "phone_service.h"
#include "esp_timer_shim.h"
#include <atomic>
#include <vector>

#include <metalio/metalio.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
std::atomic<int64_t> s_vision_open_until_us{0};

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
    Application::GetInstance().GetAudioService().SetVolume(vol, false);
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

void ReplyBatteryLocal()
{
    int level = 0;
    int charging = 0;
    int discharging = 0;
    char buf[64];
    if (!metalio_board_peek_battery(&level, &charging, &discharging) &&
        !metalio_board_get_battery(&level, &charging, &discharging))
    {
        Reply("暂时读不到电量。");
        return;
    }
    if (charging)
        std::snprintf(buf, sizeof(buf), "当前电量 %d%%，正在充电。", level);
    else
        std::snprintf(buf, sizeof(buf), "当前电量 %d%%。", level);
    write(1, "LOC_BAT\n", 8);
    Reply(buf);
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
    if (ChatLocalCmd_IsVisionAppName(query)) {
        // Consume before scheduling: a delayed duplicate cloud tool cannot
        // reopen the camera after the user has dismissed it.
        if(!ChatLocalCmd_ConsumeVisionOpenRequest()) return false;
        lv_lock();
        const bool opened=CameraScreen::IsLookAndSeeMode() ||
            lv_async_call([](void *) { CameraScreen::OpenLookAndSee(false); }, nullptr)==LV_RESULT_OK;
        lv_unlock();
        if (!opened) {
            Reply("Hi视界已内置，但当前无法创建界面，请稍后再试。");
            return true;
        }
        Reply("Hi视界已打开，可以询问画面，也可以继续聊天。");
        return true;
    }
    if (ChatScreen::IsActive()||SystemChrome_FloatAiOpen())
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
    static const char *kVerbs[] = {"请帮我打开", "帮我打开", "给我打开", "我想打开",
                                   "请打开", "打开", "开启", "启动", "进入", "去打开", nullptr};
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

bool ChatLocalCmd_IsVisionAppName(const std::string &name)
{
    std::string key;
    key.reserve(name.size());
    for (unsigned char c:StripOpenVerb(name)) {
        if (c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='_' || c=='-') continue;
        key.push_back(c<128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
    }
    // Do not match bare 世界: ordinary questions and other app names must
    // keep their meaning. These are names of the explicitly requested app.
    return key=="视界" || key=="hi视界" || key=="看一看" ||
        key=="嗨世界" || key=="hi世界" || key=="嗨视觉" ||
        key=="hi视觉" || key=="嗨视界" || key=="hivision" ||
        key=="vision" || key=="lookandsee";
}

std::string ChatLocalCmd_NormalizeVisionOpenCommand(const std::string &text)
{
    s_vision_open_until_us.store(0,std::memory_order_release);
    // Match an affirmative command and the COMPLETE app name. Mentioning
    // it, asking why it opened, or saying not to open it grants no permission.
    if (Contains(text,"不要")||Contains(text,"不用")||Contains(text,"别")||
        Contains(text,"不想")||Contains(text,"不打开")||Contains(text,"别再")) return text;
    std::string name=StripOpenVerb(text);
    bool trimmed=true;
    while(trimmed) {
        trimmed=false;
        for(const char *end:{" ","一下","吧","。","！","!","，",","}) {
            const size_t n=std::strlen(end);
            if(name.size()>=n && name.compare(name.size()-n,n,end)==0) {
                name.resize(name.size()-n);trimmed=true;break;
            }
        }
    }
    if (ChatLocalCmd_IsVisionAppName(name)) {
        s_vision_open_until_us.store(esp_timer_get_time()+10000000,std::memory_order_release);
        return "打开Hi 视界";
    }
    return text;
}

bool ChatLocalCmd_ConsumeVisionOpenRequest()
{
    return s_vision_open_until_us.exchange(0,std::memory_order_acq_rel)>esp_timer_get_time();
}

bool EndsWithText(const std::string &s, const char *suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void TrimCallTail(std::string &name)
{
    bool changed = true;
    while (changed && !name.empty())
    {
        changed = false;
        for (const char *tail : {"吧", "呀", "啊", "呢", "哦", "嘛", "了",
                                 "。", "！", "，", "？", "?", "!"})
        {
            if (!EndsWithText(name, tail))
                continue;
            name.resize(name.size() - std::strlen(tail));
            changed = true;
        }
    }
    if (EndsWithText(name, "的电话") && name.size() > std::strlen("的电话"))
        name.resize(name.size() - std::strlen("的电话"));
    else if (EndsWithText(name, "电话") && name.size() > std::strlen("电话"))
        name.resize(name.size() - std::strlen("电话"));
}

std::string DialDigits(const std::string &raw)
{
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c >= '0' && c <= '9')
            out.push_back(static_cast<char>(c));
        else if (c == '+' && out.empty())
            out.push_back('+');
    }
    return out;
}

bool IsDialNumber(const std::string &s)
{
    return s.size() >= 3 && s.size() <= 24 && s == DialDigits(s);
}

void QueueDial(const std::string &number)
{
    auto *req = new char[25];
    std::snprintf(req, 25, "%s", number.c_str());
    lv_lock();
    const lv_result_t queued = lv_async_call([](void *p) {
        auto *n = static_cast<char *>(p);
        if (!CallScreen::DialVoice(n))
            Reply("暂时打不了，正在通话或电话界面打不开。");
        delete[] n;
    }, req);
    lv_unlock();
    if (queued != LV_RESULT_OK)
    {
        delete[] req;
        Reply("暂时无法拨号。");
    }
}

/* must: the user said 打电话给, so a miss still stays on this device.
 * Optional verbs (呼叫/拨打) fall through when nobody matches. */
bool PlaceCall(const std::string &query, bool must)
{
    if (query.empty())
    {
        if (!must)
            return false;
        Reply("要打给谁？");
        return true;
    }
    const auto book = PhoneService::Contacts();
    if (IsDialNumber(query))
    {
        std::string who = query;
        for (const auto &c : book)
        {
            if (DialDigits(c.number) == query)
            {
                who = c.name;
                break;
            }
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "正在打给%s。", who.c_str());
        Reply(buf);
        QueueDial(query);
        write(1, "LOC_CALL\n", 9);
        return true;
    }

    std::vector<PhoneService::Contact> exact;
    std::vector<PhoneService::Contact> partial;
    for (const auto &c : book)
    {
        if (c.name == query)
            exact.push_back(c);
        else if (PhoneService::Matches(c, query))
            partial.push_back(c);
    }
    const std::vector<PhoneService::Contact> &hits = !exact.empty() ? exact : partial;
    if (hits.size() != 1)
    {
        if (!must)
            return false;
        if (hits.empty())
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "通讯录里没有「%s」。", query.c_str());
            Reply(buf);
        }
        else
        {
            std::string msg = "找到多个联系人：";
            const size_t shown = hits.size() < 4 ? hits.size() : 4;
            for (size_t i = 0; i < shown; ++i)
            {
                if (i)
                    msg += "、";
                msg += hits[i].name;
            }
            if (hits.size() > shown)
                msg += "等";
            msg += "，请说全名。";
            Reply(msg.c_str());
        }
        return true;
    }
    const std::string number = DialDigits(hits[0].number);
    if (!IsDialNumber(number))
    {
        Reply("这个联系人没有可用号码。");
        return true;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "正在打给%s。", hits[0].name.c_str());
    Reply(buf);
    QueueDial(number);
    write(1, "LOC_CALL\n", 9);
    return true;
}

/* 0 = not a call, 1 = must handle, 2 = only if a contact or number matches.
 * The verb may sit in the middle, as in “我想打电话给张三”. */
int ExtractCallee(const std::string &s, std::string &name)
{
    if (Contains(s, "不要") || Contains(s, "不用") || Contains(s, "不想") ||
        Contains(s, "别打") || Contains(s, "别给"))
        return 0;
    static const char *kMust[] = {
        "请帮我打电话给", "帮我打电话给", "请打电话给", "打个电话给",
        "拨个电话给", "拨打电话给", "打电话给", "帮我打给", "请打给", "打给",
        nullptr};
    size_t at = std::string::npos;
    size_t len = 0;
    for (int i = 0; kMust[i] != nullptr; ++i)
    {
        const size_t n = std::strlen(kMust[i]);
        const size_t p = s.find(kMust[i]);
        if (p == std::string::npos)
            continue;
        if (at == std::string::npos || p < at || (p == at && n > len))
        {
            at = p;
            len = n;
        }
    }
    if (at != std::string::npos)
    {
        name = s.substr(at + len);
        TrimCallTail(name);
        return 1;
    }
    const size_t ge = s.find("给");
    if (ge != std::string::npos)
    {
        static const char *kTail[] = {"打个电话", "打电话", nullptr};
        for (int i = 0; kTail[i] != nullptr; ++i)
        {
            const size_t n = std::strlen(kTail[i]);
            const size_t tail = s.find(kTail[i], ge + std::strlen("给"));
            if (tail == std::string::npos || tail <= ge + std::strlen("给"))
                continue;
            name = s.substr(ge + std::strlen("给"), tail - (ge + std::strlen("给")));
            TrimCallTail(name);
            return 1;
        }
    }
    static const char *kOptional[] = {
        "帮我拨打", "请拨打", "拨打给", "拨给", "拨打", "呼叫", nullptr};
    for (int i = 0; kOptional[i] != nullptr; ++i)
    {
        const size_t n = std::strlen(kOptional[i]);
        const size_t p = s.find(kOptional[i]);
        if (p == std::string::npos || p + n >= s.size())
            continue;
        name = s.substr(p + n);
        TrimCallTail(name);
        if (!name.empty())
            return 2;
    }
    return 0;
}

bool ChatLocalCmd_CallContact(const std::string &name_or_number)
{
    std::string name;
    name.reserve(name_or_number.size());
    for (char c : name_or_number)
    {
        if (c != ' ' && c != '\t')
            name.push_back(c);
    }
    TrimCallTail(name);
    std::string callee;
    if (ExtractCallee(name, callee) != 0)
        name = callee;
    if (name.empty())
    {
        Reply("要打给谁？");
        return false;
    }
    PlaceCall(name, true);
    return true;
}

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

    // Resolve explicit song/artist requests locally before sending them to
    // the cloud. Ordinary questions, radio and recording controls keep their
    // existing paths. A match never silently substitutes an unrelated song.
    if(!Contains(s,"不想")&&!Contains(s,"不要")&&!Contains(s,"电台")&&!Contains(s,"录音")) {
        static const char *verbs[]={"我想要听","我想听","想听","帮我播放","请播放","播放一首","播放歌曲","播放"};
        std::string query;
        for(const char *verb:verbs)if(s.compare(0,std::strlen(verb),verb)==0){query=s.substr(std::strlen(verb));break;}
        for(const char *end:{"。","！","，","吧","的歌曲","的音乐","的歌"})
            if(query.size()>=std::strlen(end)&&query.compare(query.size()-std::strlen(end),std::strlen(end),end)==0)query.resize(query.size()-std::strlen(end));
        if(!query.empty()&&query!="音乐"&&query!="Hi音乐"&&query!="这首歌"&&query!="下一首") {
            const bool queued=HiMusicScreen::PlayLocalQuery(query);
            Reply(queued?"正在查找本地歌曲。":"暂时无法启动歌曲查找，请重试。");return true;
        }
    }

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
    if ((Contains(s, "多少") || Contains(s, "多大") || Contains(s, "几") ||
         Contains(s, "现在") || Contains(s, "当前")) &&
        (Contains(s, "音量") || Contains(s, "声音")))
    {
        const int vol = Application::GetInstance().GetAudioService().GetVolume();
        char buf[48];
        std::snprintf(buf, sizeof(buf), "当前音量是 %d%%", vol);
        Reply(buf);
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

    /* Chat and the home popup share this handler. Answer here so both
     * show the level without waiting for the cloud tool. */
    if (!Contains(s, "打开") && !Contains(s, "开启") && !Contains(s, "启动") &&
        (Contains(s, "电量") || Contains(s, "电池") || Contains(s, "多少电") ||
         Contains(s, "充电了") || Contains(s, "在充电") || Contains(s, "充没充")))
    {
        ReplyBatteryLocal();
        return true;
    }

    /* Hi 家 devices. Bare 开灯/关灯 is left for GPIO48 / DeviceLink. */
    {
        char reply[192];
        if (SmartHome_VoiceCommand(s.c_str(), reply, sizeof(reply))) {
            write(1, "LOC_HOME\n", 9);
            Reply(reply);
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

    /* ---- Call a contact. The book is read now, not from a saved list. ---- */
    {
        std::string callee;
        const int kind = ExtractCallee(s, callee);
        if (kind != 0 && PlaceCall(callee, kind == 1))
            return true;
        if (kind == 0 && !Contains(s, "不要") && !Contains(s, "不用") &&
            !Contains(s, "不想") && !Contains(s, "别打") &&
            (Contains(s, "打电话") || Contains(s, "打个电话")))
        {
            Reply("要打给谁？");
            return true;
        }
    }

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
