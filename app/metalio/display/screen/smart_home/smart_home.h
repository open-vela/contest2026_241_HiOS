/*
 * Local smart-home model shared by the desktop cards, settings, and Home.
 * Devices stay on this panel (no Mi Home cloud). Voice assistant on/off
 * is stored beside them so settings and wake gating read one place.
 */

#pragma once

#include <stddef.h>

enum SmartKind {
    kSmartLight = 0,
    kSmartSwitch = 1,
    kSmartCurtain = 2,
    kSmartClimate = 3,
    kSmartFan = 4,
};

struct SmartDevice {
    char name[32];
    char room[16];
    int kind;
    int on;
    int level;
    int pinned;
    int home_on; /* Last manually selected power, independent of presence. */
    char entity[96];
};

constexpr int kSmartMaxDevices = 12;

void SmartHome_EnsureLoaded(void);
int SmartHome_Count(void);
const SmartDevice *SmartHome_At(int index);
int SmartHome_Pinned(int *out_index, int max_out);

void SmartHome_SetPower(int index, int on, bool persist);
void SmartHome_SetLevel(int index, int level, bool persist);
void SmartHome_SetPinned(int index, int pinned);
void SmartHome_SetName(int index, const char *name);
void SmartHome_SetRoom(int index, const char *room);
int SmartHome_Add(int kind);
bool SmartHome_Remove(int index);
bool SmartHome_Bind(int index, const char *entity);

bool SmartHome_VoiceEnabled(void);
void SmartHome_SetVoiceEnabled(bool on);

/* Desktop presence. true = 回家, false = 离家. */
bool SmartHome_AtHome(void);
void SmartHome_SetAtHome(bool at_home);

/* Chat and the home popup share this. Bare 开灯/关灯 stays on GPIO48.
 * Returns false when the sentence is not an Hi 家 command. */
bool SmartHome_VoiceCommand(const char *text, char *reply, size_t reply_n);

const char *SmartHome_KindName(int kind);
/* Slider range for the kind. Climate is Celsius; others are percent. */
void SmartHome_LevelRange(int kind, int *min_out, int *max_out);
const char *SmartHome_StatusText(const SmartDevice *dev);
