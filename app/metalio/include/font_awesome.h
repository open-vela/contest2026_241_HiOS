/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Font Awesome icon definitions for LVGL.
 * Ported from MetalioClaw4 (xingzhi-esp32 dependency).
 * These are Unicode code points in the LVGL symbol font range (0xF000+).
 */

#ifndef FONT_AWESOME_H
#define FONT_AWESOME_H

/* Volume icons */
#define FONT_AWESOME_VOLUME_XMARK      "\xEF\x9A\xA9"  /*  */
#define FONT_AWESOME_VOLUME_HIGH       "\xEF\x80\xA8"  /*  */
#define FONT_AWESOME_VOLUME_LOW        "\xEF\x80\xA7"  /*  */
#define FONT_AWESOME_VOLUME_OFF        "\xEF\x80\xA6"  /*  */

/* Battery icons */
#define FONT_AWESOME_BATTERY_EMPTY     "\xEF\x89\x84"  /*  */
#define FONT_AWESOME_BATTERY_QUARTER   "\xEF\x89\x83"  /*  */
#define FONT_AWESOME_BATTERY_HALF      "\xEF\x89\x82"  /*  */
#define FONT_AWESOME_BATTERY_THREE_QUARTERS "\xEF\x89\x81" /*  */
#define FONT_AWESOME_BATTERY_FULL      "\xEF\x89\x80"  /*  */
#define FONT_AWESOME_BATTERY_BOLT      "\xEF\x8D\xB6"  /* U+F376 battery-bolt */
#define FONT_AWESOME_BATTERY_SLASH     "\xEF\x8D\xB7"  /* U+F377 battery-slash */

/* Network icons */
#define FONT_AWESOME_WIFI              "\xEF\x87\xAB"  /*  */
#define FONT_AWESOME_WIFI_WEAK         "\xEF\x9A\xAB"  /*  */
#define FONT_AWESOME_WIFI_FAIR         "\xEF\x9A\xAC"  /*  */
#define FONT_AWESOME_WIFI_STRONG       "\xEF\x9A\xAD"  /*  */
#define FONT_AWESOME_WIFI_SLASH        "\xEF\x9A\xAA"  /*  */
#define FONT_AWESOME_SIGNAL            "\xEF\x80\x92"  /*  */
#define FONT_AWESOME_SIGNAL_SLASH      "\xEF\x9A\xB9"  /*  */

/* Connectivity icons */
#define FONT_AWESOME_BLUETOOTH         "\xEF\x8A\x93"  /*  */
#define FONT_AWESOME_BLUETOOTH_B       "\xEF\x8A\x94"  /*  */
#define FONT_AWESOME_SATELLITE         "\xEF\x81\x9E"  /*  */
#define FONT_AWESOME_SATELLITE_DISH    "\xEF\x9F\x80"  /*  */

/* General icons */
#define FONT_AWESOME_GEAR              "\xEF\x80\x93"  /*  */
#define FONT_AWESOME_GEAR_FULL         "\xEF\x82\x85"  /*  */
#define FONT_AWESOME_POWER_OFF         "\xEF\x80\x91"  /*  */
#define FONT_AWESOME_CIRCLE_INFO       "\xEF\x81\x9A"  /*  */
#define FONT_AWESOME_TRIANGLE_EXCLAMATION "\xEF\x81\xB1" /*  */
#define FONT_AWESOME_CIRCLE_CHECK      "\xEF\x81\x98"  /*  */
#define FONT_AWESOME_CIRCLE_XMARK      "\xEF\x81\x97"  /*  */
#define FONT_AWESOME_MICROPHONE        "\xEF\x84\xB0"  /*  */
#define FONT_AWESOME_MICROPHONE_SLASH  "\xEF\x84\xB1"  /*  */
#define FONT_AWESOME_PLAY              "\xEF\x81\x8B"  /*  */
#define FONT_AWESOME_PAUSE             "\xEF\x81\x8C"  /*  */
#define FONT_AWESOME_STOP              "\xEF\x81\x8D"  /*  */
#define FONT_AWESOME_ROTATE            "\xEF\x80\x9E"  /*  */
#define FONT_AWESOME_SUN               "\xEF\x86\x85"  /*  */
#define FONT_AWESOME_MOON              "\xEF\x86\x86"  /*  */
#define FONT_AWESOME_LOCK              "\xEF\x80\xA3"  /*  */
#define FONT_AWESOME_LOCK_OPEN         "\xEF\x8F\x81"  /*  */
#define FONT_AWESOME_LIST              "\xEF\x80\x8B"  /*  */
#define FONT_AWESOME_HOUSE             "\xEF\x80\x95"  /*  */
#define FONT_AWESOME_BACKWARD          "\xEF\x81\x8A"  /*  */
#define FONT_AWESOME_FORWARD           "\xEF\x81\x8E"  /*  */
#define FONT_AWESOME_ARROW_LEFT        "\xEF\x81\xA0"  /*  */
#define FONT_AWESOME_ARROW_RIGHT       "\xEF\x81\xA1"  /*  */
#define FONT_AWESOME_BARS              "\xEF\x83\x89"  /*  */
#define FONT_AWESOME_CALENDAR          "\xEF\x84\xB3"  /*  */
#define FONT_AWESOME_CLOCK             "\xEF\x80\x97"  /*  */
#define FONT_AWESOME_CAMERA            "\xEF\x80\xB0"  /*  */
#define FONT_AWESOME_IMAGE             "\xEF\x80\xBE"  /*  */
#define FONT_AWESOME_MAP               "\xEF\x89\xB9"  /*  */
#define FONT_AWESOME_MUSIC             "\xEF\x80\x81"  /*  */
#define FONT_AWESOME_PHONE             "\xEF\x82\x95"  /*  */
#define FONT_AWESOME Calculator        "\xEF\x87\xAC"  /*  */
#define FONT_AWESOME_CALCULATOR        "\xEF\x87\xAC"  /*  */
#define FONT_AWESOME_GAMEPAD           "\xEF\x84\x9B"  /*  */
#define FONT_AWESOME_THERMOMETER       "\xEF\x92\x91"  /*  */
#define FONT_AWESOME_MAGNET            "\xEF\x81\xB6"  /*  */
#define FONT_AWESOME_SD_CARD           "\xEF\x9F\x82"  /*  */
#define FONT_AWESOME_USB               "\xEF\x8A\x87"  /*  */
#define FONT_AWESOME_BROADCAST_TOWER   "\xEF\x94\x99"  /*  */
#define FONT_AWESOME_EYE               "\xEF\x81\xAE"  /*  */
#define FONT_AWESOME_EYE_SLASH         "\xEF\x81\xB0"  /*  */
#define FONT_AWESOME_HEART             "\xEF\x80\x84"  /*  */
#define FONT_AWESOME_STAR              "\xEF\x80\x85"  /*  */
#define FONT_AWESOME_BELL              "\xEF\x83\xB3"  /*  */
#define FONT_AWESOME_BELL_SLASH        "\xEF\x87\xBE"  /*  */
#define FONT_AWESOME_SEARCH            "\xEF\x80\x82"  /*  */
#define FONT_AWESOME_TRASH             "\xEF\x87\xB8"  /*  */
#define FONT_AWESOME_DOWNLOAD          "\xEF\x80\x99"  /*  */
#define FONT_AWESOME_UPLOAD            "\xEF\x82\x93"  /*  */
#define FONT_AWESOME_REFRESH           "\xEF\x80\xA1"  /*  */
#define FONT_AWESOME_WRENCH            "\xEF\x82\xAD"  /*  */
#define FONT_AWESOME_SLIDERS           "\xEF\x87\x9E"  /*  */
#define FONT_AWESOME_GLOBE             "\xEF\x82\xAC"  /*  */
#define FONT_AWESOME_LANGUAGE          "\xEF\x86\x9A"  /*  */
#define FONT_AWESOME_PALETTE           "\xEF\x94\xBF"  /*  */
#define FONT_AWESOME_DROPLET           "\xEF\x81\x83"  /*  */
#define FONT_AWESOME_DROPLET_SLASH     "\xEF\x97\x87"  /*  */
#define FONT_AWESOME_CLOUD             "\xEF\x83\x82"  /*  */
#define FONT_AWESOME_CLOUD_BOLT        "\xEF\xAD\xAD"  /* ﭭ */
#define FONT_AWESOME_SNOWFLAKE         "\xEF\x8B\x9C"  /*  */
#define FONT_AWESOME_WIND              "\xEF\x9F\xAE"  /*  */
#define FONT_AWESOME_LOCATION_DOT      "\xEF\x8F\x85"  /*  */
#define FONT_AWESOME_LOCATION_ARROW    "\xEF\x84\x97"  /*  */
#define FONT_AWESOME_COMPASS           "\xEF\x85\x8E"  /*  */
#define FONT_AWESOME_MESSAGE           "\xEF\x89\xBA"  /*  */
#define FONT_AWESOME_COMMENTS          "\xEF\x82\x86"  /*  */
#define FONT_AWESOME_ROBOT             "\xEF\x95\x84"  /*  */
#define FONT_AWESOME_USER              "\xEF\x80\x87"  /*  */
#define FONT_AWESOME_USERS             "\xEF\x83\x80"  /*  */
#define FONT_AWESOME_CIRCLE_USER       "\xEF\x8A\xBD"  /*  */
#define FONT_AWESOME_ADDRESS_BOOK      "\xEF\x8A\xB9"  /*  */
#define FONT_AWESOME_ADDRESS_CARD      "\xEF\x8A\xBB"  /*  */
#define FONT_AWESOME_QRCODE            "\xEF\x80\xa9"  /*  */
#define FONT_AWESOME_BOLT              "\xEF\x83\xA7"  /*  */
#define FONT_AWESOME_PLUG              "\xEF\x87\xA6"  /*  */
#define FONT_AWESOME_PLUG_CIRCLE_XMARK "\xEF\x95\x97"  /*  */

/* Spinner / loading */
#define FONT_AWESOME_SPINNER           "\xEF\x84\x90"  /*  */
#define FONT_AWESOME_CIRCLE_NOTCH      "\xEF\x87\xBD"  /*  */

/* App-specific */
#define FONT_AWESOME_WAVEFORM          "\xEF\x87\xB8"  /*  */
#define FONT_AWESOME_GRIP              "\xEF\x96\x8D"  /*  */
#define FONT_AWESOME_BACKSPACE         "\xEF\x95\x9A"  /*  */

#endif /* FONT_AWESOME_H */
