#pragma once

#include <string>

/* Thread-safe HA transport. UI owns SmartDevice; worker never touches LVGL. */
struct HaDeviceState {
    bool available = false;
    bool known = false;
    bool on = false;
    bool dimmable = false;
    bool pending = false;
    int level = 0;
    int error = 0;
};

enum HaError { kHaOk, kHaNetwork, kHaAuth, kHaMissing, kHaResponse,
               kHaTimeout, kHaNotConfigured, kHaUnsupported };

void HaClient_Start();
std::string HaClient_Url();
/* For the masked connection editor only; never display or log in plaintext. */
std::string HaClient_Token();
/* Empty token retains the saved token. */
bool HaClient_Configure(const std::string &url, const std::string &token);
bool HaClient_Configured();
bool HaClient_Bind(const std::string &entity, const std::string &replace = "");
void HaClient_Unbind(const std::string &entity);
bool HaClient_State(const std::string &entity, HaDeviceState *out);
bool HaClient_Power(const std::string &entity, bool on);
bool HaClient_Level(const std::string &entity, int percent);
bool HaClient_ValidEntity(const std::string &entity);
/* Persist a UI snapshot on the HA worker, away from the display thread. */
void HaClient_SaveDevices(const std::string &blob);
