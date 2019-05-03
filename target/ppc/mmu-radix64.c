/*
 *  PowerPC Radix MMU mulation helpers for QEMU.
 *
 *  Copyright (c) 2016 Suraj Jitindar Singh, IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "sysemu/kvm.h"
#include "kvm_ppc.h"
#include "exec/log.h"
#include "mmu-radix64.h"
#include "mmu-book3s-v3.h"

static inline bool ppc_radix64_hw_rc_updates(CPUPPCState *env)
{
#ifdef CONFIG_ATOMIC64
    return true;
#else
    return !qemu_tcg_mttcg_enabled();
#endif
}

static bool ppc_radix64_get_fully_qualified_addr(CPUPPCState *env, vaddr eaddr,
                                                 uint64_t *lpid, uint64_t *pid)
{
    if (msr_hv) { /* MSR[HV] -> Hypervisor/bare metal */
        switch (eaddr & R_EADDR_QUADRANT) {
        case R_EADDR_QUADRANT0:
            *lpid = 0;
            *pid = env->spr[SPR_BOOKS_PID];
            break;
        case R_EADDR_QUADRANT1:
            *lpid = env->spr[SPR_LPIDR];
            *pid = env->spr[SPR_BOOKS_PID];
            break;
        case R_EADDR_QUADRANT2:
            *lpid = env->spr[SPR_LPIDR];
            *pid = 0;
            break;
        case R_EADDR_QUADRANT3:
            *lpid = 0;
            *pid = 0;
            break;
        }
    } else {  /* !MSR[HV] -> Guest */
        switch (eaddr & R_EADDR_QUADRANT) {
        case R_EADDR_QUADRANT0: /* Guest application */
            *lpid = env->spr[SPR_LPIDR];
            *pid = env->spr[SPR_BOOKS_PID];
            break;
        case R_EADDR_QUADRANT1: /* Illegal */
        case R_EADDR_QUADRANT2:
            return false;
        case R_EADDR_QUADRANT3: /* Guest OS */
            *lpid = env->spr[SPR_LPIDR];
            *pid = 0; /* pid set to 0 -> addresses guest operating system */
            break;
        }
    }

    return true;
}

static void ppc_radix64_raise_segi(PowerPCCPU *cpu, int rwx, vaddr eaddr)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    if (rwx == 2) { /* Instruction Segment Interrupt */
        cs->exception_index = POWERPC_EXCP_ISEG;
    } else { /* Data Segment Interrupt */
        cs->exception_index = POWERPC_EXCP_DSEG;
        env->spr[SPR_DAR] = eaddr;
    }
    env->error_code = 0;
}

static void ppc_radix64_raise_si(PowerPCCPU *cpu, int rwx, vaddr eaddr,
                                uint32_t cause)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    if (rwx == 2) { /* Instruction Storage Interrupt */
        cs->exception_index = POWERPC_EXCP_ISI;
        env->error_code = cause;
    } else { /* Data Storage Interrupt */
        cs->exception_index = POWERPC_EXCP_DSI;
        if (rwx == 1) { /* Write -> Store */
            cause |= DSISR_ISSTORE;
        }
        env->spr[SPR_DSISR] = cause;
        env->spr[SPR_DAR] = eaddr;
        env->error_code = 0;
    }
}

static void ppc_radix64_raise_hsi(PowerPCCPU *cpu, int rwx, vaddr eaddr,
                                  hwaddr g_raddr, uint32_t cause)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    if (rwx == 2) { /* H Instruction Storage Interrupt */
        cs->exception_index = POWERPC_EXCP_HISI;
        env->spr[SPR_ASDR] = g_raddr;
        env->error_code = cause;
    } else { /* H Data Storage Interrupt */
        cs->exception_index = POWERPC_EXCP_HDSI;
        if (rwx == 1) { /* Write -> Store */
            cause |= DSISR_ISSTORE;
        }
        env->spr[SPR_HDSISR] = cause;
        env->spr[SPR_HDAR] = eaddr;
        env->spr[SPR_ASDR] = g_raddr;
        env->error_code = 0;
    }
}

static bool ppc_radix64_check_prot(PowerPCCPU *cpu, int rwx, uint64_t pte,
                                   int *fault_cause, int *prot,
                                   bool partition_scoped)
{
    CPUPPCState *env = &cpu->env;
    const int need_prot[] = { PAGE_READ, PAGE_WRITE, PAGE_EXEC };

    /* Check Page Attributes (pte58:59) */
    if (((pte & R_PTE_ATT) == R_PTE_ATT_NI_IO) && (rwx == 2)) {
        /*
         * Radix PTE entries with the non-idempotent I/O attribute are treated
         * as guarded storage
         */
        *fault_cause |= SRR1_NOEXEC_GUARD;
        return true;
    }

    /* Determine permissions allowed by Encoded Access Authority */
    if (!partition_scoped && (pte & R_PTE_EAA_PRIV) && msr_pr) {
        *prot = 0;
    } else if (msr_pr || (pte & R_PTE_EAA_PRIV) || partition_scoped) {
        *prot = ppc_radix64_get_prot_eaa(pte);
    } else { /* !msr_pr && !(pte & R_PTE_EAA_PRIV) && !partition_scoped */
        *prot = ppc_radix64_get_prot_eaa(pte);
        *prot &= ppc_radix64_get_prot_amr(cpu); /* Least combined permissions */
    }

    /* Check if requested access type is allowed */
    if (need_prot[rwx] & ~(*prot)) { /* Page Protected for that Access */
        *fault_cause |= DSISR_PROTFAULT;
        return true;
    }

    /* Check RC bits if necessary */
    if (!ppc_radix64_hw_rc_updates(env)) {
        if (!(pte & R_PTE_R) || ((rwx == 1) && !(pte & R_PTE_C))) {
            *fault_cause |= DSISR_ATOMIC_RC;
            return true;
        }
    }

    return false;
}

static uint64_t ppc_radix64_set_rc(PowerPCCPU *cpu, int rwx, uint64_t pte, hwaddr pte_addr)
{
    CPUState *cs = CPU(cpu);
    uint64_t npte;

    npte = pte | R_PTE_R; /* Always set reference bit */

    if (rwx == 1) { /* Store/Write */
        npte |= R_PTE_C; /* Set change bit */
    }
    if (pte == npte) {
        return pte;
    }

#ifdef CONFIG_ATOMIC64
    if (qemu_tcg_mttcg_enabled()) {
        uint64_t old_be = cpu_to_be32(pte);
        uint64_t new_be = cpu_to_be32(npte);
        MemTxResult result;
        uint64_t old_ret;

        old_ret = address_space_cmpxchgq_notdirty(cs->as, pte_addr,
                                                  old_be, new_be,
                                                  MEMTXATTRS_UNSPECIFIED,
                                                  &result);
        if (result == MEMTX_OK) {
            if (old_ret != old_be && old_ret != new_be) {
                return 0;
            }
            return npte;
        }

        /* Do we need to support this case where PTEs aren't in RAM ?
         *
         * For now fallback to non-atomic case
         */
    }
#endif

    stq_phys(cs->as, pte_addr, npte);
    return npte;
}

static uint64_t ppc_radix64_next_level(PowerPCCPU *cpu, vaddr eaddr,
                                       uint64_t *pte_addr, uint64_t *nls,
                                       int *psize, int *fault_cause)
{
    CPUState *cs = CPU(cpu);
    uint64_t index, pde;

    if (*nls < 5) { /* Directory maps less than 2**5 entries */
        *fault_cause |= DSISR_R_BADCONFIG;
        return 0;
    }

    /* Read page <directory/table> entry from guest address space */
    pde = ldq_phys(cs->as, *pte_addr);
    if (!(pde & R_PTE_VALID)) {         /* Invalid Entry */
        *fault_cause |= DSISR_NOPTE;
        return 0;
    }

    *psize -= *nls;
    if (!(pde & R_PTE_LEAF)) { /* Prepare for next iteration */
        *nls = pde & R_PDE_NLS;
        index = eaddr >> (*psize - *nls);       /* Shift */
        index &= ((1UL << *nls) - 1);           /* Mask */
        *pte_addr = (pde & R_PDE_NLB) + (index * sizeof(pde));
    }
    return pde;
}

static uint64_t ppc_radix64_walk_tree(PowerPCCPU *cpu, vaddr eaddr,
                                      uint64_t base_addr, uint64_t nls,
                                      hwaddr *raddr, int *psize,
                                      int *fault_cause, hwaddr *pte_addr)
{
    uint64_t index, pde;

    index = eaddr >> (*psize - nls);    /* Shift */
    index &= ((1UL << nls) - 1);       /* Mask */
    *pte_addr = base_addr + (index * sizeof(pde));
    do {
        pde = ppc_radix64_next_level(cpu, eaddr, pte_addr, &nls, psize,
                                     fault_cause);
    } while ((pde & R_PTE_VALID) && !(pde & R_PTE_LEAF));

    /* Did we find a valid leaf? */
    if ((pde & R_PTE_VALID) && (pde & R_PTE_LEAF)) {
        uint64_t rpn = pde & R_PTE_RPN;
        uint64_t mask = (1UL << *psize) - 1;

        /* Or high bits of rpn and low bits to ea to form whole real addr */
        *raddr = (rpn & ~mask) | (eaddr & mask);
    }

    return pde;
}

static int ppc_radix64_partition_scoped_xlate(PowerPCCPU *cpu, int rwx,
                                              vaddr eaddr, hwaddr g_raddr,
                                              ppc_v3_pate_t pate,
                                              hwaddr *h_raddr, int *h_prot,
                                              int *h_page_size, bool pde_addr,
                                              bool cause_excp)
{
    CPUPPCState *env = &cpu->env;
    int fault_cause = 0;
    hwaddr pte_addr;
    uint64_t pte;

restart:
    *h_page_size = PRTBE_R_GET_RTS(pate.dw0);
    pte = ppc_radix64_walk_tree(cpu, g_raddr, pate.dw0 & PRTBE_R_RPDB,
                                pate.dw0 & PRTBE_R_RPDS, h_raddr, h_page_size,
                                &fault_cause, &pte_addr);
    /* No valid pte or access denied due to protection */
    if (!(pte & R_PTE_VALID) ||
            ppc_radix64_check_prot(cpu, rwx, pte, &fault_cause, h_prot, 1)) {
        if (pde_addr) /* address being translated was that of a guest pde */
            fault_cause |= DSISR_PRTABLE_FAULT;
        if (cause_excp)
            ppc_radix64_raise_hsi(cpu, rwx, eaddr, g_raddr, fault_cause);
        return 1;
    }

    /* Update Reference and Change Bits */
    if (ppc_radix64_hw_rc_updates(env)) {
        pte = ppc_radix64_set_rc(cpu, rwx, pte, pte_addr);
        if (!pte) {
            goto restart;
        }
    }

    /* If the page doesn't have C, treat it as read only */
    if (!(pte & R_PTE_C))
        *h_prot &= ~PAGE_WRITE;

    return 0;
}

static int ppc_radix64_process_scoped_xlate(PowerPCCPU *cpu, int rwx,
                                            vaddr eaddr, uint64_t lpid, uint64_t pid,
                                            ppc_v3_pate_t pate, hwaddr *g_raddr,
                                            int *g_prot, int *g_page_size,
                                            bool cause_excp)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;
    uint64_t offset, size, prtbe_addr, prtbe0, base_addr, nls, index, pte;
    int fault_cause = 0, h_page_size, h_prot, ret;
    hwaddr h_raddr, pte_addr;

    /* Index Process Table by PID to Find Corresponding Process Table Entry */
    offset = pid * sizeof(struct prtb_entry);
    size = 1ULL << ((pate.dw1 & PATE1_R_PRTS) + 12);
    if (offset >= size) {
        /* offset exceeds size of the process table */
        if (cause_excp)
            ppc_radix64_raise_si(cpu, rwx, eaddr, DSISR_NOPTE);
        return 1;
    }
    prtbe_addr = (pate.dw1 & PATE1_R_PRTB) + offset;
    /* address subject to partition scoped translation */
    if (cpu->vhyp && (lpid == 0)) {
        prtbe0 = ldq_phys(cs->as, prtbe_addr);
    } else {
        ret = ppc_radix64_partition_scoped_xlate(cpu, 0, eaddr, prtbe_addr,
                                                 pate, &h_raddr, &h_prot,
                                                 &h_page_size, 1, 1);
        if (ret)
            return ret;
        prtbe0 = ldq_phys(cs->as, h_raddr);
    }

    /* Walk Radix Tree from Process Table Entry to Convert EA to RA */
restart:
    *g_page_size = PRTBE_R_GET_RTS(prtbe0);
    base_addr = prtbe0 & PRTBE_R_RPDB;
    nls = prtbe0 & PRTBE_R_RPDS;
    if (msr_hv || (cpu->vhyp && (lpid == 0))) {
        /* Can treat process tree addresses as real addresses */
        pte = ppc_radix64_walk_tree(cpu, eaddr & R_EADDR_MASK, base_addr, nls,
                                    g_raddr, g_page_size, &fault_cause,
                                    &pte_addr);
    } else {
        index = (eaddr & R_EADDR_MASK) >> (*g_page_size - nls); /* Shift */
        index &= ((1UL << nls) - 1);                            /* Mask */
        pte_addr = base_addr + (index * sizeof(pte));

        /* Each process tree address subject to partition scoped translation */
        do {
            ret = ppc_radix64_partition_scoped_xlate(cpu, 0, eaddr, pte_addr,
                                                     pate, &h_raddr, &h_prot,
                                                     &h_page_size, 1, 1);
            if (ret)
                return ret;

            pte = ppc_radix64_next_level(cpu, eaddr & R_EADDR_MASK, &h_raddr,
                                         &nls, g_page_size, &fault_cause);
            pte_addr = h_raddr;
        } while ((pte & R_PTE_VALID) && !(pte & R_PTE_LEAF));

        /* Did we find a valid leaf? */
        if ((pte & R_PTE_VALID) && (pte & R_PTE_LEAF)) {
            uint64_t rpn = pte & R_PTE_RPN;
            uint64_t mask = (1UL << *g_page_size) - 1;

            /* Or high bits of rpn and low bits to ea to form whole real addr */
            *g_raddr = (rpn & ~mask) | (eaddr & mask);
        }
    }

    if (!(pte & R_PTE_VALID) ||
            ppc_radix64_check_prot(cpu, rwx, pte, &fault_cause, g_prot, 0)) {
        /* No valid pte or access denied due to protection */
        if (cause_excp)
            ppc_radix64_raise_si(cpu, rwx, eaddr, fault_cause);
        return 1;
    }

    /* Update Reference and Change Bits */
    if (ppc_radix64_hw_rc_updates(env)) {
        pte = ppc_radix64_set_rc(cpu, rwx, pte, pte_addr);
        if (!pte)
            goto restart;
    }

    /* If the page doesn't have C, treat it as read only */
    if (!(pte & R_PTE_C))
        *g_prot &= ~PAGE_WRITE;

    return 0;
}

static bool validate_pate(PowerPCCPU *cpu, uint64_t lpid, ppc_v3_pate_t *pate)
{
    CPUPPCState *env = &cpu->env;

    if (!(pate->dw0 & PATE0_HR)) {
        return false;
    }
    if (lpid == 0 && !msr_hv) {
        return false;
    }
    if ((pate->dw0 & PATE1_R_PRTS) < 5)
        return false;
    /* More checks ... */
    return true;
}

static int ppc_radix64_xlate(PowerPCCPU *cpu, vaddr eaddr, int rwx,
                             uint64_t lpid, uint64_t pid, bool relocation,
                             hwaddr *raddr, int *psizep, int *protp,
                             bool cause_excp)
{
    CPUPPCState *env = &cpu->env;
    ppc_v3_pate_t pate;
    int psize, prot;
    hwaddr g_raddr;

    *psizep = INT_MAX;
    *protp = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    /* Get Process Table */
    if (cpu->vhyp && (lpid == 0)) {
        PPCVirtualHypervisorClass *vhc;
        vhc = PPC_VIRTUAL_HYPERVISOR_GET_CLASS(cpu->vhyp);
        vhc->get_pate(cpu->vhyp, &pate);
    } else {
        if (!ppc64_v3_get_pate(cpu, lpid, &pate)) {
            if (cause_excp)
                ppc_radix64_raise_si(cpu, rwx, eaddr, DSISR_NOPTE);
            return 1;
        }
        if (!validate_pate(cpu, lpid, &pate)) {
            if (cause_excp)
                ppc_radix64_raise_si(cpu, rwx, eaddr, DSISR_R_BADCONFIG);
            return 1;
        }
    }

    /*
     * Radix tree translation is a 2 step translation process:
     * 1. Process Scoped translation - Guest Eff Addr -> Guest Real Addr
     * 2. Partition Scoped translation - Guest Real Addr -> Host Real Addr
     *
     *                                       MSR[HV]
     *            -----------------------------------------------
     *            |             |     HV = 0    |     HV = 1    |
     *            -----------------------------------------------
     *            | Relocation  |   Partition   |      No       |
     *            | = Off       |    Scoped     |  Translation  |
     * Relocation -----------------------------------------------
     *            | Relocation  |  Partition &  |    Process    |
     *            | = On        |Process Scoped |    Scoped     |
     *            -----------------------------------------------
     */

    /* Perform process scoped translation if relocation enabled */
    if (relocation) {
        int ret = ppc_radix64_process_scoped_xlate(cpu, rwx, eaddr, lpid, pid,
                                                   pate, &g_raddr, &prot,
                                                   &psize, cause_excp);
        if (ret)
            return ret;
        *psizep = MIN(*psizep, psize);
        *protp &= prot;
    } else {
        g_raddr = eaddr & R_EADDR_MASK;
    }

    /* Perform partition scoped xlate if !HV or HV access to quadrants 1 or 2 */
    if ((lpid != 0) || (!cpu->vhyp && !msr_hv)) {
        int ret = ppc_radix64_partition_scoped_xlate(cpu, rwx, eaddr, g_raddr,
                                                     pate, raddr, &prot, &psize,
                                                     0, cause_excp);
        if (ret)
            return ret;
        *psizep = MIN(*psizep, psize);
        *protp &= prot;
    } else {
        *raddr = g_raddr;
    }

    return 0;
}

int ppc_radix64_handle_mmu_fault(PowerPCCPU *cpu, vaddr eaddr, int rwx,
                                 int mmu_idx)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;
    uint64_t pid, lpid = env->spr[SPR_LPIDR];
    int psize, prot;
    bool relocation;
    hwaddr raddr;

    assert(!(msr_hv && cpu->vhyp));
    assert((rwx == 0) || (rwx == 1) || (rwx == 2));

    relocation = ((rwx == 2) && (msr_ir == 1)) || ((rwx != 2) && (msr_dr == 1));
    /* HV or virtual hypervisor Real Mode Access */
    if (!relocation && (msr_hv || (cpu->vhyp && (lpid == 0)))) {
        /* In real mode top 4 effective addr bits (mostly) ignored */
        raddr = eaddr & 0x0FFFFFFFFFFFFFFFULL;

        tlb_set_page(cs, eaddr & TARGET_PAGE_MASK, raddr & TARGET_PAGE_MASK,
                     PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
                     TARGET_PAGE_SIZE);
        return 0;
    }

    /* Check UPRT (we avoid the check in real mode to deal with
     * transitional states during kexec.
     */
    if (!ppc64_use_proc_tbl(cpu)) {
        qemu_log_mask(LOG_GUEST_ERROR, "LPCR:UPRT not set in radix mode ! LPCR=%016lx\n",
                      env->spr[SPR_LPCR]);
    }

    /* Virtual Mode Access - get the fully qualified address */
    if (!ppc_radix64_get_fully_qualified_addr(env, eaddr, &lpid, &pid)) {
        ppc_radix64_raise_segi(cpu, rwx, eaddr);
        return 1;
    }

    /* Translate eaddr to raddr (where raddr is addr qemu needs for access) */
    if (ppc_radix64_xlate(cpu, eaddr, rwx, lpid, pid, relocation, &raddr,
                          &psize, &prot, 1)) {
        return 1;
    }

    tlb_set_page(cs, eaddr & TARGET_PAGE_MASK, raddr & TARGET_PAGE_MASK,
                 prot, mmu_idx, 1UL << psize);
    return 0;
}

hwaddr ppc_radix64_get_phys_page_debug(PowerPCCPU *cpu, target_ulong eaddr)
{
    CPUPPCState *env = &cpu->env;
    uint64_t lpid = 0, pid = 0;
    int psize, prot;
    hwaddr raddr;

    /* Handle Real Mode */
    if ((msr_dr == 0) && (msr_hv || (cpu->vhyp && (lpid == 0)))) {
        /* In real mode top 4 effective addr bits (mostly) ignored */
        return eaddr & 0x0FFFFFFFFFFFFFFFULL;
    }

    /* Virtual Mode Access - get the fully qualified address */
    if (!ppc_radix64_get_fully_qualified_addr(env, eaddr, &lpid, &pid)) {
        return -1;
    }

    if (ppc_radix64_xlate(cpu, eaddr, 0, lpid, pid, msr_dr, &raddr, &psize,
                          &prot, 0)) {
        return -1;
    }

    return raddr & TARGET_PAGE_MASK;
}
