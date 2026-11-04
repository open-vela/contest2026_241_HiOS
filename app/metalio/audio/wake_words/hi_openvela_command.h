#pragma once
/* Single command shared by registration, detection, display and reporting.
 * Pronunciation: /haɪ oʊpənˈvɛlə/.
 * ARPABET: HH AY1 | OW1 P AH0 N V EH1 L AH0.
 * MN7 symbols follow Espressif tool/multinet_g2p.py, not letter spelling. */
#define HI_OPENVELA_MODEL "mn7_en"
#define HI_OPENVELA_COMMAND_ID 1
#define HI_OPENVELA_PHRASE "Hi openvela"
#define HI_OPENVELA_PHONEMES "hi bPcN VfLc"
#define HI_OPENVELA_PHONEMES_CONTIGUOUS "hi bPcNVfLc"
#define HI_OPENVELA_PHONEMES_EY "hi bPcN VdLc"
#define HI_OPENVELA_PHONEMES_IY "hi bPcN VmLc"
#define HI_OPENVELA_PHONEME_COUNT 3
#define HI_OPENVELA_THRESHOLD 0.14f
#define HI_OPENVELA_DURATION_MS 5000
#define HI_OPENVELA_SAMPLE_RATE 16000

// Complete competing phrase; it must never emit a wake event.
#define HI_OPENVELA_REJECT_COMMAND_ID 2
#define HI_OPENVELA_REJECT_PHRASE "Hey openvela"
#define HI_OPENVELA_REJECT_PHONEMES "hd bPcN VfLc"
#define HI_OPENVELA_REJECT_PHONEMES_EY "hd bPcN VdLc"
#define HI_OPENVELA_REJECT_PHONEMES_IY "hd bPcN VmLc"
#define HI_OPENVELA_VOCABULARY_COUNT 6
