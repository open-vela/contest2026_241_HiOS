/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Live internet radio via GMF: HTTP/HTTPS HLS (m3u8) and progressive MP3.
 * TLS uses the NuttX mbedTLS client in nuttx_http_adapter (verify none).
 */
#pragma once

#include <cstddef>

struct RadioStation {
    const char *name;
    const char *url;
    const char *hint;
    const char *fallback_url = nullptr;
};

inline constexpr RadioStation kRadioStations[] = {
    /* CNR — MPEG-TS HLS */
    {"中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8", "CNR"},
    {"经济之声", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", "CNR"},
    {"音乐之声", "http://satellitepull.cnr.cn/live/wxyyzs/playlist.m3u8", "CNR"},
    {"神州之声", "http://satellitepull.cnr.cn/live/wxszzs/playlist.m3u8", "CNR"},
    {"经典音乐广播", "http://satellitepull.cnr.cn/live/wxdszs/playlist.m3u8", "CNR"},
    /* CRI */
    {"CRI 环球资讯", "https://sk.cri.cn/905.m3u8", "FM 90.5"},
    {"CRI 英语资讯", "http://sk.cri.cn/am846.m3u8", "China Plus"},
    {"CRI 中文环球", "http://sk.cri.cn/hyhq.m3u8", "中文"},
    /* 央广 / 蜻蜓 MP3 */
    {"中国之声(蜻蜓)", "https://lhttp.qtfm.cn/live/15318317/64k.mp3", "CNR"},
    {"广东音乐之声", "https://lhttp.qtfm.cn/live/1260/64k.mp3", "广东"},
    {"江苏经典流行", "https://lhttp.qtfm.cn/live/4938/64k.mp3", "江苏"},
    /* 凤凰 — HLS */
    {"凤凰卫视资讯台",
     "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8", "凤凰"},
    {"凤凰卫视中文台",
     "http://playtv-live.ifeng.com/live/06OLEGEGM4G_audio.m3u8", "凤凰"},
    /* CCTV 电视伴音 — HLS */
    {"CCTV-13 新闻伴音",
     "https://piccpndali.v.myalicdn.com/audio/cctv13_2.m3u8", "CCTV"},
    {"CCTV-4 中文国际",
     "https://piccpndali.v.myalicdn.com/audio/cctv4_2.m3u8", "CCTV"},
    /* Same Qingting channels, using their verified continuous MP3 feeds. */
    {"上海新闻广播",
     "https://lhttp.qtfm.cn/live/270/64k.mp3", "上海"},
    {"第一财经",
     "https://lhttp.qtfm.cn/live/266/64k.mp3", "上海"},
    {"北京新闻广播",
     "https://lhttp.qtfm.cn/live/339/64k.mp3", "北京"},
    {"珠江经济台",
     "https://lhttp.qtfm.cn/live/1271/64k.mp3", "FM 97.4"},
    {"广东音乐(HLS)",
     "https://lhttp.qtfm.cn/live/1273/64k.mp3", "FM 99.3"},
    {"浙江之声",
     "https://lhttp.qtfm.cn/live/4864/64k.mp3", "浙江"},
    {"江苏新闻广播",
     "https://lhttp.qtfm.cn/live/2804/64k.mp3", "江苏"},
    {"湖南交通广播",
     "https://lhttp.qtfm.cn/live/3967/64k.mp3", "湖南"},
    {"河南交通广播",
     "https://lhttp.qtfm.cn/live/1211/64k.mp3", "河南"},
    {"深圳新闻广播",
     "https://lhttp.qtfm.cn/live/1278/64k.mp3", "深圳"},
    {"广州交通电台",
     "https://lhttp.qtfm.cn/live/4955/64k.mp3", "FM 106.1"},
    {"华语金曲500首", "https://lhttp.qtfm.cn/live/5022308/64k.mp3", "音乐"},
    {"广东珠江之声", "https://lhttp.qtfm.cn/live/470/64k.mp3", "广东"},
    {"北京文艺广播", "https://lhttp.qtfm.cn/live/333/64k.mp3", "FM 87.6"},
    {"上海东方广播", "https://lhttp.qtfm.cn/live/267/64k.mp3", "上海"},
    {"广东新闻广播",
     "http://satellitepull.cnr.cn/live/wxgdxwgb/playlist.m3u8", "广东"},
    {"深圳飞扬971", "http://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8", "FM 97.1", "https://lhttp.qtfm.cn/live/1271/64k.mp3"},
};

inline constexpr size_t kRadioStationCount =
    sizeof(kRadioStations) / sizeof(kRadioStations[0]);

/* Default: CRI HTTPS HLS — exercises TLS + TS/AAC path. */
inline constexpr int kDefaultRadioStationIndex = 5;
