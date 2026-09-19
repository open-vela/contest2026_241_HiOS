############################################################################
# apps/metalio/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = metalio
PRIORITY  = $(CONFIG_METALIO_APP_PRIORITY)
STACKSIZE = $(CONFIG_METALIO_APP_STACKSIZE)
MODULE    = $(CONFIG_METALIO_APP)

MAINSRC = metalio_main.c

# Root-level application modules (ported from MetalioClaw4 main/application.cc,
# main/openclaw_client and the openvela-specific network service).
CXXSRCS  = application.cxx openclaw_client.cxx network_service.cxx ui_home.cxx
CXXSRCS += chat_local_cmd.cxx

# ESP-IDF shim layer
CXXSRCS += port/board_shim.cxx

# Vendored cJSON library (single-file, MIT licensed)
CSRCS += port/cjson/cJSON.c

# Locale stubs for libstdc++ (NuttX libc lacks setlocale/strxfrm/etc.)
CSRCS += port/locale_stubs.c

# 64-bit atomic emulation for rv32imac (no libatomic.a, no native 64-bit atomics)
CSRCS += port/atomic8_emulation.c

# __cxa_guard_acquire / __cxa_guard_release fix — replaces libsupc++ versions
# that use pthread_mutex / pthread_cond (may hang during early startup on NuttX)
CSRCS += port/cxa_guard_fix.c

# Assets and API endpoints
CXXSRCS += assets.cxx

# Embedded UI icon bundle + "A:" filesystem driver (MetalioClaw4 icons)
CSRCS += display/icons_bundle.c display/icons_bundle_extra.c display/lvgl_assets_fs.c

# LED control (WS2812 single LED, PWM GPIO LED, WS2812 circular strip)
CXXSRCS += led/single_led.cxx led/gpio_led.cxx led/circular_strip.cxx

# Board implementation (metalio-claw-4 application-level init)
CXXSRCS += boards/metalio_claw_4_board.cxx

# SimpleUart — /dev/ttyS1 AT-command channel for the BT audio codec
CXXSRCS += boards/simple_uart.cxx

# Application framework (settings, system_info, device state events, i18n)
CXXSRCS += system/settings.cxx system/system_info.cxx system/device_state_event.cxx
CXXSRCS += i18n/i18n.cxx

# Display framework (base class, LVGL display, theme, image, font)
CXXSRCS += display/display.cxx display/lvgl_display.cxx display/lvgl_theme.cxx \
          display/theme_manager.cxx \
          display/lvgl_image.cxx display/lvgl_font.cxx display/nuttx_lvgl_display.cxx \
          display/stub_fonts.cxx

# UI text: SimSun 20/30 (stub_fonts aliases font_puhui_*). Boot logo: Press Start.
# Chat bubbles: full PuHui CJK. Icons/numbers: real FontAwesome / PuHui number.
CSRCS += display/fonts/lv_font_simsun_20_cjk.c
CSRCS += display/fonts/lv_font_simsun_30_cjk.c
CSRCS += display/fonts/lv_font_press_start_2p_120.c
CSRCS += display/fonts/font_chat_cjk_30.c
CSRCS += display/fonts/font_awesome_20_4.c
CSRCS += display/fonts/font_awesome_30_4.c
CSRCS += display/fonts/font_puhui_number_50_4.c
CSRCS += display/fonts/font_puhui_number_120_4.c

# Audio service (ported from MetalioClaw4 main/audio_service.cc).
# Uses the NuttX /dev/audio/pcm0 character device via AudioCodec.
# MetalioAudioCodec: direct I2S (no I2C codec chip on this board).
CXXSRCS += audio/audio_service.cxx audio/audio_codec.cxx audio/metalio_audio_codec.cxx \
           audio/opus_codec.cxx

# Protocol layer (ported from MetalioClaw4 main/protocols/).
# MqttProtocol uses a small BSD-socket MQTT v3.1.1 client + UDP audio channel.
# WebsocketProtocol uses a small RFC 6455 client over BSD sockets.
CXXSRCS += protocols/protocol.cxx protocols/mqtt.cxx protocols/websocket_protocol.cxx

# MCP (Model Context Protocol) server — JSON-RPC tool dispatch.
CXXSRCS += mcp/mcp_server.cxx

# OTA firmware update — HTTP download + version/activation flow.
CXXSRCS += ota/ota.cxx

# Screen infrastructure (base class, manager, utilities, power policy)
CXXSRCS += display/screen/screen_base.cxx display/screen/screen_manager.cxx \
          display/screen/screen_util.cxx \
          display/screen/idle_power_policy/idle_power_policy.cxx \
          display/screen/pwr_key_handler/pwr_key_handler.cxx \
          display/screen/wifi_required_dialog/wifi_required_dialog.cxx

# Batch 1 screens (boot, standby, test, info, theme, settings)
CXXSRCS += display/screen/boot_screen/boot_screen.cxx \
          display/screen/standby_screen/standby_screen.cxx \
          display/screen/test_screen/test_screen.cxx \
          display/screen/test_screen/test_ui_common.cxx \
          display/screen/test_screen/screen_color_test.cxx \
          display/screen/test_screen/touch_panel_test.cxx \
          display/screen/test_screen/audio_test.cxx \
          display/screen/test_screen/vibrate_motor_test.cxx \
          display/screen/test_screen/camera_test.cxx \
          display/screen/test_screen/cell_4g_test.cxx \
          display/screen/test_screen/wifi_test.cxx \
          display/screen/test_screen/sc7a20h_test.cxx \
          display/screen/test_screen/qmc6309_test.cxx \
          display/screen/test_screen/sd_card_test.cxx \
          display/screen/test_screen/gps_test.cxx \
          display/screen/test_screen/bq27220_test.cxx \
          display/screen/test_screen/nu1680_test.cxx \
          display/screen/test_screen/pin_gpio_test.cxx \
          display/screen/test_screen/auto_test_screen.cxx \
          display/screen/test_screen/stress_test_screen.cxx \
          display/screen/info_screen/info_screen.cxx \
          display/screen/theme_screen/theme_screen.cxx \
          display/screen/settings_screen/settings_screen.cxx

# LVGL stress demo (C, ported from lv_demo_stress)
CSRCS += display/screen/test_screen/stress_demo.c

# Batch 2 screens (backlight, bluetooth, sd_card, vibrate, magnet, level)
CXXSRCS += display/screen/backlight_screen/backlight_screen.cxx \
          display/screen/bluetooth_screen/bluetooth_screen.cxx \
          display/screen/sd_card_screen/sd_card_screen.cxx \
          display/screen/vibrate_screen/vibrate_screen.cxx \
          display/screen/magnet_screen/magnet_screen.cxx \
          display/screen/level_screen/level_screen.cxx

# Batch 3 screens (home, chat) — main launcher + chat page
CXXSRCS += display/screen/home_screen/home_screen.cxx \
          display/screen/chat_screen/chat_screen.cxx

# Batch 5 screens (camera, calculator, calendar, call)
CXXSRCS += display/screen/camera_screen/camera_screen.cxx \
          display/screen/camera_screen/camera_v4l2_driver.cxx \
          display/screen/camera_screen/camera_jpeg_encoder.cxx \
          display/screen/calculator_screen/calculator_screen.cxx \
          display/screen/calendar_screen/calendar_screen.cxx \
          display/screen/call_screen/call_screen.cxx

# Batch 6 screens (ota, pin_test, secondary, game_2048)
CXXSRCS += display/screen/ota_screen/ota_screen.cxx \
          display/screen/pin_test_screen/pin_test_screen.cxx \
          display/screen/secondary_screen/secondary_screen.cxx \
          display/screen/game_2048_screen/game_2048_screen.cxx

# Batch 7 screens (ai_image_gen, translate, digital_people)
CXXSRCS += display/screen/ai_image_gen_screen/ai_image_gen_screen.cxx \
          display/screen/translate_screen/translate_screen.cxx \
          display/screen/digital_people_screen/digital_people_screen.cxx \
          display/screen/digital_people_screen/digital_people_settings_screen.cxx \
          display/screen/digital_people_screen/digital_people_prefs.cxx

# Batch 8 screens (weather, music, gps, radio)
CXXSRCS += display/screen/weather_screen/weather_screen.cxx \
          display/screen/weather_screen/weather_icon_map.cxx \
          display/screen/weather_screen/weather_city_list.cxx \
          display/screen/music_screen/music_screen.cxx \
          display/screen/gps_screen/gps_screen.cxx \
          display/screen/radio_screen/radio_player.cxx \
          display/screen/radio_screen/radio_screen.cxx \
          display/screen/cicada_screen/cicada_screen.cxx

# Batch 9 screens (network, openclaw, recording, espclaw)
CXXSRCS += display/screen/recording_screen/recording_screen.cxx \
          display/screen/network_screen/network_screen.cxx \
          display/screen/openclaw_screen/openclaw_screen.cxx \
          display/screen/espclaw_screen/espclaw_screen.cxx


VPATH += :port :system :i18n :display :audio :protocols :mcp :ota
VPATH += :led :boards
VPATH += :display/fonts
VPATH += :display/screen
VPATH += :display/screen/boot_screen :display/screen/standby_screen
VPATH += :display/screen/test_screen :display/screen/info_screen
VPATH += :display/screen/theme_screen :display/screen/settings_screen
VPATH += :display/screen/backlight_screen :display/screen/bluetooth_screen
VPATH += :display/screen/sd_card_screen :display/screen/vibrate_screen
VPATH += :display/screen/magnet_screen :display/screen/level_screen
VPATH += :display/screen/idle_power_policy :display/screen/pwr_key_handler
VPATH += :display/screen/wifi_required_dialog
VPATH += :display/screen/home_screen :display/screen/chat_screen
VPATH += :display/screen/camera_screen :display/screen/calculator_screen
VPATH += :display/screen/calendar_screen :display/screen/call_screen
VPATH += :display/screen/ota_screen :display/screen/pin_test_screen
VPATH += :display/screen/secondary_screen :display/screen/game_2048_screen
VPATH += :display/screen/ai_image_gen_screen :display/screen/translate_screen
VPATH += :display/screen/digital_people_screen
VPATH += :display/screen/weather_screen :display/screen/music_screen
VPATH += :display/screen/gps_screen :display/screen/radio_screen
VPATH += :display/screen/cicada_screen
VPATH += :display/screen/network_screen :display/screen/openclaw_screen
VPATH += :display/screen/recording_screen :display/screen/espclaw_screen

CFLAGS += -I$(APPDIR)/metalio/include
CFLAGS += -I$(APPDIR)/metalio/port
CFLAGS += -I$(APPDIR)/metalio/led
CFLAGS += -I$(APPDIR)/metalio/boards
CFLAGS += -I$(APPDIR)/metalio/audio
CFLAGS += -I$(APPDIR)/metalio/protocols
CFLAGS += -I$(APPDIR)/metalio/mcp
CFLAGS += -I$(APPDIR)/metalio/ota
CFLAGS += -I$(APPDIR)/metalio/display
CFLAGS += -I$(APPDIR)/metalio/display/screen
CFLAGS += -I$(APPDIR)/metalio/display/screen/pwr_key_handler
CFLAGS += -I$(APPDIR)/metalio/display/screen/idle_power_policy
CFLAGS += -I$(APPDIR)/metalio/display/screen/wifi_required_dialog
CFLAGS += -I$(APPDIR)/metalio/display/screen/home_screen
CFLAGS += -I$(APPDIR)/metalio/display/screen/chat_screen
CFLAGS += -I$(APPDIR)/metalio/display/screen/digital_people_screen
CFLAGS += -I$(APPDIR)/metalio/system
CFLAGS += -I$(APPDIR)/metalio/i18n
CFLAGS += -I$(APPDIR)/../vendor/metalio/include
CFLAGS += -I$(TOPDIR)/boards/risc-v/esp32p4/metalio-claw-4/include
CXXFLAGS += -I$(APPDIR)/metalio/include
CXXFLAGS += -I$(APPDIR)/metalio/port
CXXFLAGS += -I$(APPDIR)/metalio/led
CXXFLAGS += -I$(APPDIR)/metalio/boards
CXXFLAGS += -I$(APPDIR)/metalio/audio
CXXFLAGS += -I$(APPDIR)/metalio/protocols
CXXFLAGS += -I$(APPDIR)/metalio/mcp
CXXFLAGS += -I$(APPDIR)/metalio/ota
CXXFLAGS += -I$(APPDIR)/metalio/display
CXXFLAGS += -I$(APPDIR)/metalio/display/screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/pwr_key_handler
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/idle_power_policy
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/wifi_required_dialog
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/home_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/cicada_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/chat_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/digital_people_screen
CXXFLAGS += -I$(APPDIR)/metalio/system
CXXFLAGS += -I$(APPDIR)/metalio/i18n
CXXFLAGS += -I$(APPDIR)/../vendor/metalio/include
CXXFLAGS += -I$(TOPDIR)/boards/risc-v/esp32p4/metalio-claw-4/include

# ESP-GMF port (HLS streaming + FFT). Sources are conditionally added by
# the included Make.defs based on CONFIG_METALIO_ESP_GMF / CONFIG_METALIO_GMF_FFT.
include $(APPDIR)/metalio/audio/esp-gmf/Make.defs

# ESP-SR (WakeNet / AFE AEC)
include $(APPDIR)/metalio/audio/esp-sr/Make.defs

include $(APPDIR)/Application.mk
