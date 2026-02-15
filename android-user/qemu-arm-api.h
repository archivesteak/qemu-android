/*
 * QEMU ARM Library API - Embeddable ARM32 emulation engine
 *
 * Converts QEMU user-mode emulation from a standalone process into
 * an embeddable library for running ARM32 code within an Android app.
 *
 * Copyright (c) 2024-2026 PERunner Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_ARM_API_H
#define QEMU_ARM_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the QEMU ARM emulation engine.
 *
 * Must be called once before any other qemu_arm_* functions.
 * Sets up TCG JIT compiler, creates ARM CPU, configures address space.
 *
 * @cpu_model: ARM CPU model name (e.g., "cortex-a7"). NULL for default.
 * Returns: 0 on success, negative errno on failure.
 */
int qemu_arm_init(const char *cpu_model);

/*
 * Load an ARM32 ELF executable into the emulated address space.
 *
 * Parses ELF headers, maps PT_LOAD segments, resolves interpreter,
 * sets up stack with argv/envp/auxv, and configures initial CPU state.
 *
 * @path: Path to the ELF binary.
 * @argv: NULL-terminated argument array. argv[0] should be the program name.
 * @envp: NULL-terminated environment array. Can be NULL for empty env.
 * Returns: 0 on success, negative errno on failure.
 */
int qemu_arm_load_elf(const char *path, char **argv, char **envp);

/*
 * Run the loaded ELF binary until it exits.
 *
 * Enters the cpu_loop which translates and executes ARM32 code via TCG.
 * Returns when the guest calls exit/exit_group.
 *
 * Returns: guest exit code.
 */
int qemu_arm_run(void);

/*
 * Call an ARM32 function at the given guest address.
 *
 * Sets up registers r0-r3 with arguments, sets PC to func_addr,
 * sets LR to a return trampoline, and runs until the function returns.
 * Supports re-entrant calls (guest callback -> host -> guest).
 *
 * @func_addr: Guest address of the ARM32 function.
 * @a0..a3: Arguments placed in r0-r3 per ARM AAPCS.
 * Returns: Value in r0 after the function returns.
 */
uint32_t qemu_arm_call(uint32_t func_addr, uint32_t a0, uint32_t a1,
                       uint32_t a2, uint32_t a3);

/*
 * Allocate memory in the guest 32-bit address space.
 *
 * @size: Number of bytes to allocate.
 * Returns: Guest address of allocated memory, or 0 on failure.
 */
uint32_t qemu_arm_guest_alloc(size_t size);

/*
 * Convert a guest address to a host pointer.
 *
 * @guest_addr: 32-bit guest address.
 * Returns: Host pointer that can be used to read/write guest memory.
 */
void *qemu_arm_g2h(uint32_t guest_addr);

/*
 * Register a host callback that ARM32 code can call.
 *
 * Allocates a guest trampoline address that, when called by ARM32 code,
 * invokes the given host callback with the current CPU state.
 *
 * @callback: Host function to call. Receives CPU state for reading
 *            r0-r3 (arguments) and setting r0 (return value).
 * Returns: Guest address of the trampoline, or 0 on failure.
 */
typedef void (*qemu_arm_callback_fn)(void *cpu_env);
uint32_t qemu_arm_register_callback(qemu_arm_callback_fn callback);

/*
 * Get/set ARM32 register values.
 * reg 0-15: r0-r15 (r13=SP, r14=LR, r15=PC)
 */
uint32_t qemu_arm_get_reg(int reg);
void qemu_arm_set_reg(int reg, uint32_t value);

/*
 * Clean up and release all resources.
 */
void qemu_arm_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_ARM_API_H */
