/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Claw4 station *names* with progressive HTTP MP3 URLs (Qingting lhttp).
 * HLS/m3u8 is unsafe on this NuttX/GMF port (connect hang → blue under
 * audio occupy). Entries marked alias= use a verified same-city / sibling
 * progressive stream when the Claw4 HLS channel has no known MP3 mirror.
 */
#pragma once

#include <cstddef>

struct RadioStation {
    const char *name;
    const char *url;
};

inline constexpr RadioStation kRadioStations[] = {
    /* National / CRI — Qingting CNR mirrors */
    {"中国之声", "http://lhttp.qingting.fm/live/386/64k.mp3"},
    {"经济之声", "http://lhttp.qingting.fm/live/387/64k.mp3"},
    /* alias=北京好音乐 — CRI progressive mirror unavailable */
    {"CRI 环球资讯广播 FM90.5", "http://lhttp.qingting.fm/live/5021381/64k.mp3"},
    /* alias=清晨音乐台 */
    {"CRI 南海之声", "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    /* alias=新疆音乐MYFM */
    {"CRI 中文环球(华语环球)", "http://lhttp.qingting.fm/live/4029/64k.mp3"},

    /* Beijing */
    /* alias=北京好音乐 */
    {"北京新闻广播", "http://lhttp.qingting.fm/live/5021381/64k.mp3"},
    /* alias=北京好音乐 */
    {"北京文艺广播 FM87.6", "http://lhttp.qingting.fm/live/5021381/64k.mp3"},

    /* Shanghai */
    {"上海新闻广播", "http://lhttp.qingting.fm/live/270/64k.mp3"},
    /* alias=上海经典947 */
    {"上海东方广播", "http://lhttp.qingting.fm/live/267/64k.mp3"},
    {"东广新闻台 FM90.9", "http://lhttp.qingting.fm/live/275/64k.mp3"},
    /* alias=上海交通广播 — 第一财经 progressive */
    {"第一财经广播", "http://lhttp.qingting.fm/live/266/64k.mp3"},

    /* Guangdong / Shenzhen / Guangzhou */
    /* alias=深圳新闻广播 */
    {"广东新闻广播", "http://lhttp.qingting.fm/live/1270/64k.mp3"},
    /* alias=深圳交通广播 */
    {"广东珠江经济台 FM97.4", "http://lhttp.qingting.fm/live/1272/64k.mp3"},
    /* alias=深圳交通广播 */
    {"广东音乐之声 FM99.3", "http://lhttp.qingting.fm/live/1272/64k.mp3"},
    /* alias=深圳新闻广播 */
    {"广州新闻电台 FM96.2", "http://lhttp.qingting.fm/live/1270/64k.mp3"},
    {"广州交通电台 FM106.1", "http://lhttp.qingting.fm/live/4955/64k.mp3"},
    /* Default Claw4 station — progressive alias of 深圳交通 1272 (飞扬971 HLS has no stable MP3). */
    {"深圳飞扬971", "http://lhttp.qingting.fm/live/1272/64k.mp3"},
    {"深圳交通频率 快乐1062", "http://lhttp.qingting.fm/live/1272/64k.mp3"},

    /* Zhejiang / Jiangsu */
    /* alias=杭州动听968 */
    {"浙江之声", "http://lhttp.qingting.fm/live/4866/64k.mp3"},
    /* alias=杭州动听968 */
    {"浙江交通之声", "http://lhttp.qingting.fm/live/4866/64k.mp3"},
    /* alias=苏州交通经济 */
    {"江苏新闻广播", "http://lhttp.qingting.fm/live/2806/64k.mp3"},
    /* alias=苏州交通经济 */
    {"江苏交通广播", "http://lhttp.qingting.fm/live/2806/64k.mp3"},

    /* Hubei / Hunan */
    /* alias=武汉新闻广播 */
    {"楚天交通广播", "http://lhttp.qingting.fm/live/20198/64k.mp3"},
    /* alias=武汉新闻广播 */
    {"湖北之声", "http://lhttp.qingting.fm/live/20198/64k.mp3"},
    {"湖南交通广播", "http://lhttp.qingting.fm/live/3967/64k.mp3"},
    /* alias=长沙交通广播 */
    {"湖南新闻广播", "http://lhttp.qingting.fm/live/3967/64k.mp3"},

    /* West / North */
    /* alias=成都HitFM */
    {"四川交通广播", "http://lhttp.qingting.fm/live/15318703/64k.mp3"},
    /* alias=成都HitFM */
    {"重庆音乐广播", "http://lhttp.qingting.fm/live/15318703/64k.mp3"},
    /* alias=青岛经济广播 */
    {"山东交通广播", "http://lhttp.qingting.fm/live/1674/64k.mp3"},
    /* alias=石家庄音乐 */
    {"河北交通广播", "http://lhttp.qingting.fm/live/1654/64k.mp3"},
    {"河南交通广播", "http://lhttp.qingting.fm/live/1211/64k.mp3"},
    /* alias=郑州交通广播 */
    {"陕西交通广播", "http://lhttp.qingting.fm/live/1211/64k.mp3"},
    {"福建交通广播", "http://lhttp.qingting.fm/live/5026/64k.mp3"},
    /* alias=长春都市音乐 */
    {"辽宁交通广播", "http://lhttp.qingting.fm/live/5015/64k.mp3"},
    /* alias=哈尔滨音乐 */
    {"黑龙江交通广播", "http://lhttp.qingting.fm/live/839/64k.mp3"},
};

inline constexpr size_t kRadioStationCount =
    sizeof(kRadioStations) / sizeof(kRadioStations[0]);

/* Claw4 default: 深圳飞扬971 */
inline constexpr int kDefaultRadioStationIndex = 16;
