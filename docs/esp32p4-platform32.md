# ESP32-P4 SMP and flash32 release

## CPU scheduling

NuttX SMP is enabled with CONFIG_NCPUS=2 and CONFIG_SMP_NCPUS=2.
CPU1 runs through the NuttX scheduler, not a separate raw AMP loop.
Each thread runs on one CPU at a time; threads may run concurrently.
Legacy UI, audio and hardware-service threads retain their inherited CPU0
mask (CONFIG_SMP_DEFAULT_CPUSET=0x1). The independent image converter has
a CPU0+CPU1 affinity mask (0x3); NuttX selects an eligible CPU by readiness
and priority. When CPU1 is busy it can run on CPU0, rather than forcing a
wait on CPU1. IPC still has a private task per CPU.
Explicit pthread/FreeRTOS affinity is installed before thread activation.
This is the first measured compute-worker migration. It deliberately does
not promise automatic load balancing for every app or reduced total work.
Do not change the default mask to 0x3 without driver and app acceptance.

Core startup, per-CPU CLIC/IPI routes, FPU context, cross-core atomic64,
internal IPC/interrupt stacks and image conversion have hardware proofs.
The image worker has a 20 ms bounded wait; timeout disables further offload
and the existing CPU0 converter produces the frame. Its reference-test
planes are static internal SRAM, not locals on the 2 KiB bring-up stack.
The existing VMware ESP speech archives are authoritative and unchanged.

## Flash layout

Validated board: ESP32-P4 rev 1.0, GD JEDEC C8 40 19, physical 32 MiB.
Firmware starts at 0x2000 and ends before 0x1fc0000.
Maximum image length: 33,284,096 bytes (31.7421875 MiB).
Reserved area: 0x1fc0000 through 0x1ffffff.
Settings A data/commit: 0x1fe0000 / 0x1fe1000.
Settings B data/commit: 0x1fe2000 / 0x1fe3000.
The MIO1 payload is unchanged; journal generation, checksum and commit
metadata are separate. Inactive data is verified before metadata commits.
Runtime flash writes cannot reach the application slot.

Both four-byte flash commands and boot cache addressing are enabled.
The 16,986,496-byte SMP engineering image executed the same high function
on CPU0 and CPU1 at physical flash 0x01035148, CRC d0f12b91. High-address
write/read/non-alias/restore also passed. Padding, acoustic tests and flash
write probes are excluded from the production binary.

## Build and flash

Build in /home/kaykay/HiOS/nuttx using the selected .config and VMware speech
libraries. Final firmware is nuttx/nuttx.bin in both Windows and VMware.
Use nuttx/flash_ai.sh [baud], optionally ESPTOOL_PORT=/dev/ttyACM0.
The OS performing the flash must own the P4 USB connection.
The wrapper selects isolated esptool 5.3.1 and uses a RAM stub. It checks
layout, existing image and settings, preserves the reserved journal, checks
boot, and recovers the verified release after a failed boot. Never erase the
whole chip. --check-image is read-only and requires no device.
HiOS_SAVED_VERSION.json records the immutable verified_firmware_relative.

## Acceptance boundaries

Hardware proofs cover startup, core tasks/FPU, atomic64, 19,200 image pixels,
both-core high XIP and native high flash restoration. These are not a claim
that every app, peripheral and long-running workload has been retested.
USB-related speaker hiss improved on battery and independent charging;
the user subsequently reported improvement on PC USB as well. No complete
noise elimination is claimed. Microphone, wake-word, routing and ownership
rules were preserved. Only atomic gain handling and a 10 ms volume-change
ramp were changed, with steady-state PCM verified unchanged.

## Measured image-worker scheduling

The engineering-only comparison uses the same YUV420 input and full RGB565
reference checks for fixed-CPU1 and both-CPU policies, with and without bounded
CPU1 contention. It confirms actual execution on both cores and checks the
existing one-shot timeout/CPU0 fallback. Camera preview, the music UI, the
chat idle UI and return-home navigation are observed in the same run. This
is not real phone, acoustic audio or spoken-AI acceptance. CPU utilization
sampling is approximate; frame timing uses a bounded read of the existing
microsecond systimer. The measurement JSON is stored under output/smp_balance.
The production image contains neither the sampling/pressure tasks nor their
automatic UI navigation. The legacy audio/microphone ownership and routing
rules, GUI affinity and communication-service bindings remain unchanged.
