/*
 * QEMU ARM Library API implementation
 *
 * Extracts initialization and execution from linux-user/main.c into
 * a reusable library API for embedding ARM32 emulation.
 *
 * Copyright (c) 2024-2026 PERunner Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/accel.h"
#include "accel/accel-ops.h"
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/mman.h>

#include "qapi/error.h"
#include "qemu.h"
#include "user-internals.h"
#include "loader.h"
#include "user/safe-syscall.h"
#include "qemu/path.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/memfd.h"
#include "crypto/init.h"
#include "target_mman.h"
#include "exec/page-protection.h"
#include "exec/page-vary.h"
#include "exec/mmap-lock.h"
#include "exec/cpu-common.h"
#include "tcg/startup.h"
#include "user/cpu_loop.h"
#include "signal-common.h"
#include "user-mmap.h"
#include "fd-trans.h"

#include "qemu-arm-api.h"

/*
 * Macro to read a 32-bit instruction from guest memory,
 * handling endian swap for BE code. Copied from arm/cpu_loop.c.
 */
#define get_user_code_u32(x, gaddr, env)                \
    ({ abi_long __r = get_user_u32((x), (gaddr));       \
        if (!__r && bswap_code(arm_sctlr_b(env))) {     \
            (x) = bswap32(x);                           \
        }                                               \
        __r;                                            \
    })

/* From arm/cpu_loop.c — made non-static for library use */
extern int do_kernel_trap(CPUARMState *env);

/* Forward declarations from main.c */
extern unsigned long guest_stack_size;
extern unsigned long mmap_min_addr;

/* Library-mode global state */
static CPUState *lib_cpu;
static CPUARMState *lib_env;
static struct image_info lib_info;
static TaskState *lib_ts;
static bool lib_initialized;

/* Return trampoline: a guest address containing a BKPT instruction.
 * When PC hits this address, qemu_arm_call returns. */
static uint32_t return_trampoline_addr;

/* Callback table for host callbacks callable from ARM32 code */
#define MAX_CALLBACKS 256
static qemu_arm_callback_fn callback_table[MAX_CALLBACKS];
static int num_callbacks;
static uint32_t callback_trampoline_base;

/*
 * Allocate a page of guest memory for trampolines.
 * Write BKPT/UDF instructions that we intercept in our call loop.
 */
static int setup_trampolines(void)
{
    uint32_t page;
    uint32_t *host_ptr;

    /* Allocate a page in guest address space — writable first */
    page = target_mmap(0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == (uint32_t)-1) {
        return -ENOMEM;
    }

    host_ptr = (uint32_t *)g2h_untagged(page);

    /* First word: return trampoline (BKPT #0xFFFF) */
    return_trampoline_addr = page;
    host_ptr[0] = 0xE12FFF7F; /* BKPT #0xFFFF */

    /* Remaining words: callback trampolines using UDF instructions.
     * ARM encoding of UDF: 0xE7F000F0 | (imm12 << 8) | imm4
     * We encode the callback index into the immediate field. */
    callback_trampoline_base = page + 4;
    for (int i = 0; i < MAX_CALLBACKS && (i + 1) * 4 < 4096; i++) {
        host_ptr[i + 1] = 0xE7F000F0 | (((0xAB + (i >> 4)) & 0xFFF) << 8)
                          | (i & 0xF);
    }

    /* Make the trampoline page executable (no longer writable) */
    target_mprotect(page, 4096, PROT_READ | PROT_EXEC);

    return 0;
}

int qemu_arm_init(const char *cpu_model)
{
    AccelState *accel;
    AccelClass *ac;
    const char *cpu_type;
    unsigned long host_page_size;
    unsigned long max_reserved_va;

    if (lib_initialized) {
        return -EALREADY;
    }

    /* Phase 1: Infrastructure init (main.c:702-705) */
    error_init("qemu-arm-lib");
    module_call_init(MODULE_INIT_TRACE);
    qemu_init_cpu_list();
    module_call_init(MODULE_INIT_QOM);

    /* Phase 2: Configure paths (main.c:757-759) */
    init_paths("");
    init_qemu_uname_release();

    /* Phase 3: CPU model selection (main.c:793-796) */
    if (cpu_model == NULL) {
        cpu_model = "cortex-a7";
    }
    cpu_type = parse_cpu_option(cpu_model);

    /* Phase 4: TCG accelerator init (main.c:799-809) */
    accel = current_accel();
    ac = ACCEL_GET_CLASS(accel);
    accel_init_interfaces(ac);
    object_property_set_bool(OBJECT(accel), "one-insn-per-tb",
                             false, &error_abort);
    ac->init_machine(accel, NULL);

    /* Phase 5: Page size and CPU creation (main.c:816-823) */
    host_page_size = qemu_real_host_page_size();
    set_preferred_target_page_bits(ctz32(host_page_size));
    finalize_target_page_bits();

    lib_cpu = cpu_create(cpu_type);
    lib_env = cpu_env(lib_cpu);
    cpu_reset(lib_cpu);
    thread_cpu = lib_cpu;

    /* Phase 6: Address space configuration (main.c:831-895) */
    max_reserved_va = MAX_RESERVED_VA(lib_cpu);
    reserved_va = max_reserved_va;

    if (reserved_va != 0) {
        guest_addr_max = reserved_va;
    } else {
        guest_addr_max = UINT32_MAX;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wtautological-compare"

    if (reserved_va) {
        if (TASK_UNMAPPED_BASE < reserved_va) {
            task_unmapped_base = TASK_UNMAPPED_BASE;
        } else {
            task_unmapped_base = TARGET_PAGE_ALIGN(reserved_va / 3);
        }
    } else {
        task_unmapped_base = 0x10000000;
    }
    mmap_next_start = task_unmapped_base;

    if (reserved_va) {
        if (ELF_ET_DYN_BASE < reserved_va) {
            elf_et_dyn_base = ELF_ET_DYN_BASE;
        } else {
            elf_et_dyn_base = TARGET_PAGE_ALIGN(reserved_va / 3) * 2;
        }
    } else {
        elf_et_dyn_base = 0x18000000;
    }

#pragma GCC diagnostic pop

    /* Phase 7: Crypto init (main.c:899-910) */
    {
        Error *err = NULL;
        qcrypto_init(&err);
        if (err) {
            error_report_err(err);
            return -EINVAL;
        }
    }

    /* Phase 8: mmap_min_addr (main.c:920-943)
     * On Android, /proc/sys/vm/mmap_min_addr may not be readable.
     * Fall back to host page size. */
    {
        FILE *fp = fopen("/proc/sys/vm/mmap_min_addr", "r");
        if (fp) {
            unsigned long tmp;
            if (fscanf(fp, "%lu", &tmp) == 1 && tmp != 0) {
                mmap_min_addr = MAX(tmp, host_page_size);
            }
            fclose(fp);
        }
        if (mmap_min_addr == 0) {
            mmap_min_addr = host_page_size;
        }
    }

    /* Phase 9: fd translation init (main.c:972) */
    fd_trans_init();

    lib_initialized = true;
    return 0;
}

int qemu_arm_load_elf(const char *path, char **argv, char **envp)
{
    struct linux_binprm bprm;
    int execfd, ret;
    char **empty_env = NULL;

    if (!lib_initialized) {
        return -EINVAL;
    }

    /* Set up empty env if none provided */
    if (envp == NULL) {
        empty_env = g_new0(char *, 1);
        envp = empty_env;
    }

    /* Open the binary */
    execfd = open(path, O_RDONLY);
    if (execfd < 0) {
        g_free(empty_env);
        return -errno;
    }

    /* Set up task state (main.c:964-970) */
    memset(&lib_info, 0, sizeof(lib_info));
    memset(&bprm, 0, sizeof(bprm));

    lib_ts = g_new0(TaskState, 1);
    init_task_state(lib_ts);
    lib_ts->info = &lib_info;
    lib_ts->bprm = &bprm;
    lib_cpu->opaque = lib_ts;
    task_settid(lib_ts);

    /* Load the ELF binary (main.c:974) */
    ret = loader_exec(execfd, path, argv, envp, &lib_info, &bprm);
    close(execfd);
    g_free(empty_env);

    if (ret != 0) {
        return ret;
    }

    /* Set up heap (main.c:1018) */
    target_set_brk(lib_info.brk);

    /* Syscall init — after loader_exec, matching main.c:1019 order */
    syscall_init();

    /* Signal init (main.c:1020) */
    signal_init(NULL);

    /* Generate TCG prologue now that guest_base is fixed (main.c:1025) */
    tcg_prologue_init();

    /* Set initial CPU state from loaded binary (main.c:1027) */
    init_main_thread(lib_cpu, &lib_info);

    /* Set up trampolines for library-mode calling */
    ret = setup_trampolines();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int qemu_arm_run(void)
{
    if (!lib_initialized || !lib_ts) {
        return -EINVAL;
    }

    /* Run the standard cpu_loop — runs until exit() */
    cpu_loop(lib_env);

    /* cpu_loop normally never returns; in library mode
     * we'll intercept exit/exit_group to return here */
    return 0;
}

/*
 * Library-mode execution loop for qemu_arm_call.
 *
 * Similar to cpu_loop() in arm/cpu_loop.c but returns when
 * execution reaches the return trampoline address.
 */
uint32_t qemu_arm_call(uint32_t func_addr, uint32_t a0, uint32_t a1,
                       uint32_t a2, uint32_t a3)
{
    CPUState *cs = lib_cpu;
    int trapnr;

    if (!lib_initialized) {
        return (uint32_t)-1;
    }

    /* Save current register state for re-entrant calls */
    uint32_t saved_regs[16];
    uint32_t saved_cpsr = cpsr_read(lib_env);
    memcpy(saved_regs, lib_env->regs, sizeof(saved_regs));

    /* Set up call: arguments in r0-r3, LR = return trampoline, PC = func */
    lib_env->regs[0] = a0;
    lib_env->regs[1] = a1;
    lib_env->regs[2] = a2;
    lib_env->regs[3] = a3;
    lib_env->regs[14] = return_trampoline_addr; /* LR */
    lib_env->regs[15] = func_addr & ~1u;        /* PC (strip Thumb bit) */

    /* Set Thumb mode based on bit 0 of function address */
    if (func_addr & 1) {
        lib_env->thumb = 1;
        cpsr_write(lib_env, cpsr_read(lib_env) | CPSR_T,
                   CPSR_EXEC, CPSRWriteByInstr);
    } else {
        lib_env->thumb = 0;
        cpsr_write(lib_env, cpsr_read(lib_env) & ~CPSR_T,
                   CPSR_EXEC, CPSRWriteByInstr);
    }

    /* Run until we hit the return trampoline */
    for (;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        qemu_process_cpu_events(cs);

        switch (trapnr) {
        case EXCP_UDEF:
        case EXCP_NOCP:
        case EXCP_INVSTATE:
            {
                uint32_t opcode;
                get_user_code_u32(opcode, lib_env->regs[15], lib_env);

                /* Check for our callback UDF encoding */
                if ((opcode & 0xFFF000F0) == 0xE7F000F0) {
                    int hi = (opcode >> 8) & 0xFFF;
                    int lo = opcode & 0xF;
                    int idx = ((hi - 0xAB) << 4) | lo;
                    if (idx >= 0 && idx < num_callbacks &&
                        callback_table[idx]) {
                        callback_table[idx](lib_env);
                        /* Return from callback to caller (LR) */
                        lib_env->regs[15] = lib_env->regs[14];
                        break;
                    }
                }
                /* Unhandled undefined instruction */
                goto done;
            }

        case EXCP_SWI:
            {
                unsigned int n;
                abi_long ret;

                lib_env->eabi = true;
                if (lib_env->thumb) {
                    n = lib_env->regs[7];
                } else {
                    uint32_t insn;
                    get_user_code_u32(insn, lib_env->regs[15] - 4, lib_env);
                    n = insn & 0xffffff;
                    if (n == 0) {
                        n = lib_env->regs[7];
                    } else {
                        n ^= ARM_SYSCALL_BASE;
                        lib_env->eabi = false;
                    }
                }

                if (n > ARM_NR_BASE) {
                    switch (n) {
                    case ARM_NR_cacheflush:
                        lib_env->regs[0] = 0;
                        break;
                    case ARM_NR_set_tls:
                        cpu_set_tls(lib_env, lib_env->regs[0]);
                        lib_env->regs[0] = 0;
                        break;
                    case ARM_NR_get_tls:
                        lib_env->regs[0] = cpu_get_tls(lib_env);
                        break;
                    case ARM_NR_breakpoint:
                        lib_env->regs[15] -= lib_env->thumb ? 2 : 4;
                        goto done;
                    default:
                        if (n < 0xf0800) {
                            lib_env->regs[0] = -TARGET_ENOSYS;
                        } else {
                            goto done;
                        }
                        break;
                    }
                } else {
                    ret = do_syscall(lib_env, n,
                                    lib_env->regs[0], lib_env->regs[1],
                                    lib_env->regs[2], lib_env->regs[3],
                                    lib_env->regs[4], lib_env->regs[5],
                                    0, 0);
                    if (ret == -QEMU_ERESTARTSYS) {
                        lib_env->regs[15] -= lib_env->thumb ? 2 : 4;
                    } else if (ret != -QEMU_ESIGRETURN) {
                        lib_env->regs[0] = ret;
                    }
                }
            }
            break;

        case EXCP_DEBUG:
        case EXCP_BKPT:
            /* Check if we hit the return trampoline */
            if (lib_env->regs[15] == return_trampoline_addr) {
                goto done;
            }
            /* Unknown breakpoint */
            goto done;

        case EXCP_KERNEL_TRAP:
            if (do_kernel_trap(lib_env)) {
                goto done;
            }
            break;

        case EXCP_INTERRUPT:
            /* Signals should be handled below */
            break;

        case EXCP_YIELD:
            break;

        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;

        case EXCP_PREFETCH_ABORT:
        case EXCP_DATA_ABORT:
            /* Fatal for library-mode calls */
            goto done;

        default:
            goto done;
        }

        process_pending_signals(lib_env);
    }

done:
    {
        uint32_t result = lib_env->regs[0];

        /* Restore saved register state */
        memcpy(lib_env->regs, saved_regs, sizeof(saved_regs));
        cpsr_write(lib_env, saved_cpsr, CPSR_USER | CPSR_EXEC,
                   CPSRWriteByInstr);

        return result;
    }
}

uint32_t qemu_arm_guest_alloc(size_t size)
{
    uint32_t addr;

    if (!lib_initialized) {
        return 0;
    }

    addr = target_mmap(0, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == (uint32_t)-1) {
        return 0;
    }
    return addr;
}

void *qemu_arm_g2h(uint32_t guest_addr)
{
    return g2h_untagged(guest_addr);
}

uint32_t qemu_arm_register_callback(qemu_arm_callback_fn callback)
{
    if (num_callbacks >= MAX_CALLBACKS || !lib_initialized) {
        return 0;
    }

    int idx = num_callbacks++;
    callback_table[idx] = callback;
    return callback_trampoline_base + idx * 4;
}

uint32_t qemu_arm_get_reg(int reg)
{
    if (!lib_initialized || reg < 0 || reg > 15) {
        return 0;
    }
    return lib_env->regs[reg];
}

void qemu_arm_set_reg(int reg, uint32_t value)
{
    if (!lib_initialized || reg < 0 || reg > 15) {
        return;
    }
    lib_env->regs[reg] = value;
}

void qemu_arm_cleanup(void)
{
    if (!lib_initialized) {
        return;
    }

    /* TODO: proper cleanup of CPU state, TCG buffers, mapped pages */
    lib_initialized = false;
    lib_cpu = NULL;
    lib_env = NULL;
    lib_ts = NULL;
}
