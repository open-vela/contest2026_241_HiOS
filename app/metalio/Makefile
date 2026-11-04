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
CXXSRCS  = application.cxx openclaw_client.cxx network_service.cxx app_dns.cxx ui_home.cxx
CXXSRCS += chat_local_cmd.cxx
CXXSRCS += display/input_method/pinyin_engine.cxx display/input_method/hi_input_method.cxx

# ESP-IDF shim layer
CXXSRCS += port/board_shim.cxx
CXXSRCS += port/usb_virtual_disk.cxx port/usb_secondary_display.cxx
CSRCS += port/tinyusb/tusb.c port/tinyusb/common/tusb_fifo.c
CSRCS += port/tinyusb/device/usbd.c port/tinyusb/class/msc/msc_device.c port/tinyusb/class/vendor/vendor_device.c port/tinyusb/class/hid/hid_device.c
CSRCS += port/tinyusb/portable/synopsys/dwc2/dwc2_common.c
CSRCS += port/tinyusb/portable/synopsys/dwc2/dcd_dwc2.c
CFLAGS += -I$(APPDIR)/metalio/port/tinyusb
CXXFLAGS += -I$(APPDIR)/metalio/port/tinyusb
VPATH += :port/tinyusb :port/tinyusb/common :port/tinyusb/device
VPATH += :port/tinyusb/class/hid :port/tinyusb/class/vendor :port/tinyusb/class/msc :port/tinyusb/portable/synopsys/dwc2

# Portable WebP decoder for direct AI Horde results.
CSRCS += third_party/libwebp/src/dec/alpha_dec.c third_party/libwebp/src/dec/buffer_dec.c third_party/libwebp/src/dec/frame_dec.c third_party/libwebp/src/dec/idec_dec.c third_party/libwebp/src/dec/io_dec.c third_party/libwebp/src/dec/quant_dec.c third_party/libwebp/src/dec/tree_dec.c third_party/libwebp/src/dec/vp8_dec.c third_party/libwebp/src/dec/vp8l_dec.c third_party/libwebp/src/dec/webp_dec.c
CSRCS += third_party/libwebp/src/dsp/alpha_processing.c third_party/libwebp/src/dsp/cpu.c third_party/libwebp/src/dsp/dec.c third_party/libwebp/src/dsp/dec_clip_tables.c third_party/libwebp/src/dsp/filters.c third_party/libwebp/src/dsp/lossless.c third_party/libwebp/src/dsp/rescaler.c third_party/libwebp/src/dsp/upsampling.c third_party/libwebp/src/dsp/yuv.c
CSRCS += third_party/libwebp/src/utils/bit_reader_utils.c third_party/libwebp/src/utils/color_cache_utils.c third_party/libwebp/src/utils/filters_utils.c third_party/libwebp/src/utils/huffman_utils.c third_party/libwebp/src/utils/palette.c third_party/libwebp/src/utils/quant_levels_dec_utils.c third_party/libwebp/src/utils/random_utils.c third_party/libwebp/src/utils/rescaler_utils.c third_party/libwebp/src/utils/thread_utils.c third_party/libwebp/src/utils/utils.c
CFLAGS += -I$(APPDIR)/metalio/third_party/libwebp
CXXFLAGS += -I$(APPDIR)/metalio/third_party/libwebp

# Vendored cJSON library (single-file, MIT licensed)
CSRCS += port/cjson/cJSON.c
CSRCS += port/quirc/quirc.c port/quirc/identify.c port/quirc/decode.c port/quirc/version_db.c
CXXSRCS += display/screen/smart_home/ha_qr_scanner.cxx display/screen/smart_home/ha_qr_token.cxx

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
CSRCS += display/icons_bundle.c display/icons_bundle_extra.c display/lvgl_assets_fs.c display/info_hios_logo_asset.c

# LED control (WS2812 single LED, PWM GPIO LED, WS2812 circular strip)
CXXSRCS += led/single_led.cxx led/gpio_led.cxx led/circular_strip.cxx

# Board implementation (metalio-claw-4 application-level init)
CXXSRCS += boards/metalio_claw_4_board.cxx

# SimpleUart — /dev/ttyS1 AT-command channel for the BT audio codec
CXXSRCS += boards/simple_uart.cxx

# Application framework (settings, system_info, device state events, i18n)
CXXSRCS += system/settings.cxx system/system_info.cxx system/device_state_event.cxx
CXXSRCS += system/sim_pin_store.cxx system/phone_service.cxx
CXXSRCS += system/phone_number_region.cxx system/sms_service.cxx system/sms_pdu.cxx
CXXSRCS += display/screen/sms_screen/sms_screen.cxx
VPATH += :display/screen/sms_screen
CXXSRCS += i18n/i18n.cxx

# EAF animation player, shared by chat and digital people.
CSRCS += display/eaf/esp_eaf_dec.c display/eaf/lv_eaf.c display/eaf/eaf_nuttx_jpeg.c
CSRCS += display/eaf/lv_sjpg.c
VPATH += :display/eaf
CFLAGS += -I$(APPDIR)/metalio/display/eaf
CXXFLAGS += -I$(APPDIR)/metalio/display/eaf

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
CSRCS += display/fonts/font_chat_cjk_30.c display/fonts/font_puhui_full_20.c
ASRCS += display/fonts/puhui_20_bin.S
CSRCS += display/fonts/font_network_status.c
CSRCS += display/fonts/font_awesome_20_4.c
CSRCS += display/fonts/font_ai_slogan_32_bold.c
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
          display/screen/wifi_required_dialog/wifi_required_dialog.cxx \
          display/screen/system_chrome/system_chrome.cxx \
          display/screen/system_chrome/control_center.cxx \
          display/screen/system_chrome/recents_overlay.cxx

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

# Batch 2 screens (bluetooth, sd_card, vibrate, magnet, level)
CXXSRCS += display/screen/bluetooth_screen/bluetooth_screen.cxx \
          display/screen/sd_card_screen/sd_card_screen.cxx \
          display/screen/vibrate_screen/vibrate_screen.cxx \
          display/screen/magnet_screen/magnet_screen.cxx \
          display/screen/level_screen/level_screen.cxx

# Batch 3 screens (home, chat) — main launcher + chat page
CXXSRCS += display/screen/home_screen/home_screen.cxx \
          display/screen/home_screen/desktop_folders.cxx \
          display/screen/home_screen/home_icon.cxx \
          display/screen/chat_screen/chat_screen.cxx \
          display/screen/panel_screen/panel_screen.cxx \
          display/screen/smart_home/smart_home.cxx \
          display/screen/smart_home/ha_client.cxx \
          display/screen/home_app_screen/home_app_screen.cxx

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
          display/screen/game_2048_screen/game_2048_screen.cxx \
          display/screen/arcade_screen/arcade_engine.cxx \
          display/screen/arcade_screen/arcade_screen.cxx \
          display/screen/arcade_screen/fruit_screen.cxx \
          display/screen/xiangqi_screen/xiangqi_engine.cxx \
          display/screen/xiangqi_screen/xiangqi_screen.cxx

# Batch 7 screens (translate, digital_people, AI image generation)
CXXSRCS += display/screen/translate_screen/translate_screen.cxx \
          display/screen/ai_image_gen_screen/ai_image_gen_screen.cxx \
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

# Hi Music local SD-card player (separate from the Bluetooth Music app).
CXXSRCS += display/screen/hi_music_screen/hi_music_screen.cxx \
          display/screen/hi_music_screen/local_music_player.cxx

# Batch 9 screens (network, openclaw, recording)
CXXSRCS += display/screen/recording_screen/recording_screen.cxx \
          display/screen/network_screen/network_screen.cxx \
          display/screen/openclaw_screen/openclaw_screen.cxx


VPATH += :port :system :i18n :display :audio :protocols :mcp :ota
VPATH += :led :boards
VPATH += :display/fonts
VPATH += :display/screen
VPATH += :display/screen/boot_screen :display/screen/standby_screen
VPATH += :display/screen/test_screen :display/screen/info_screen
VPATH += :display/screen/theme_screen :display/screen/settings_screen
VPATH += :display/screen/bluetooth_screen
VPATH += :display/screen/sd_card_screen :display/screen/vibrate_screen
VPATH += :display/screen/magnet_screen :display/screen/level_screen
VPATH += :display/screen/idle_power_policy :display/screen/pwr_key_handler
VPATH += :display/screen/wifi_required_dialog
VPATH += :display/screen/system_chrome
VPATH += :display/screen/home_screen :display/screen/chat_screen
VPATH += :display/screen/panel_screen :display/screen/smart_home :display/screen/home_app_screen
VPATH += :display/screen/camera_screen :display/screen/calculator_screen
VPATH += :display/screen/calendar_screen :display/screen/call_screen
VPATH += :display/screen/ota_screen :display/screen/pin_test_screen
VPATH += :display/screen/secondary_screen :display/screen/game_2048_screen
VPATH += :display/screen/arcade_screen :display/screen/xiangqi_screen
VPATH += :display/screen/translate_screen
VPATH += :display/screen/ai_image_gen_screen
VPATH += :display/screen/digital_people_screen
VPATH += :display/screen/weather_screen :display/screen/music_screen
VPATH += :display/screen/hi_music_screen
VPATH += :display/screen/gps_screen :display/screen/radio_screen
VPATH += :display/screen/cicada_screen
VPATH += :display/screen/network_screen :display/screen/openclaw_screen
VPATH += :display/screen/recording_screen

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
CFLAGS += -I$(APPDIR)/metalio/display/screen/system_chrome
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
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/system_chrome
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/home_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/cicada_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/chat_screen
CXXFLAGS += -I$(APPDIR)/metalio/display/screen/digital_people_screen
CXXFLAGS += -I$(APPDIR)/metalio/system
CXXFLAGS += -I$(APPDIR)/metalio/i18n
CXXFLAGS += -I$(APPDIR)/../vendor/metalio/include
CXXFLAGS += -I$(TOPDIR)/boards/risc-v/esp32p4/metalio-claw-4/include

# HiOS OpenClaw agent backend and on-screen input method.
CXXSRCS += agent/hios_agent.cxx
CSRCS += agent/hios_tls_roots.c agent/mimo_voice.c
CSRCS += agent/upstream/src/llm/llm_vision.c
CSRCS += agent/upstream/src/voice/voice_asr.c agent/upstream/src/voice/voice_tts.c \
         agent/upstream/src/voice/volc_asr.c agent/upstream/src/voice/volc_tts.c \
         agent/upstream/src/voice/volc_tts_ws.c
CSRCS += agent/upstream/src/core/message_bus.c agent/upstream/src/core/message_bus_tap.c \
         agent/upstream/src/core/agent_loop.c agent/upstream/src/core/agent_trace.c \
         agent/upstream/src/core/context_builder.c agent/upstream/src/core/memory_store.c \
         agent/upstream/src/core/session_mgr.c agent/upstream/src/llm/llm_proxy.c \
         agent/upstream/src/llm/llm_parse.c agent/upstream/src/llm/llm_router.c \
         agent/upstream/src/llm/llm_cache.c agent/upstream/src/infra/config_store.c \
         agent/upstream/src/infra/http_proxy.c agent/upstream/src/infra/vela_tls.c \
         agent/upstream/src/tools/tool_registry.c agent/upstream/src/tools/tool_guard.c \
         agent/upstream/src/tools/skill_loader.c agent/upstream/src/tools/tool_get_time.c \
         agent/upstream/src/tools/tool_files.c agent/upstream/src/node/node_client.c
CFLAGS += -I$(APPDIR)/metalio/agent -I$(APPDIR)/metalio/agent/upstream/include \
          -I$(APPDIR)/metalio/agent/upstream/src \
          -I$(APPDIR)/metalio/port/cjson \
          -DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR='"/sdcard/hios-agent"'
CXXFLAGS += -I$(APPDIR)/metalio/agent -I$(APPDIR)/metalio/agent/upstream/include \
            -I$(APPDIR)/metalio/agent/upstream/src \
            -I$(APPDIR)/metalio/display/input_method \
            -I$(APPDIR)/metalio/port/cjson \
            -DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR='"/sdcard/hios-agent"'
VPATH += :display/input_method

# ESP-GMF port (HLS streaming + FFT). Sources are conditionally added by
# the included Make.defs based on CONFIG_METALIO_ESP_GMF / CONFIG_METALIO_GMF_FFT.
include $(APPDIR)/metalio/audio/esp-gmf/Make.defs

# ESP-SR (WakeNet / AFE AEC)
include $(APPDIR)/metalio/audio/esp-sr/Make.defs

# System Hi input, compass, clock and independent notification audio.
CXXSRCS += audio/local_audio_decode.cxx system/notification_sound.cxx system/clock_service.cxx
ifeq ($(CONFIG_METALIO_PHONE_PCM_BRIDGE),y)
CXXSRCS += audio/phone_pcm_bridge.cxx
endif
CXXSRCS += display/screen/clock_screen/clock_screen.cxx display/screen/compass_screen/compass_screen.cxx
VPATH += :display/screen/clock_screen :display/screen/compass_screen

# HiApp SDK 1: one shared, bounded Lua runtime; app packages stay on SD.
CXXSRCS += hiapp/hiapp.cxx hiapp/runtime.cxx hiapp/resources.cxx hiapp/registry.cxx hiapp/javascript.cxx display/screen/app_manager_screen/app_manager_screen.cxx
CXXSRCS += hiapp/web_images.cxx
CXXSRCS += hiapp/video_player.cxx hiapp/video_source.cxx hiapp/video_scan.cxx
CSRCS += hiapp/video_decoder.c hiapp/video_convert.c hiapp/video_sdk_mutex.c $(wildcard third_party/tinyh264/native/*.c)
# Speed-optimize only the bounded video path; preserve global size settings.
$(foreach src,hiapp/video_decoder.c hiapp/video_convert.c $(wildcard third_party/tinyh264/native/*.c),$(eval $(src)_CFLAGS += -O3 -fno-strict-aliasing))
hiapp/video_player.cxx_CXXFLAGS += -O2 -fno-strict-aliasing
CFLAGS += -I$(APPDIR)/metalio/hiapp -I$(APPDIR)/metalio/third_party/tinyh264/native
CXXFLAGS += -I$(APPDIR)/metalio/hiapp -I$(APPDIR)/metalio/third_party/tinyh264/native
VPATH += :third_party/tinyh264/native
# Reuse the existing Espressif container parsers; link only referenced formats.
VIDEO_EXP_ROOT = audio/esp-gmf/components/esp_extractor
# SDK pthread objects use ESP-IDF layout. Native NuttX mutexes must never be
# initialized in their four-byte slots. Rewrite a derived archive's imports.
VIDEO_ABI_READY := $(shell python3 $(APPDIR)/../scripts/prepare_hivideo_extractor.py --source $(APPDIR)/metalio/$(VIDEO_EXP_ROOT)/lib/esp32p4/libesp_extractor.a --output $(APPDIR)/staging/libhivideo_extractor.a --ar $(firstword $(AR)))
ifneq ($(VIDEO_ABI_READY),ready)
$(error Failed to prepare Hi Video extractor ABI; refusing unsafe SDK linkage)
endif
# NuttX also discovers staging archives by filename. Replace the old staging
# alias, otherwise it can satisfy SDK symbols before the adapted archive.
$(shell ln -sfn libhivideo_extractor.a $(APPDIR)/staging/libesp_extractor.a)
EXTRA_LIBS += -lhivideo_extractor
ifeq ($(HI_VIDEO_TARGET_TEST),1)
CXXFLAGS += -DHI_VIDEO_TARGET_TEST -I$(APPDIR)/../scripts/hi_video_tests
hiapp/video_convert.c_CFLAGS += -DHI_VIDEO_TARGET_TEST
endif
CSRCS += hiapp/web_svg.c hiapp/web_jpeg.c third_party/webgif/gif.c
VPATH += :third_party/webgif
VPATH += :hiapp :display/screen/app_manager_screen :third_party/lua54
CXXFLAGS += -I$(APPDIR)/metalio/hiapp -I$(APPDIR)/metalio/third_party/lua54 -DLUA_32BITS
CFLAGS += -I$(APPDIR)/metalio/third_party/lua54 -DLUA_32BITS -DLUAI_MAXCCALLS=60
CSRCS += third_party/lua54/lapi.c third_party/lua54/lcode.c third_party/lua54/lctype.c \
 third_party/lua54/ldebug.c third_party/lua54/ldo.c third_party/lua54/ldump.c \
 third_party/lua54/lfunc.c third_party/lua54/lgc.c third_party/lua54/llex.c \
 third_party/lua54/lmem.c third_party/lua54/lobject.c third_party/lua54/lopcodes.c \
 third_party/lua54/lparser.c third_party/lua54/lstate.c third_party/lua54/lstring.c \
 third_party/lua54/ltable.c third_party/lua54/ltm.c third_party/lua54/lundump.c \
 third_party/lua54/lvm.c third_party/lua54/lzio.c third_party/lua54/lauxlib.c \
 third_party/lua54/lbaselib.c third_party/lua54/lmathlib.c third_party/lua54/lstrlib.c \
 third_party/lua54/ltablib.c third_party/lua54/lutf8lib.c

CXXSRCS += display/screen/digital_people_screen/xiaobai_resources.cxx
ASRCS += display/screen/digital_people_screen/xiaobai_assets.S
VPATH += :third_party/duktape
CSRCS += third_party/duktape/duktape.c
CFLAGS += -I$(APPDIR)/metalio/third_party/duktape
CXXFLAGS += -I$(APPDIR)/metalio/third_party/duktape
CSRCS += port/hi_task_policy.c
CXXFLAGS += -I$(APPDIR)/metalio/port/cxx_smp
ifeq ($(HI_APP_SMP_PROBE),1)
CXXSRCS += port/hi_task_probe.cxx
CFLAGS += -DHI_APP_SMP_PROBE=1
CXXFLAGS += -DHI_APP_SMP_PROBE=1
endif

ifeq ($(HI_RADIO_4G_PROBE),1)
CXXFLAGS += -DHI_STABILITY_PROBE=1 -DHI_RADIO_4G_PROBE=1
endif

ifeq ($(HI_SERVICES_PROBE),1)
CXXFLAGS += -DHI_SERVICES_PROBE=1
endif
ifeq ($(HI_FOLDER_FOCUSED_PROBE),1)
CXXFLAGS += -DHI_FOLDER_FOCUSED_PROBE=1
endif
ifeq ($(HI_FOLDER_PROBE_RECOVERY),1)
CXXFLAGS += -DHI_FOLDER_PROBE_RECOVERY=1
endif
CXXSRCS += display/screen/weather_screen/weather_service.cxx
CXXSRCS += display/screen/weather_screen/weather_direct.cxx

include $(APPDIR)/Application.mk

# These public C++ headers contain shared object layouts. The generated
# dependency file can survive an incremental source sync, so an older caller
# can otherwise allocate the former sizeof(Http) while the new constructor
# writes the larger object. Recompile every C++ caller when a public layout
# or this source list changes, including callers with transitive includes.
METALIO_LAYOUT_HEADERS = $(wildcard include/*.h ota/*.h hiapp/*.h third_party/webgif/*.h port/hi_task*.h) cxx11_support/thread port/cxx_smp/thread display/display.h
$(CXXOBJS): $(METALIO_LAYOUT_HEADERS) Makefile
$(filter %web_svg.c$(SUFFIX)$(OBJEXT),$(COBJS)): $(wildcard third_party/nanosvg/*.h)
$(filter %web_jpeg.c$(SUFFIX)$(OBJEXT),$(COBJS)): third_party/stb/stb_image.h
$(filter %gif.c$(SUFFIX)$(OBJEXT),$(COBJS)): third_party/webgif/AnimatedGIF.h

# .incbin inputs are not discovered by the assembler dependency scanner.
# Rebuild the resource object whenever the packed binary changes.
$(filter %xiaobai_assets.S$(SUFFIX)$(OBJEXT),$(CAOBJS)): display/screen/digital_people_screen/xiaobai_assets.bin




# Runtime includes are application code and must participate in incremental builds.
$(filter %runtime.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): $(wildcard hiapp/*.inc)

$(filter %home_screen.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): display/screen/home_screen/desktop_drag.h display/screen/home_screen/desktop_storage_queue.h display/screen/home_screen/desktop_folder_pager.inc display/screen/home_screen/desktop_folder_ui.inc $(wildcard display/screen/home_screen/stability_probe.inc)
$(filter %home_screen.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): $(wildcard display/screen/home_screen/radio_4g_probe.inc)
$(filter %radio_player.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): display/screen/radio_screen/radio_connect_policy.h

$(filter %compass_screen.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): $(wildcard display/screen/compass_screen/*.h)

$(filter %ai_image_gen_screen.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): $(wildcard display/screen/ai_image_gen_screen/horde*.h)

$(filter %gps_screen.cxx$(SUFFIX)$(OBJEXT),$(CXXOBJS)): display/screen/gps_screen/gps_direct.inc
