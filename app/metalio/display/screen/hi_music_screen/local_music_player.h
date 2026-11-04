#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class LocalMusicPlayer
{
public:
    enum class State { Idle, Loading, Playing, Paused, Error };
    static LocalMusicPlayer &Instance();
    bool ScanLibrary();
    size_t Count() const;
    std::string Title(size_t index) const;
    bool FindLocalTrack(const std::string &query,std::string &path,std::string &title);
    std::string CurrentTitle() const;
    std::string StatusText() const;
    State GetState() const;
    int CurrentIndex() const;
    void PlayIndex(int index);
    void PlayFile(const std::string &path, std::function<bool()> allowed = {});
    void RestorePlayback(std::function<bool()> allowed = {});
    void RememberPlayback();
    void PreviewFile(const std::string &path);
    void PlayRingtoneFile(const std::string &path);
    void Toggle();
    void SetPaused(bool paused);
    int PositionMillis() const;
    int DurationMillis() const;
    void SeekTo(int ms);
    void BeginScrub();
    void ScrubTo(int ms,int velocity);
    void EndScrub(bool resume);
    void SetVinylNoise(bool on);
    bool VinylNoise() const;
    void SetRepeat(int mode);
    int RepeatMode() const;
    void SetShuffle(bool on);
    bool Shuffle() const;
    void Next();
    void Previous();
    bool IsMuted() const;
    void ToggleMute();
    void Stop();
    void StopRingtonePlayback();
    void StopPreviewPlayback();
private:
    LocalMusicPlayer();
    ~LocalMusicPlayer();
    LocalMusicPlayer(const LocalMusicPlayer &) = delete;
    LocalMusicPlayer &operator=(const LocalMusicPlayer &) = delete;
    void RequestFile(const std::string &path,std::function<bool()> allowed,bool preview,bool ringtone,int start_ms = 0);
    struct Impl;
    Impl *impl_;
};
