#include "hi_task_policy.h"
#include "smart_home.h"
#include "settings.h"
#include "ha_client.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>

namespace {

SmartDevice s_devs[kSmartMaxDevices];
int s_count = 0;
bool s_ready = false;

void ClearDev(SmartDevice *d)
{
    std::memset(d, 0, sizeof(*d));
}

void CopyField(char *dst, size_t n, const char *src)
{
    if (dst == nullptr || n == 0)
        return;
    if (src == nullptr)
        src = "";
    std::snprintf(dst, n, "%s", src);
}

void SeedDefaults(void)
{
    struct Seed {
        const char *name;
        const char *room;
        int kind;
        int on;
        int level;
        int pinned;
    };
    static const Seed kSeed[] = {
        {"台灯Pro", "主卧", kSmartLight, 0, 40, 1},
        {"主卧吸顶灯", "主卧", kSmartLight, 1, 72, 1},
        {"客厅灯", "客厅", kSmartLight, 0, 55, 0},
        {"客厅窗帘", "客厅", kSmartCurtain, 0, 0, 0},
        {"客厅空调", "客厅", kSmartClimate, 0, 26, 0},
        {"书房插座", "书房", kSmartSwitch, 0, 0, 0},
    };
    s_count = 0;
    for (const Seed &s : kSeed) {
        if (s_count >= kSmartMaxDevices)
            break;
        SmartDevice &d = s_devs[s_count++];
        ClearDev(&d);
        CopyField(d.name, sizeof(d.name), s.name);
        CopyField(d.room, sizeof(d.room), s.room);
        d.kind = s.kind;
        d.on = s.on;
        d.level = s.level;
        d.pinned = s.pinned;
        d.home_on = 0; // Demo defaults are not a manually chosen home preset.
    }
}

void Persist(void)
{
    char blob[3200];
    size_t used = 0;
    blob[0] = '\0';
    for (int i = 0; i < s_count; ++i) {
        const SmartDevice &d = s_devs[i];
        char line[256];
        std::snprintf(line, sizeof(line), "%s|%s|%d|%d|%d|%d|%d|%s;",
                      d.name, d.room, d.kind, d.on ? 1 : 0, d.level,
                      d.pinned ? 1 : 0, d.home_on ? 1 : 0, d.entity);
        const size_t n = std::strlen(line);
        if (used + n >= sizeof(blob))
            break;
        std::memcpy(blob + used, line, n);
        used += n;
        blob[used] = '\0';
    }
    HaClient_SaveDevices(blob);
}

bool ParseLine(const char *line, SmartDevice *out)
{
    char name[32];
    char room[16];
    int kind = 0;
    int on = 0;
    int level = 0;
    int pinned = 0;
    if (std::sscanf(line, "%31[^|]|%15[^|]|%d|%d|%d|%d",
                    name, room, &kind, &on, &level, &pinned) != 6)
        return false;
    if (name[0] == '\0')
        return false;
    if (kind < kSmartLight || kind > kSmartFan)
        kind = kSmartLight;
    ClearDev(out);
    CopyField(out->name, sizeof(out->name), name);
    CopyField(out->room, sizeof(out->room), room);
    out->kind = kind;
    out->on = on ? 1 : 0;
    out->level = level;
    out->pinned = pinned ? 1 : 0;
    out->home_on = out->on;
    int home_on = 0;
    char entity[96] = {};
    const int fields = std::sscanf(line, "%*[^|]|%*[^|]|%*d|%*d|%*d|%*d|%d|%95[^|;\n]", &home_on, entity);
    if (fields >= 1) out->home_on = home_on ? 1 : 0;
    if (fields == 2 && HaClient_ValidEntity(entity))
        CopyField(out->entity, sizeof(out->entity), entity);
    int lo = 0;
    int hi = 100;
    SmartHome_LevelRange(kind, &lo, &hi);
    if (out->level < lo)
        out->level = lo;
    if (out->level > hi)
        out->level = hi;
    return true;
}

void Load(void)
{
    if (s_ready)
        return;
    s_ready = true;
    HaClient_Start();
    SeedDefaults();
    Settings s("smarthome", false);
    const std::string raw = s.GetString("devices", "");
    if (raw.empty())
        return;
    SmartDevice parsed[kSmartMaxDevices];
    int n = 0;
    const char *p = raw.c_str();
    while (*p != '\0' && n < kSmartMaxDevices) {
        char line[256];
        size_t i = 0;
        while (*p != '\0' && *p != '\n' && *p != ';' && i + 1 < sizeof(line))
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n' || *p == ';')
            ++p;
        if (line[0] == '\0')
            continue;
        if (ParseLine(line, &parsed[n]))
            ++n;
    }
    if (n <= 0)
        return;
    s_count = n;
    for (int i = 0; i < n; ++i)
        s_devs[i] = parsed[i];
    for (int i = 0; i < n; ++i)
        if (s_devs[i].entity[0]) HaClient_Bind(s_devs[i].entity);
}

}  // namespace

void SmartHome_EnsureLoaded(void)
{
    Load();
}

int SmartHome_Count(void)
{
    Load();
    return s_count;
}

const SmartDevice *SmartHome_At(int index)
{
    Load();
    if (index < 0 || index >= s_count)
        return nullptr;
    SmartDevice &dev = s_devs[index];
    HaDeviceState state;
    if (dev.entity[0] && HaClient_State(dev.entity, &state) && state.known) {
        dev.on = state.on ? 1 : 0;
        if (state.dimmable) dev.level = state.level;
    }
    return &dev;
}

int SmartHome_Pinned(int *out_index, int max_out)
{
    Load();
    if (out_index == nullptr || max_out <= 0)
        return 0;
    int n = 0;
    for (int i = 0; i < s_count && n < max_out; ++i) {
        if (s_devs[i].pinned)
            out_index[n++] = i;
    }
    return n;
}

void SmartHome_SetPower(int index, int on, bool persist)
{
    Load();
    if (index < 0 || index >= s_count)
        return;
    if (s_devs[index].entity[0]) {
        if (HaClient_Power(s_devs[index].entity, on != 0)) {
            s_devs[index].home_on = on ? 1 : 0;
            if (persist) Persist();
        }
        return;
    }
    s_devs[index].on = on ? 1 : 0;
    s_devs[index].home_on = s_devs[index].on;
    if (s_devs[index].kind == kSmartCurtain)
        s_devs[index].level = on ? (s_devs[index].level > 0 ? s_devs[index].level : 100)
                                 : 0;
    if (persist)
        Persist();
}

void SmartHome_SetLevel(int index, int level, bool persist)
{
    Load();
    if (index < 0 || index >= s_count)
        return;
    if (s_devs[index].entity[0]) {
        if (persist && HaClient_Level(s_devs[index].entity, level)) {
            s_devs[index].home_on = level > 0 ? 1 : 0;
            Persist();
        }
        return;
    }
    int lo = 0;
    int hi = 100;
    SmartHome_LevelRange(s_devs[index].kind, &lo, &hi);
    if (level < lo)
        level = lo;
    if (level > hi)
        level = hi;
    s_devs[index].level = level;
    if (s_devs[index].kind == kSmartCurtain)
        s_devs[index].on = level > 0 ? 1 : 0;
    else if ((s_devs[index].kind == kSmartLight || s_devs[index].kind == kSmartFan) && level > 0)
        s_devs[index].on = 1;
    s_devs[index].home_on = s_devs[index].on;
    if (persist)
        Persist();
}

void SmartHome_SetPinned(int index, int pinned)
{
    Load();
    if (index < 0 || index >= s_count)
        return;
    s_devs[index].pinned = pinned ? 1 : 0;
    Persist();
}

void SmartHome_SetName(int index, const char *name)
{
    Load();
    if (index < 0 || index >= s_count || name == nullptr || name[0] == '\0' || std::strpbrk(name, "|;\r\n"))
        return;
    CopyField(s_devs[index].name, sizeof(s_devs[index].name), name);
    Persist();
}

void SmartHome_SetRoom(int index, const char *room)
{
    Load();
    if (index < 0 || index >= s_count || room == nullptr || room[0] == '\0' || std::strpbrk(room, "|;\r\n"))
        return;
    CopyField(s_devs[index].room, sizeof(s_devs[index].room), room);
    Persist();
}

int SmartHome_Add(int kind)
{
    Load();
    if (s_count >= kSmartMaxDevices)
        return -1;
    if (kind < kSmartLight || kind > kSmartFan)
        kind = kSmartLight;
    SmartDevice &d = s_devs[s_count];
    ClearDev(&d);
    const char *name = "新灯";
    int level = 60;
    if (kind == kSmartSwitch) {
        name = "新开关";
        level = 0;
    } else if (kind == kSmartCurtain) {
        name = "新窗帘";
        level = 0;
    } else if (kind == kSmartClimate) {
        name = "新空调";
        level = 26;
    } else if (kind == kSmartFan) {
        name = "新风扇";
        level = 0;
    }
    CopyField(d.name, sizeof(d.name), name);
    CopyField(d.room, sizeof(d.room), "客厅");
    d.kind = kind;
    d.on = 0;
    d.level = level;
    d.pinned = 0;
    const int index = s_count++;
    Persist();
    return index;
}

bool SmartHome_Remove(int index)
{
    Load();
    if (index < 0 || index >= s_count)
        return false;
    if (s_devs[index].entity[0]) HaClient_Unbind(s_devs[index].entity);
    for (int i = index; i + 1 < s_count; ++i)
        s_devs[i] = s_devs[i + 1];
    --s_count;
    ClearDev(&s_devs[s_count]);
    Persist();
    return true;
}

bool SmartHome_Bind(int index, const char *entity)
{
    Load();
    if (index < 0 || index >= s_count || !entity) return false;
    for (int i = 0; i < s_count; ++i)
        if (i != index && entity[0] && !std::strcmp(s_devs[i].entity, entity)) return false;
    if (entity[0]) {
        if (!HaClient_Bind(entity, s_devs[index].entity)) return false;
        s_devs[index].kind = !std::strncmp(entity, "light.", 6) ? kSmartLight : kSmartSwitch;
    } else if (s_devs[index].entity[0]) HaClient_Unbind(s_devs[index].entity);
    CopyField(s_devs[index].entity, sizeof(s_devs[index].entity), entity);
    Persist();
    return true;
}

bool SmartHome_VoiceEnabled(void)
{
    Settings s("assistant", false);
    return s.GetInt("voice", 1) != 0;
}

void SmartHome_SetVoiceEnabled(bool on)
{
    Settings s("assistant", true);
    s.SetInt("voice", on ? 1 : 0);
}

namespace {

int s_at_home = -1;

void *PersistAtHome(void *arg)
{
    const int at_home = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings s("smarthome", true);
    s.SetInt("athome", at_home);
    return nullptr;
}

bool DeviceIsOn(const SmartDevice &dev)
{
    return dev.kind == kSmartCurtain ? dev.level > 0 : dev.on != 0;
}

int s_restore_level[kSmartMaxDevices];

void PresenceApply(int index, bool on)
{
    SmartDevice &dev = s_devs[index];
    if (!on && dev.kind == kSmartCurtain && dev.level > 0)
        s_restore_level[index] = dev.level;
    if (dev.entity[0])
        HaClient_Power(dev.entity, on);
    dev.on = on ? 1 : 0;
    if (dev.kind == kSmartCurtain) {
        if (on) {
            const int remembered = s_restore_level[index];
            dev.level = remembered > 0 ? remembered : 100;
        } else {
            dev.level = 0;
        }
    }
}

}  // namespace

bool SmartHome_AtHome(void)
{
    if (s_at_home < 0) {
        Settings s("smarthome", false);
        s_at_home = s.GetInt("athome", 1) != 0 ? 1 : 0;
    }
    return s_at_home != 0;
}

void SmartHome_SetAtHome(bool at_home)
{
    Load();
    bool dirty = false;
    for (int i = 0; i < s_count; ++i) {
        SmartDevice &dev = s_devs[i];
        const bool opened = DeviceIsOn(dev);
        if (!at_home) {
            /* Only switches that are actually on. Remember them so 回家 can
             * restore that state. An off device stays untouched unless the
             * user has manually turned it on (home_on). */
            if (!opened)
                continue;
            dev.home_on = 1;
            PresenceApply(i, false);
            dirty = true;
        } else if (dev.home_on && !opened) {
            PresenceApply(i, true);
            dirty = true;
        }
    }
    if (dirty)
        Persist();
    s_at_home = at_home ? 1 : 0;
    /* kvflash erase on the LVGL thread blanks the panel. */
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 16384);
    const auto flag = reinterpret_cast<void *>(static_cast<intptr_t>(s_at_home));
    /* Never fall back onto the interface thread. A failed spawn leaves the
     * choice in RAM; the next toggle persists it. */
    (void)hi_task_create_legacy(&tid, &attr, PersistAtHome, flag);
    pthread_attr_destroy(&attr);
}

const char *SmartHome_KindName(int kind)
{
    switch (kind) {
    case kSmartSwitch:
        return "开关";
    case kSmartCurtain:
        return "窗帘";
    case kSmartClimate:
        return "空调";
    case kSmartFan:
        return "风扇";
    default:
        return "灯";
    }
}

void SmartHome_LevelRange(int kind, int *min_out, int *max_out)
{
    int lo = 0;
    int hi = 100;
    if (kind == kSmartClimate) {
        lo = 16;
        hi = 30;
    }
    if (min_out != nullptr)
        *min_out = lo;
    if (max_out != nullptr)
        *max_out = hi;
}

namespace {

bool Hear(const char *text, const char *word)
{
    return text != nullptr && word != nullptr && std::strstr(text, word) != nullptr;
}

bool HearNumber(const char *text, int *out)
{
    if (text == nullptr || out == nullptr)
        return false;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            continue;
        int value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            ++p;
        }
        *out = value;
        return true;
    }
    return false;
}

bool DeviceOpen(const SmartDevice *dev)
{
    if (dev == nullptr)
        return false;
    if (dev->kind == kSmartCurtain)
        return dev->level > 0;
    return dev->on != 0;
}

void CopyReply(char *reply, size_t reply_n, const char *text)
{
    if (reply == nullptr || reply_n == 0)
        return;
    std::snprintf(reply, reply_n, "%s", text != nullptr ? text : "");
}

}  // namespace

bool SmartHome_VoiceCommand(const char *text, char *reply, size_t reply_n)
{
    if (text == nullptr || text[0] == '\0' || reply == nullptr || reply_n == 0)
        return false;
    reply[0] = '\0';
    SmartHome_EnsureLoaded();

    if (Hear(text, "打开") &&
        (Hear(text, "Hi家") || Hear(text, "HI家") || Hear(text, "智能家居")))
        return false;

    if (Hear(text, "离家") || Hear(text, "出门了")) {
        SmartHome_SetAtHome(false);
        CopyReply(reply, reply_n, "已离家，正在开着的设备已关闭。");
        return true;
    }
    if (Hear(text, "回家了") || Hear(text, "到家了") || Hear(text, "我回来了")) {
        SmartHome_SetAtHome(true);
        CopyReply(reply, reply_n, "已回家，之前手动打开的设备已恢复。");
        return true;
    }

    const char *rooms[] = {"主卧", "客厅", "书房", "餐厅", "阳台", "次卧", nullptr};
    const char *room = nullptr;
    for (int i = 0; rooms[i] != nullptr; ++i) {
        if (Hear(text, rooms[i]))
            room = rooms[i];
    }

    int kind = -1;
    if (Hear(text, "窗帘"))
        kind = kSmartCurtain;
    else if (Hear(text, "空调"))
        kind = kSmartClimate;
    else if (Hear(text, "风扇") || Hear(text, "电扇"))
        kind = kSmartFan;
    else if (Hear(text, "插座") || Hear(text, "开关"))
        kind = kSmartSwitch;
    else if (Hear(text, "灯"))
        kind = kSmartLight;

    int hits[kSmartMaxDevices];
    int hit_count = 0;
    const int count = SmartHome_Count();
    for (int i = 0; i < count; ++i) {
        const SmartDevice *dev = SmartHome_At(i);
        if (dev == nullptr || dev->name[0] == '\0' || std::strlen(dev->name) < 6)
            continue;
        if (!Hear(text, dev->name))
            continue;
        if (room != nullptr && std::strcmp(dev->room, room) != 0)
            continue;
        hits[hit_count++] = i;
    }
    if (hit_count == 0 && kind >= 0) {
        const bool bare_lamp = kind == kSmartLight && room == nullptr &&
                               !Hear(text, "所有") && !Hear(text, "全部") &&
                               !Hear(text, "都");
        if (bare_lamp)
            return false;
        for (int i = 0; i < count; ++i) {
            const SmartDevice *dev = SmartHome_At(i);
            if (dev == nullptr || dev->kind != kind)
                continue;
            if (room != nullptr && std::strcmp(dev->room, room) != 0)
                continue;
            hits[hit_count++] = i;
        }
    }
    if (kind < 0 && hit_count == 0)
        return false;
    if (hit_count == 0) {
        CopyReply(reply, reply_n, "Hi 家里没有找到这个设备。");
        return true;
    }

    const bool query = (Hear(text, "状态") || Hear(text, "开了吗") ||
                        Hear(text, "关了吗") || Hear(text, "多少度") ||
                        Hear(text, "几度")) &&
                       !Hear(text, "打开") && !Hear(text, "关闭") &&
                       !Hear(text, "关掉") && !Hear(text, "调到");
    const bool turn_off = Hear(text, "关掉") || Hear(text, "关闭") ||
                          Hear(text, "关上") || Hear(text, "关了") ||
                          Hear(text, "拉上") || Hear(text, "关灯") ||
                          Hear(text, "关窗帘") || Hear(text, "关空调") ||
                          Hear(text, "关风扇");
    const bool turn_on = !turn_off &&
                         (Hear(text, "打开") || Hear(text, "开启") ||
                          Hear(text, "拉开") || Hear(text, "开灯") ||
                          Hear(text, "开窗帘") || Hear(text, "开空调") ||
                          Hear(text, "开风扇"));
    int number = 0;
    const bool has_number = HearNumber(text, &number);
    const bool set_level = has_number &&
                           (Hear(text, "调到") || Hear(text, "调成") ||
                            Hear(text, "设为") || Hear(text, "设成") ||
                            Hear(text, "开到") || Hear(text, "亮度") ||
                            Hear(text, "风速") || Hear(text, "温度") ||
                            Hear(text, "度"));
    int delta = 0;
    if (Hear(text, "亮一点") || Hear(text, "调亮") || Hear(text, "热一点"))
        delta = Hear(text, "热一点") ? 1 : 15;
    else if (Hear(text, "暗一点") || Hear(text, "调暗") || Hear(text, "冷一点"))
        delta = Hear(text, "冷一点") ? -1 : -15;
    else if (Hear(text, "大一点"))
        delta = 15;
    else if (Hear(text, "小一点"))
        delta = -15;

    if (!query && !turn_off && !turn_on && !set_level && delta == 0) {
        CopyReply(reply, reply_n, "要打开、关闭，还是调节这个设备？");
        return true;
    }

    if (query) {
        char body[160];
        body[0] = '\0';
        for (int i = 0; i < hit_count && i < 3; ++i) {
            const SmartDevice *dev = SmartHome_At(hits[i]);
            if (dev == nullptr)
                continue;
            char piece[48];
            if (dev->kind == kSmartClimate)
                std::snprintf(piece, sizeof(piece), "%s%s，%d°C",
                              dev->name, DeviceOpen(dev) ? "开着" : "关着",
                              dev->level);
            else
                std::snprintf(piece, sizeof(piece), "%s%s",
                              dev->name, DeviceOpen(dev) ? "开着" : "关着");
            if (body[0] != '\0')
                std::strncat(body, "，", sizeof(body) - std::strlen(body) - 1);
            std::strncat(body, piece, sizeof(body) - std::strlen(body) - 1);
        }
        std::snprintf(reply, reply_n, "%s。", body);
        return true;
    }

    for (int i = 0; i < hit_count; ++i) {
        const SmartDevice *dev = SmartHome_At(hits[i]);
        if (dev == nullptr)
            continue;
        if (set_level)
            SmartHome_SetLevel(hits[i], number, true);
        else if (delta != 0)
            SmartHome_SetLevel(hits[i], dev->level + delta, true);
        else if (turn_off)
            SmartHome_SetPower(hits[i], 0, true);
        else if (turn_on)
            SmartHome_SetPower(hits[i], 1, true);
    }

    const SmartDevice *first = SmartHome_At(hits[0]);
    const char *name = first != nullptr ? first->name : "设备";
    if (hit_count == 1 && set_level && first != nullptr &&
        first->kind == kSmartClimate)
        std::snprintf(reply, reply_n, "已将%s调到 %d°C。", name, number);
    else if (hit_count == 1 && (set_level || delta != 0))
        std::snprintf(reply, reply_n, "已调节%s。", name);
    else if (hit_count == 1 && turn_off)
        std::snprintf(reply, reply_n, "已关闭%s。", name);
    else if (hit_count == 1 && turn_on)
        std::snprintf(reply, reply_n, "已打开%s。", name);
    else if (turn_off)
        std::snprintf(reply, reply_n, "已关闭 %d 个 Hi 家设备。", hit_count);
    else if (turn_on)
        std::snprintf(reply, reply_n, "已打开 %d 个 Hi 家设备。", hit_count);
    else
        std::snprintf(reply, reply_n, "已调节 %d 个 Hi 家设备。", hit_count);
    return true;
}

const char *SmartHome_StatusText(const SmartDevice *dev)
{
    if (dev == nullptr)
        return "已关";
    if (dev->entity[0]) {
        HaDeviceState state;
        if (!HaClient_State(dev->entity, &state)) return "未连接";
        if (state.pending) return "执行中";
        if (state.error != kHaOk) return "控制失败";
        if (!state.available || !state.known) return "离线";
    }
    if (dev->kind == kSmartCurtain)
        return dev->level > 0 ? "已开" : "已关";
    return dev->on ? "已开" : "已关";
}
