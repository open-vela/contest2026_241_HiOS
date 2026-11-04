#pragma once
#include "weather_data.h"
#include <mutex>

// Network I/O belongs only to the worker. Views obtain bounded value copies.
class WeatherService {
public:
    static constexpr const char* kDefaultDistrictId="440306";
    struct Snapshot {
        WeatherDistrictData data;
        std::string district_id, error;
        bool busy=false;
        unsigned generation=0;
    };
    static WeatherService& Instance();
    std::string DistrictId();
    bool SetDistrictId(const std::string& id);
    WeatherDistrictData Cached();
    Snapshot GetSnapshot();
    /* Stack-only view for the idle panel. Copying Snapshot pulls every
     * forecast vector onto the interface thread; that heap churn is what
     * turns a static smart screen solid blue. */
    struct PanelFields {
        char description[64];
        char temperature[8];
        char place_top[64];
        char place_bottom[64];
        unsigned generation;
    };
    void CopyPanel(PanelFields* out);
    void Request(bool force=false);
private:
    WeatherService();
    static void* Worker(void*);
    void Run();
    std::mutex lock_;
    Snapshot state_;
    bool started_=false, requested_=false;
    uint64_t attempt_ms_=0, success_ms_=0;
};
