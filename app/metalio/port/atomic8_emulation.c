/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * 64-bit atomic operation emulation for rv32imac.
 *
 * The riscv32-esp-elf toolchain (rv32imac) has no libatomic.a and the
 * ISA lacks native 64-bit atomic instructions (LR/SC are 32-bit only).
 * GCC emits calls to __atomic_load_8 / __atomic_store_8 when it
 * encounters std::atomic<int64_t> or std::atomic<uint64_t>.
 *
 * Since CONFIG_SMP=n on ESP32-P4 (single HP core), disabling interrupts
 * around the load/store is a safe way to provide atomicity.
 *
 * For SMP builds, swap to a spinlock-based implementation or enable
 * the zacas extension (rv32imac_zacas) for native LR/SC on 64-bit.
 */

#include <stdint.h>
#include <nuttx/irq.h>

/* GCC's libatomic-compatible signatures.
 *
 * __atomic_load_8:  atomically load 8 bytes (64 bits) from *ptr
 * __atomic_store_8: atomically store 8 bytes (64 bits) to *ptr
 *
 * The memorder parameter is ignored — IRQ disable provides
 * sequentially-consistent semantics.                                */

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
    irqstate_t flags = up_irq_save();
    uint64_t val = *(const volatile uint64_t *)ptr;
    up_irq_restore(flags);
    return val;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder)
{
    irqstate_t flags = up_irq_save();
    *(volatile uint64_t *)ptr = val;
    up_irq_restore(flags);
}

/* __atomic_compare_exchange_8 — used by std::atomic::compare_exchange_weak
 * and std::atomic::compare_exchange_strong on 64-bit types. */
_Bool __atomic_compare_exchange_8(volatile void *ptr, void *expected,
                                   uint64_t desired, int weak,
                                   int success_memorder, int failure_memorder)
{
    irqstate_t flags = up_irq_save();
    uint64_t current = *(volatile uint64_t *)ptr;
    if (current == *(uint64_t *)expected)
    {
        *(volatile uint64_t *)ptr = desired;
        up_irq_restore(flags);
        return 1;
    }
    *(uint64_t *)expected = current;
    up_irq_restore(flags);
    return 0;
}

/* __atomic_exchange_8 — used by std::atomic::exchange on 64-bit types. */
uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder)
{
    irqstate_t flags = up_irq_save();
    uint64_t old = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = val;
    up_irq_restore(flags);
    return old;
}
