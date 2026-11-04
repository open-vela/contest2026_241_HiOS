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
 * All emulated 64-bit operations share one native-word spinlock. Local
 * IRQ masking prevents an ISR from deadlocking on the interrupted task;
 * the spinlock also excludes the other HP CPU in SMP builds. Explicit
 * fences conservatively provide sequential consistency for every order.
 */

#include <stdint.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

static spinlock_t g_atomic8_lock = SP_UNLOCKED;

static irqstate_t atomic8_enter(void)
{
    irqstate_t flags = spin_lock_irqsave(&g_atomic8_lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return flags;
}

static void atomic8_leave(irqstate_t flags)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&g_atomic8_lock, flags);
}

/* GCC's libatomic-compatible signatures.
 *
 * __atomic_load_8:  atomically load 8 bytes (64 bits) from *ptr
 * __atomic_store_8: atomically store 8 bytes (64 bits) to *ptr
 *
 * All orders use the stronger sequentially-consistent implementation.                                */

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
    irqstate_t flags = atomic8_enter();
    uint64_t val = *(const volatile uint64_t *)ptr;
    atomic8_leave(flags);
    return val;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder)
{
    irqstate_t flags = atomic8_enter();
    *(volatile uint64_t *)ptr = val;
    atomic8_leave(flags);
}

/* __atomic_compare_exchange_8 — used by std::atomic::compare_exchange_weak
 * and std::atomic::compare_exchange_strong on 64-bit types. */
_Bool __atomic_compare_exchange_8(volatile void *ptr, void *expected,
                                   uint64_t desired, int weak,
                                   int success_memorder, int failure_memorder)
{
    irqstate_t flags = atomic8_enter();
    uint64_t current = *(volatile uint64_t *)ptr;
    if (current == *(uint64_t *)expected)
    {
        *(volatile uint64_t *)ptr = desired;
        atomic8_leave(flags);
        return 1;
    }
    *(uint64_t *)expected = current;
    atomic8_leave(flags);
    return 0;
}

/* __atomic_exchange_8 — used by std::atomic::exchange on 64-bit types. */
uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder)
{
    irqstate_t flags = atomic8_enter();
    uint64_t old = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = val;
    atomic8_leave(flags);
    return old;
}
