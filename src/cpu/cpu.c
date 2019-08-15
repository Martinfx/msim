/*
 * Copyright (c) 2000-2008 Viliam Holub
 * All rights reserved.
 *
 * Distributed under the terms of GPL.
 *
 *
 *  Processor simulation
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "../device/machine.h"
#include "../debug/debug.h"
#include "../debug/breakpoint.h"
#include "../debug/gdb.h"
#include "../io/output.h"
#include "../check.h"
#include "../main.h"
#include "../mtypes.h"
#include "../endi.h"
#include "../text.h"
#include "../env.h"
#include "../fault.h"
#include "../utils.h"
#include "cpu.h"

/** Initial state */
#define HARD_RESET_STATUS         (cp0_status_erl_mask | cp0_status_bev_mask)
#define HARD_RESET_START_ADDRESS  0xbfc00000U
#define HARD_RESET_PROC_ID        0x00000400U
#define HARD_RESET_CAUSE          0
#define HARD_RESET_WATCHLO        0
#define HARD_RESET_WATCHHI        0
#define HARD_RESET_CONFIG         0
#define HARD_RESET_RANDOM         47
#define HARD_RESET_WIRED          0

#define EXCEPTION_OFFSET  0x180

#define TRAP(x) \
	if (x) \
		res = excTr;

/** TLB lookup result */
typedef enum {
	TLBL_OK,
	TLBL_REFILL,
	TLBL_INVALID,
	TLBL_MODIFIED
} tlb_look_t;

/** Memory access mode */
typedef enum {
	AM_FETCH,
	AM_READ,
	AM_WRITE
} acc_mode_t;

typedef struct {
	uint32_t mask;
	uint32_t shift;
} shift_tab_t;

static shift_tab_t shift_tab_left[] = {
	{ 0x00ffffffU, 24 },
	{ 0x0000ffffU, 16 },
	{ 0x000000ffU, 8 },
	{ 0x00000000U, 0 }
};

static shift_tab_t shift_tab_right[] = {
	{ 0x00000000U, 0 },
	{ 0xff000000U, 8 },
	{ 0xffff0000U, 16 },
	{ 0xffffff00U, 24 }
};

static shift_tab_t shift_tab_left_store[] = {
	{ 0xffffff00U, 24 },
	{ 0xffff0000U, 16 },
	{ 0xff000000U, 8 },
	{ 0x00000000U, 0 }
};

static shift_tab_t shift_tab_right_store[] = {
	{ 0x00000000U, 0 },
	{ 0x000000ffU, 8 },
	{ 0x0000ffffU, 16 },
	{ 0x00ffffffU, 24 }
};

/** Initialize simulation environment
 *
 */
void cpu_init(cpu_t *cpu, size_t procno)
{
	/* Initially set all members to zero */
	memset(cpu, 0, sizeof(cpu_t));
	
	cpu->procno = procno;
	cpu_set_pc(cpu, HARD_RESET_START_ADDRESS);
	
	/* Inicialize cp0 registers */
	cpu->cp0[cp0_Config] = HARD_RESET_CONFIG;
	cpu->cp0[cp0_Random] = HARD_RESET_RANDOM;
	cpu->cp0[cp0_Wired] = HARD_RESET_WIRED;
	cpu->cp0[cp0_PRId] = HARD_RESET_PROC_ID;
	
	/* Initial status value */
	cpu->cp0[cp0_Status] = HARD_RESET_STATUS;
	
	cpu->cp0[cp0_Cause] = HARD_RESET_CAUSE;
	cpu->cp0[cp0_WatchLo] = HARD_RESET_WATCHLO;
	cpu->cp0[cp0_WatchHi] = HARD_RESET_WATCHHI;
	
	/* Breakpoints */
	list_init(&cpu->bps);
}

/** Set the PC register
 *
 */
void cpu_set_pc(cpu_t *cpu, ptr_t value)
{
	cpu->pc = value;
	cpu->pc_next = value + 4;
}

/** Address traslation through the TLB table
 *
 * See tlb_look_t definition
 *
 */
static tlb_look_t tlb_look(cpu_t *cpu, ptr_t *addr, bool wr)
{
	/* Ignore TLB test */
	if (cp0_status_ts(cpu) == 1)
		return TLBL_OK;
	
	unsigned int hint = cpu->tlb_hint;
	unsigned int i;
	
	/* Look for the TBL hit */
	for (i = 0; i < TLB_ENTRIES; i++) {
		tlb_entry_t *entry = &cpu->tlb[(i + hint) % TLB_ENTRIES];
		
		/* TLB hit? */
		if ((*addr & entry->mask) == entry->vpn2) {
			/* Test ASID */
			if ((!entry->global) && (entry->asid != cp0_entryhi_asid(cpu)))
				continue;
			
			/* Calculate subpage */
			uint32_t smask = (entry->mask >> 1) | SBIT;
			unsigned int subpage =
			    ((*addr & entry->mask) < (*addr & smask)) ? 1 : 0;
			
			/* Test valid & dirty */
			if (!entry->pg[subpage].valid)
				return TLBL_INVALID;
			
			if ((wr) && (!entry->pg[subpage].dirty))
				return TLBL_MODIFIED;
			
			/* Make address */
			ptr_t amask = *addr & (~smask);
			*addr = amask | (entry->pg[subpage].pfn & smask);
			
			/* Update optimization hint */
			cpu->tlb_hint = i;
			
			return TLBL_OK;
		}
	}
	
	return TLBL_REFILL;
}

/** Fill up cp0 registers with specified address
 *
 */
static void fill_tlb_error(cpu_t *cpu, ptr_t addr)
{
	cp0_badvaddr(cpu) = addr;
	
	cp0_context(cpu) &= cp0_context_ptebase_mask;
	cp0_context(cpu) |= (addr >> cp0_context_addr_shift) & ~cp0_context_res1_mask;
	
	cp0_entryhi(cpu) &= cp0_entryhi_asid_mask;
	cp0_entryhi(cpu) |= addr & cp0_entryhi_vpn2_mask;
}

/** Fill registers as the Address error exception occures
 *
 */
static void fill_addr_error(cpu_t *cpu, ptr_t addr, bool noisy)
{
	if (noisy) {
		cp0_badvaddr(cpu) = addr;
		cp0_context(cpu) &= ~cp0_context_badvpn2_mask;  /* Undefined */
		cp0_entryhi(cpu) &= ~cp0_entryhi_vpn2_mask;     /* Undefined */
	}
}

/** Search through TLB and generates apropriate exception
 *
 */
static exc_t tlb_hit(cpu_t *cpu, ptr_t *addr, bool wr, bool noisy)
{
	switch (tlb_look(cpu, addr, wr)) {
	case TLBL_OK:
		break;
	case TLBL_REFILL:
		if (noisy) {
			cpu->tlb_refill++;
			fill_tlb_error(cpu, *addr);
		}
		
		return excTLBR;
	case TLBL_INVALID:
		if (noisy) {
			cpu->tlb_invalid++;
			fill_tlb_error(cpu, *addr);
		}
		
		return excTLB;
	case TLBL_MODIFIED:
		if (noisy) {
			cpu->tlb_modified++;
			fill_tlb_error(cpu, *addr);
		}
		
		return excMod;
	}
	
	return excNone;
}

/** The user mode address conversion
 *
 */
static exc_t convert_addr_user(cpu_t *cpu, ptr_t *addr, bool wr, bool noisy)
{
	/* Test bit 31 or lookup in TLB */
	if ((*addr & SBIT) != 0) {
		fill_addr_error(cpu, *addr, noisy);
		return excAddrError;
	}
	
	/* useg */
	return tlb_hit(cpu, addr, wr, noisy);
}

/** The supervisor mode address conversion
 *
 */
static exc_t convert_addr_supervisor(cpu_t *cpu, ptr_t *addr, bool wr,
    bool noisy)
{
	if (*addr < 0x80000000U)  /* suseg */
		return tlb_hit(cpu, addr, wr, noisy);
	
	if (*addr < 0xc0000000U) {
		fill_addr_error(cpu, *addr, noisy);
		return excAddrError;
	}
	
	if (*addr < 0xe0000000U)  /* ssseg */
		return tlb_hit(cpu, addr, wr, noisy);
	
	/* *addr > 0xe0000000U */
	fill_addr_error(cpu, *addr, noisy);
	return excAddrError;
}

/** The kernel mode address conversion
 *
 */
static exc_t convert_addr_kernel(cpu_t *cpu, ptr_t *addr, bool wr, bool noisy)
{
	if (*addr < 0x80000000U) {  /* kuseg */
		if (!cp0_status_erl(cpu))
			return tlb_hit(cpu, addr, wr, noisy);
		
		return excNone;
	}
	
	if (*addr < 0xa0000000U) {  /* kseg0 */
		*addr -= 0x80000000;
		return excNone;
	}
	
	if (*addr < 0xc0000000U) {  /* kseg1 */
		*addr -= 0xa0000000;
		return excNone;
	}
	
	if (*addr < 0xe0000000U)  /* kseg2 */
		return tlb_hit(cpu, addr, wr, noisy);
	
	/* *addr > 0xe0000000U (kseg3) */
	return tlb_hit(cpu, addr, wr, noisy);
}

/** The conversion of virtual addresses
 *
 * @param write Write access.
 * @param noisy Fill apropriate processor registers
 *              if the address is incorrect.
 *
 */
exc_t convert_addr(cpu_t *cpu, ptr_t *addr, bool write, bool noisy)
{
	/* Test the type of the translation */
	if ((cp0_status_ksu(cpu) == 2) && (!cp0_status_exl(cpu)) &&
	    (!cp0_status_erl(cpu)))
		return convert_addr_user(cpu, addr, write, noisy);
	
	if ((cp0_status_ksu(cpu) == 1) && (!cp0_status_exl(cpu)) &&
	    (!cp0_status_erl(cpu)))
		return convert_addr_supervisor(cpu, addr, write, noisy);
	
	return convert_addr_kernel(cpu, addr, write, noisy);
}

/** Test for correct alignment
 *
 * Fill BadVAddr if the alignment is not correct.
 *
 */
static exc_t mem_align_test(cpu_t *cpu, ptr_t addr, len_t size, bool noisy)
{
	if (((size == 2) && (addr & 1)) || ((size == 4) && (addr & 3))) {
		fill_addr_error(cpu, addr, noisy);
		return excAddrError;
	}
	
	return excNone;
}

/** Access the virtual memory
 *
 * The operation (read/write) is specified via the wr parameter.
 * This routine does not specify the exception type.
 *
 * If the exception occures, the value does not change.
 *
 * @param mode  Memory access mode
 * @param addr  Memory address
 * @param size  Argument size in bytes (1, 2, 4)
 * @param value The value which has to be written/read to
 * @param noisy Generate exception in case of invalid operation
 *
 */
static exc_t acc_mem(cpu_t *cpu, acc_mode_t mode, ptr_t addr, len_t size,
    uint32_t *value, bool noisy)
{
	exc_t res = mem_align_test(cpu, addr, size, noisy);
	
	if (res == excNone) {
		res = convert_addr(cpu, &addr, mode == AM_WRITE, noisy);
		
		/* Check for watched address */
		if (((cp0_watchlo_r(cpu)) && (mode == AM_READ))
		    || ((cp0_watchlo_w(cpu)) && (mode == AM_WRITE))) {
			
			/* The matching is done on
			   8-byte aligned addresses */
			
			if (cpu->waddr == (addr >> 3)) {
				/*
				 * If EXL is set, the exception has to be postponed,
				 * the memory access should (probably) proceed.
				 */
				if (cp0_status_exl(cpu) == 1) {
					cpu->wpending = true;
					cpu->wexcaddr = cpu->pc;
				} else
					return excWATCH;
			}
		}
		
		if (res == excNone) {
			if (mode == AM_WRITE)
				mem_write(cpu, addr, *value, size, true);
			else
				*value = mem_read(cpu, addr, size, true);
		}
	}
	
	return res;
}

/** Preform the read access from the virtual memory
 *
 * Does not change the value if an exception occurs.
 *
 */
exc_t cpu_read_mem(cpu_t *cpu, ptr_t addr, len_t size, uint32_t *value,
    bool noisy)
{
	switch (acc_mem(cpu, AM_READ, addr, size, value, noisy)) {
	case excAddrError:
		return excAdEL;
	case excTLB:
		return excTLBL;
	case excTLBR:
		return excTLBLR;
	case excWATCH:
		return excWATCH;
	default:
		break;
	}
	
	return excNone;
}

/** Preform the fetch access from the virtual memory
 *
 * Does not change the value if an exception occurs.
 * Fetching instructions (on R4000) does not trigger
 * the WATCH exception.
 *
 */
static exc_t cpu_fetch_mem(cpu_t *cpu, ptr_t addr, len_t size, uint32_t *value,
    bool noisy)
{
	switch (acc_mem(cpu, AM_FETCH, addr, size, value, noisy)) {
	case excAddrError:
		return excAdEL;
	case excTLB:
		return excTLBL;
	case excTLBR:
		return excTLBLR;
	default:
		break;
	}
	
	return excNone;
}

/** Perform the write operation to the virtual memory
 *
 */
static exc_t cpu_write_mem(cpu_t *cpu, ptr_t addr, len_t size, uint32_t value,
    bool noisy)
{
	switch (acc_mem(cpu, AM_WRITE, addr, size, &value, noisy)) {
	case excAddrError:
		return excAdES;
	case excTLB:
		return excTLBS;
	case excTLBR:
		return excTLBSR;
	case excMod:
		return excMod;
	case excWATCH:
		return excWATCH;
	case excNone:
		return excNone;
	default:
		die(ERR_INTERN, "Internal error at %s(%u)", __FILE__, __LINE__);
	}
	
	/* Unreachable */
	return excNone;
}

/** Read an instruction
 *
 */
exc_t cpu_read_ins(cpu_t *cpu, ptr_t addr, uint32_t *value, bool noisy)
{
	exc_t res = cpu_fetch_mem(cpu, addr, BITS_32, value, noisy);
	if ((res != excNone) && (cpu->branch == BRANCH_NONE))
		cpu->excaddr = cpu->pc;
	
	return res;
}

/** Assert the specified interrupt
 *
 */
void cpu_interrupt_up(cpu_t *cpu, unsigned int no)
{
	PRE(no < INTR_COUNT);
	
	cp0_cause(cpu) |= 1 << (cp0_cause_ip0_shift + no);
	cpu->intr[no]++;
}

/* Deassert the specified interrupt
 *
 */
void cpu_interrupt_down(cpu_t *cpu, unsigned int no)
{
	PRE(no < INTR_COUNT);
	
	cp0_cause(cpu) &= ~(1 << (cp0_cause_ip0_shift + no));
}

/** Update the copy of registers
 *
 */
static void cpu_update_debug(cpu_t *cpu)
{
	memcpy(cpu->old_regs, cpu->regs, sizeof(cpu->regs));
	memcpy(cpu->old_cp0, cpu->cp0, sizeof(cpu->cp0));
	
	cpu->old_loreg = cpu->loreg;
	cpu->old_hireg = cpu->hireg;
}

/** Perform the multiplication of two integers
 *
 */
static void multiply(cpu_t *cpu, uint32_t a, uint32_t b, bool sign)
{
	/* Quick test */
	if ((a == 0) || (b == 0)) {
		cpu->hireg = 0;
		cpu->loreg = 0;
		return;
	}
	
	if (sign) {
		/* Signed multiplication */
		int64_t r = ((int64_t) ((int32_t) a)) * ((int64_t) ((int32_t) b));
		
		/* Result */
		cpu->loreg = ((uint64_t) r) & 0xffffffffU;
		cpu->hireg = ((uint64_t) r) >> 32;
	} else {
		/* Unsigned multiplication */
		uint64_t r = ((uint64_t) a) * ((uint64_t) b);
		
		/* Result */
		cpu->loreg = r & 0xffffffffU;
		cpu->hireg = r >> 32;
	}
}

/** Write a new entry into the TLB
 *
 * The entry index is determined by either
 * the random register (TLBWR) or index (TLBWI).
 *
 */
static void TLBW(cpu_t *cpu, bool random, exc_t *res)
{
	if ((cp0_status_cu0(cpu) == 1)
	    || ((cp0_status_ksu(cpu) == 0)
	    || (cp0_status_exl(cpu) == 1)
	    || (cp0_status_erl(cpu) == 1))) {
		
		unsigned int index =
		    random ? cp0_random_random(cpu) : cp0_index_index(cpu);
		
		if (index > 47) {
			/*
			 * Undefined behavior, doing nothing complies.
			 * Random is read-only, its index should be always fine.
			 */
			mprintf("\nTLBWI: Invalid value in Index\n");
		} else {
			/* Fill TLB */
			tlb_entry_t *entry = &cpu->tlb[index];
			
			entry->mask = cp0_entryhi_vpn2_mask & ~cp0_pagemask(cpu);
			entry->vpn2 = cp0_entryhi(cpu) & entry->mask;
			entry->global = cp0_entrylo0_g(cpu) & cp0_entrylo1_g(cpu);
			entry->asid = cp0_entryhi_asid(cpu);
			
			entry->pg[0].pfn = cp0_entrylo0_pfn(cpu) << 12;
			entry->pg[0].cohh = cp0_entrylo0_c(cpu);
			entry->pg[0].dirty = cp0_entrylo0_d(cpu);
			entry->pg[0].valid = cp0_entrylo0_v(cpu);
			
			entry->pg[1].pfn = cp0_entrylo1_pfn(cpu) << 12;
			entry->pg[1].cohh = cp0_entrylo1_c(cpu);
			entry->pg[1].dirty = cp0_entrylo1_d(cpu);
			entry->pg[1].valid = cp0_entrylo1_v(cpu);
		}
	} else {
		/* Coprocessor unusable */
		*res = excCpU;
		cp0_cause(cpu) &= ~cp0_cause_ce_mask;
	}
}

/** Execute the instruction specified by the opcode
 *
 * A really huge one, isn't it.
 *
 */
static exc_t execute(cpu_t *cpu, instr_info_t ii)
{
	exc_t res = excNone;
	uint32_t pca = cpu->pc_next + 4;
	
	uint32_t urrs = cpu->regs[ii.rs];
	uint32_t urrt = cpu->regs[ii.rt];
	
	uint32_t utmp32;
	uint32_t utmp32b;
	uint64_t utmp64;
	ptr_t addr;
	
	switch (ii.opcode) {
	
	/*
	 * Aritmetic, logic, shifts
	 */
	
	case opcADD:
		utmp32 = urrs + urrt;
		
		if (!((urrs ^ urrt) & SBIT) && ((urrs ^ utmp32) & SBIT)) {
			res = excOv;
			break;
		}
		
		cpu->regs[ii.rd] = utmp32;
		break;
	case opcADDI:
		utmp32 = urrs + ii.imm;
		
		if (!((urrs ^ ii.imm) & SBIT) && ((ii.imm ^ utmp32) & SBIT)) {
			res = excOv;
			break;
		}
		
		cpu->regs[ii.rt] = utmp32;
		break;
	case opcADDIU:
		cpu->regs[ii.rt] = urrs + ii.imm;
		break;
	case opcADDU:
		cpu->regs[ii.rd] = urrs + urrt;
		break;
	case opcAND:
		cpu->regs[ii.rd] = urrs & urrt;
		break;
	case opcANDI:
		cpu->regs[ii.rt] = urrs & (ii.imm & 0xffffU);
		break;
	case opcCLO:
		utmp32 = 0;
		utmp32b = urrs;
		
		while ((utmp32b & 0x80000000U) && (utmp32 < 32)) {
			utmp32++;
			utmp32b <<= 1;
		}
		
		cpu->regs[ii.rd] = utmp32;
		break;
	case opcCLZ:
		utmp32 = 0;
		utmp32b = urrs;
		
		while ((!(utmp32b & 0x80000000U)) && (utmp32 < 32)) {
			utmp32++;
			utmp32b <<= 1;
		}
		
		cpu->regs[ii.rd] = utmp32;
		break;
	case opcDADD:
	case opcDADDI:
	case opcDADDIU:
	case opcDADDU:
	case opcDDIV:
	case opcDDIVU:
		/* 64-bit instructions */
		res = excRI;
		break;
	case opcDIV:
		if (urrt == 0) {
			cpu->loreg = 0;
			cpu->hireg = 0;
		} else {
			cpu->loreg = (uint32_t) (((int32_t) urrs) / ((int32_t) urrt));
			cpu->hireg = (uint32_t) (((int32_t) urrs) % ((int32_t) urrt));
		}
		break;
	case opcDIVU:
		if (urrt == 0) {
			cpu->loreg = 0;
			cpu->hireg = 0;
		} else {
			cpu->loreg = urrs / urrt;
			cpu->hireg = urrs % urrt;
		}
		break;
	case opcDMULT:
	case opcDMULTU:
	case opcDSLL:
	case opcDSLLV:
	case opcDSLL32:
	case opcDSRA:
	case opcDSRAV:
	case opcDSRA32:
	case opcDSRL:
	case opcDSRLV:
	case opcDSRL32:
	case opcDSUB:
	case opcDSUBU:
		/* 64-bit instructions */
		res = excRI;
		break;
	case opcMADD:
		utmp64 = ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		multiply(cpu, urrs, urrt, true);
		utmp64 += ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		cpu->hireg = utmp64 >> 32;
		cpu->loreg = utmp64 & 0xffffffffU;
		break;
	case opcMADDU:
		utmp64 = ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		multiply(cpu, urrs, urrt, false);
		utmp64 += ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		cpu->hireg = utmp64 >> 32;
		cpu->loreg = utmp64 & 0xffffffffU;
		break;
	case opcMSUB:
		utmp64 = ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		multiply(cpu, urrs, urrt, true);
		utmp64 -= ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		cpu->hireg = utmp64 >> 32;
		cpu->loreg = utmp64 & 0xffffffffU;
		break;
	case opcMSUBU:
		utmp64 = ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		multiply(cpu, urrs, urrt, false);
		utmp64 -= ((uint64_t) cpu->hireg << 32) | cpu->loreg;
		cpu->hireg = utmp64 >> 32;
		cpu->loreg = utmp64 & 0xffffffffU;
		break;
	case opcMUL:
		utmp64 = ((uint64_t) urrs) * ((uint64_t) urrt);
		cpu->regs[ii.rd] = utmp64 & 0xffffffffU;
		break;
	case opcMOVN:
		if (urrt != 0)
			cpu->regs[ii.rd] = urrs;
		break;
	case opcMOVZ:
		if (urrt == 0)
			cpu->regs[ii.rd] = urrs;
		break;
	case opcMULT:
		multiply(cpu, urrs, urrt, true);
		break;
	case opcMULTU:
		multiply(cpu, urrs, urrt, false);
		break;
	case opcNOR:
		cpu->regs[ii.rd] = ~(urrs | urrt);
		break;
	case opcOR:
		cpu->regs[ii.rd] = urrs | urrt;
		break;
	case opcORI:
		cpu->regs[ii.rt] = urrs | (ii.imm & 0xffffU);
		break;
	case opcSLL:
		cpu->regs[ii.rd] = urrt << ii.shift;
		break;
	case opcSLLV:
		cpu->regs[ii.rd] = urrt << (urrs & 0x1fU);
		break;
	case opcSLT:
		cpu->regs[ii.rd] = ((int32_t) urrs) < ((int32_t) urrt);
		break;
	case opcSLTI:
		cpu->regs[ii.rt] = ((int32_t) urrs) < ((int32_t) ii.imm);
		break;
	case opcSLTIU:
		cpu->regs[ii.rt] = urrs < ii.imm;
		break;
	case opcSLTU:
		cpu->regs[ii.rd] = urrs < urrt;
		break;
	case opcSRA:
		cpu->regs[ii.rd] = (uint32_t) (((int32_t) urrt) >> ii.shift);
		break;
	case opcSRAV:
		cpu->regs[ii.rd] = (uint32_t) (((int32_t) urrt) >> (urrs & 0x1fU));
		break;
	case opcSRL:
		cpu->regs[ii.rd] = urrt >> ii.shift;
		break;
	case opcSRLV:
		cpu->regs[ii.rd] = urrt >> (urrs & 0x1fU);
		break;
	case opcSUB:
		utmp32 = urrs - urrt;
		
		if (((urrs ^ urrt) & SBIT) && ((urrs ^ utmp32) & SBIT)) {
			res = excOv;
			break;
		}
		
		cpu->regs[ii.rd] = utmp32;
		break;
	case opcSUBU:
		cpu->regs[ii.rd] = urrs - urrt;
		break;
	case opcXOR:
		cpu->regs[ii.rd] = urrs ^ urrt;
		break;
	case opcXORI:
		cpu->regs[ii.rt] = urrs ^ (ii.imm & 0xffffU);
		break;
	
	/*
	 * Branches and jumps
	 */
	case opcBC0FL:
	case opcBC1FL:
	case opcBC2FL:
	case opcBC3FL:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))) {
			/* Ignore - always false */
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcBC0F:
	case opcBC1F:
	case opcBC2F:
	case opcBC3F:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))) {
			/* Ignore - always false */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcBC0TL:
	case opcBC1TL:
	case opcBC2TL:
	case opcBC3TL:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))) {
			/* Ignore - always true */
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcBC0T:
	case opcBC1T:
	case opcBC2T:
	case opcBC3T:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))) {
			/* Ignore - always true */
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcBEQ:
		if (urrs == urrt) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBEQL:
		if (urrs == urrt) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcBGEZAL:
		cpu->regs[31] = cpu->pc + 8;
		/* no break */
	case opcBGEZ:
		if (!(urrs & SBIT)) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBGEZALL:
		cpu->regs[31] = cpu->pc + 8;
		/* no break */
	case opcBGEZL:
		if (!(urrs & SBIT)) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcBGTZ:
		if (((int32_t) urrs) > 0) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBGTZL:
		if (((int32_t) urrs) > 0) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcBLEZ:
		if (((int32_t) urrs) <= 0) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBLEZL:
		if (((int32_t) urrs) <= 0) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcBLTZAL:
		cpu->regs[31] = cpu->pc_next + 4;
		/* no break */
	case opcBLTZ:
		if (urrs & SBIT) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBLTZALL:
		cpu->regs[31] = cpu->pc_next + 4;
		/* no break */
	case opcBLTZL:
		if (urrs & SBIT) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcBNE:
		if (urrs != urrt) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		}
		break;
	case opcBNEL:
		if (urrs != urrt) {
			pca = cpu->pc_next + (((int32_t) ii.imm) << TARGET_SHIFT);
			cpu->branch = BRANCH_COND;
		} else {
			cpu->pc_next += 4;
			pca = cpu->pc_next + 4;
		}
		break;
	case opcJAL:
		cpu->regs[31] = cpu->pc_next + 4;
		/* no break */
	case opcJ:
		pca = (cpu->pc_next & TARGET_COMB) | (ii.imm << TARGET_SHIFT);
		cpu->branch = BRANCH_COND;
		break;
	case opcJALR:
		cpu->regs[ii.rd] = cpu->pc_next + 4;
		/* no break */
	case opcJR:
		pca = urrs;
		cpu->branch = BRANCH_COND;
		break;
	
	/*
	 * Load, store
	 */
	case opcLB:
		res = cpu_read_mem(cpu, urrs + ((int32_t) ii.imm), BITS_8,
		    &utmp32, true);
		if (res == excNone) {
			cpu->regs[ii.rt] = (utmp32 & 0x80U) ?
			    (utmp32 | 0xffffff00U) : (utmp32 & 0xffU);
		}
		break;
	case opcLBU:
		res = cpu_read_mem(cpu, urrs + ((int32_t) ii.imm), BITS_8,
		    &utmp32, true);
		if (res == excNone)
			cpu->regs[ii.rt] = utmp32 & 0xffU;
		break;
	case opcLD:
	case opcLDL:
	case opcLDR:
		/* 64-bit instructions */
		res = excRI;
		break;
	case opcLH:
		res = cpu_read_mem(cpu, urrs + ((int32_t) ii.imm), BITS_16,
		    &utmp32, true);
		if (res == excNone) {
			cpu->regs[ii.rt] = (utmp32 & 0x8000U) ?
			    (utmp32 | 0xffff0000U) : (utmp32 & 0xffffU);
		}
		break;
	case opcLHU:
		res = cpu_read_mem(cpu, urrs + ((int32_t) ii.imm), BITS_16,
		    &utmp32, true);
		if (res == excNone)
			cpu->regs[ii.rt] = utmp32 & 0xffffU;
		break;
	case opcLL:
		/* Compute virtual target address
		   and issue read operation */
		addr = urrs + ((int32_t) ii.imm);
		res = cpu_read_mem(cpu, addr, BITS_32, &utmp32, true);
		
		if (res == excNone) {  /* If the read operation has been successful */
			/* Store the value */
			cpu->regs[ii.rt] = utmp32;
			
			/* Since we need physical address to track, issue the
			   address conversion. It can't fail now. */
			convert_addr(cpu, &addr, false, false);
			
			/* Register address for tracking. */
			register_sc(cpu);
			cpu->llbit = true;
			cpu->lladdr = addr;
		} else {
			/* Invalid address; Cancel the address tracking */
			unregister_sc(cpu);
			cpu->llbit = false;
		}
		break;
	case opcLLD:
		/* 64-bit instruction */
		res = excRI;
		break;
	case opcLUI:
		cpu->regs[ii.rt] = ii.imm << 16;
		break;
	case opcLW:
		res = cpu_read_mem(cpu, urrs + ((int32_t) ii.imm), BITS_32,
		    &cpu->regs[ii.rt], true);
		break;
	case opcLWL:
		addr = (urrs + ((int32_t) ii.imm)) & ((uint32_t) ~0x03);
		res = cpu_read_mem(cpu, addr, BITS_32, &utmp32, true);
		
		if (res == excNone) {
			unsigned int index = (urrs + ((int32_t) ii.imm)) & 0x03;
			
			cpu->regs[ii.rt] &= shift_tab_left[index].mask;
			cpu->regs[ii.rt] |= utmp32 << shift_tab_left[index].shift;
		}
		break;
	case opcLWR:
		addr = (urrs + ((int32_t) ii.imm)) & ((uint32_t) ~0x03);
		res = cpu_read_mem(cpu, addr, BITS_32, &utmp32, true);
		
		if (res == excNone) {
			unsigned int index = (urrs + ((int32_t) ii.imm)) & 0x03;
			
			cpu->regs[ii.rt] &= shift_tab_right[index].mask;
			cpu->regs[ii.rt] |= (utmp32 >> shift_tab_right[index].shift)
			    & (~shift_tab_right[index].mask);
		}
		break;
	case opcLWU:
		res = excRI;
		break;
	case opcSB:
		res = cpu_write_mem(cpu, urrs + ((int32_t) ii.imm), BITS_8,
		    cpu->regs[ii.rt], true);
		break;
	case opcSC:
		if (!cpu->llbit) {
			/* If we are not tracking LL-SC,
			   then SC has to fail */
			cpu->regs[ii.rt] = 0;
		} else {
			/* We do track LL-SC address */
			
			/* Compute trarget address */
			addr = urrs + ((int32_t) ii.imm);
			
			/* Perform the write operation */
			res = cpu_write_mem(cpu, addr, BITS_32, cpu->regs[ii.rt], true);
			if (res == excNone) {
				/* The operation has been successful,
				   write the result, but... */
				cpu->regs[ii.rt] = 1;
				
				/* ...we are too polite if LL and SC addresses differ.
				   In such a case, the behaviour of SC is undefined.
				   Let's check that. */
				convert_addr(cpu, &addr, false, false);
				
				/* sc_addr now contains physical target address */
				if (addr != cpu->lladdr) {
					/* LL and SC addresses do not match ;( */
					if (errors)
						mprintf("\nError: LL-SC addresses do not match\n\n");
				}
			} else {
				/* Error writing the target */
			}
			
			/* SC always stops LL-SC address tracking */
			unregister_sc(cpu);
			cpu->llbit = false;
		}
		break;
	case opcSCD:
	case opcSD:
	case opcSDL:
	case opcSDR:
		/* 64-bit instructions */
		res = excRI;
		break;
	case opcSH:
		res = cpu_write_mem(cpu, urrs + ((int16_t) ii.imm), BITS_16,
		    cpu->regs[ii.rt], true);
		break;
	case opcSW:
		res = cpu_write_mem(cpu, urrs + ((int16_t) ii.imm), BITS_32,
		    cpu->regs[ii.rt], true);
		break;
	case opcSWL:
		addr = (urrs + ((int32_t) ii.imm)) & ((uint32_t) ~0x03);
		res = cpu_read_mem(cpu, addr, BITS_32, &utmp32, true);
		
		if (res == excNone) {
			unsigned int index = (urrs + ((int32_t) ii.imm)) & 0x03;
			
			utmp32 &= shift_tab_left_store[index].mask;
			utmp32 |= (cpu->regs[ii.rt] >> shift_tab_left_store[index].shift)
			    & (~shift_tab_left_store[index].mask);
			
			res = cpu_write_mem(cpu, addr, BITS_32, utmp32, true);
		}
		break;
	case opcSWR:
		addr = (urrs + ((int32_t) ii.imm)) & ((uint32_t) ~0x03);
		res = cpu_read_mem(cpu, addr, BITS_32, &utmp32, true);
		
		if (res == excNone) {
			unsigned int index = (urrs + ((int32_t) ii.imm)) & 0x03;
			
			utmp32 &= shift_tab_right_store[index].mask;
			utmp32 |= cpu->regs[ii.rt] << shift_tab_right_store[index].shift;
			
			res = cpu_write_mem(cpu, addr, BITS_32, utmp32, true);
		}
		break;
	
	/*
	 * Traps
	 */
	case opcTEQ:
		TRAP(urrs == urrt);
		break;
	case opcTEQI:
		TRAP(urrs == ii.imm);
		break;
	case opcTGE:
		TRAP(((int32_t) urrs) >= ((int32_t) urrt));
		break;
	case opcTGEI:
		TRAP(((int32_t) urrs) >= ((int32_t) ii.imm));
		break;
	case opcTGEIU:
		TRAP(urrs >= ii.imm);
		break;
	case opcTGEU:
		TRAP(urrs >= urrt);
		break;
	case opcTLT:
		TRAP(((int32_t) urrs) < ((int32_t) urrt));
		break;
	case opcTLTI:
		TRAP(((int32_t) urrs) < ((int32_t) ii.imm));
		break;
	case opcTLTIU:
		TRAP(urrs < ii.imm);
		break;
	case opcTLTU:
		TRAP(urrs < urrt);
		break;
	case opcTNE:
		TRAP(urrs != urrt);
		break;
	case opcTNEI:
		TRAP(urrs != ii.imm);
		break;
	
	/*
	 * Special instructions
	 */
	case opcCFC0:
		/* This instruction is not valid */
		break;
	case opcCFC1:
		if (cp0_status_cu1(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu1;
		}
		break;
	case opcCFC2:
		if (cp0_status_cu2(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu2;
		}
		break;
	case opcCFC3:
		if (cp0_status_cu3(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu3;
		}
		break;
	case opcCTC0:
		/* This instruction is not valid */
		break;
	case opcCTC1:
		if (cp0_status_cu1(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu1;
		}
		break;
	case opcCTC2:
		if (cp0_status_cu2(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu2;
		}
		break;
	case opcCTC3:
		if (cp0_status_cu3(cpu)) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu3;
		}
		break;
	case opcERET:
		if ((cp0_status_cu0(cpu))
		    || (!cp0_status_ksu(cpu))
		    || (cp0_status_exl(cpu))
		    || (cp0_status_erl(cpu))) {
			/* ERET breaks LL-SC address tracking */
			cpu->llbit = false;
			unregister_sc(cpu);
			
			/* Delay slot test */
			if ((cpu->branch != BRANCH_NONE) && (errors))
				mprintf("\nError: ERET in a delay slot\n\n");
			
			if (cp0_status_erl(cpu)) {
				/* Error level */
				cpu->pc_next = cp0_errorepc(cpu);
				pca = cpu->pc_next + 4;
				cp0_status(cpu) &= ~cp0_status_erl_mask;
			} else {
				/* Exception level */
				cpu->pc_next = cp0_epc(cpu);
				pca = cpu->pc_next + 4;
				cp0_status(cpu) &= ~cp0_status_exl_mask;
			}
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcMFC0:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))) {
			cpu->regs[ii.rt] = cpu->cp0[ii.rd];
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcMFHI:
		cpu->regs[ii.rd] = cpu->hireg;
		break;
	case opcMFLO:
		cpu->regs[ii.rd] = cpu->loreg;
		break;
	case opcMTC0:
		if ((cp0_status_cu0(cpu) == 1)
		    || ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu))
		    || (cp0_status_erl(cpu)))) {
			switch (ii.rd) {
			/* 0 */
			case cp0_Index:
				cp0_index(cpu) = urrt & 0x3fU;
				break;
			case cp0_Random:
				/* Ignored, read-only */
				break;
			case cp0_EntryLo0:
				cp0_entrylo0(cpu) = urrt & 0x3fffffffU;
				break;
			case cp0_EntryLo1:
				cp0_entrylo1(cpu) = urrt & 0x3fffffffU;
				break;
			case cp0_Context:
				cp0_context(cpu) = urrt & 0xfffffff0U;
				break;
			case cp0_PageMask:
				cp0_pagemask(cpu) = 0;
				if ((urrt == 0x0U)
				    || (urrt == 0x6000U)
				    || (urrt == 0x1e000U)
				    || (urrt == 0x7e000U)
				    || (urrt == 0x1fe000U)
				    || (urrt == 0x7fe000U)
				    || (urrt == 0x1ffe000U))
					cp0_pagemask(cpu) = urrt & cp0_pagemask_mask_mask;
				else if (errors)
					mprintf("\nMTC0: Invalid value for PageMask\n");
				break;
			case cp0_Wired:
				cp0_random(cpu) = 47;
				cp0_wired(cpu) = urrt & 0x3fU;
				if (cp0_wired(cpu) > 47)
					mprintf("\nMTC0: Invalid value for Wired\n");
				break;
			case cp0_Res1:
				/* Ignored, reserved */
				break;
			/* 8 */
			case cp0_BadVAddr:
				/* Ignored, read-only */
				break;
			case cp0_Count:
				cp0_count(cpu) = urrt;
				break;
			case cp0_EntryHi:
				cp0_entryhi(cpu) = urrt & 0xfffff0ffU;
				break;
			case cp0_Compare:
				cp0_compare(cpu) = urrt;
				cp0_cause(cpu) &= ~(1 << cp0_cause_ip7_shift);
				break;
			case cp0_Status:
				cp0_status(cpu) = urrt & 0xff77ff1fU;
				break;
			case cp0_Cause:
				cp0_cause(cpu) &= ~(cp0_cause_ip0_mask | cp0_cause_ip1_mask);
				cp0_cause(cpu) |= urrt & (cp0_cause_ip0_mask | cp0_cause_ip1_mask);
				break;
			case cp0_EPC:
				cp0_epc(cpu) = urrt;
				break;
			case cp0_PRId:
				/* Ignored, read-only */
				break;
			/* 16 */
			case cp0_Config:
				/* Ignored for simulation */
				cp0_config(cpu) = urrt & 0xffffefffU;
				break;
			case cp0_LLAddr:
				cp0_lladdr(cpu) = urrt;
				break;
			case cp0_WatchLo:
				cp0_watchlo(cpu) = urrt & (~cp0_watchlo_res_mask);
				cpu->waddr = cp0_watchhi_paddr1(cpu);
				cpu->waddr <<= (32 - cp0_watchlo_paddr0_shift);
				cpu->waddr |= cp0_watchlo_paddr0(cpu);
				break;
			case cp0_WatchHi:
				cp0_watchhi(cpu) = urrt & (~cp0_watchhi_res_mask);
				cpu->waddr = cp0_watchhi_paddr1(cpu);
				cpu->waddr <<= (32 - cp0_watchlo_paddr0_shift);
				cpu->waddr |= cp0_watchlo_paddr0(cpu);
				break;
			case cp0_XContext:
				/* Ignored in 32bit MIPS */
				break;
			case cp0_Res2:
				/* Ignored, reserved */
				break;
			case cp0_Res3:
				/* Ignored, reserved */
				break;
			case cp0_Res4:
				/* Ignored, reserved */
				break;
			/* 24 */
			case cp0_Res5:
				/* Ignored, reserved */
				break;
			case cp0_Res6:
				/* Ignored, reserved */
				break;
			case cp0_ECC:
				/* Ignored for simulation */
				cp0_ecc(cpu) = (urrt & cp0_ecc_ecc_mask) << cp0_ecc_ecc_shift;
				break;
			case cp0_CacheErr:
				/* Ignored, read-only */
				break;
			case cp0_TagLo:
				cp0_taglo(cpu) = urrt;
				break;
			case cp0_TagHi:
				cp0_taghi(cpu) = urrt;
				break;
			case cp0_ErrorEPC:
				cp0_errorepc(cpu) = urrt;
				break;
			case cp0_Res7:
				/* Ignored */
				break;
			}
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcMTC1:
		if (cp0_status_cu1(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu1;
		}
		break;
	case opcMTC2:
		if (cp0_status_cu2(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu2;
		}
		break;
	case opcMTC3:
		if (cp0_status_cu3(cpu) == 1) {
			/* Ignored */
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
			cp0_cause(cpu) |= cp0_cause_ce_cu3;
		}
		break;
	case opcSDC1:
	case opcSDC2:
		/* 64-bit instructions */
		res = excRI;
		break;
	case opcMTHI:
		cpu->hireg = urrs;
		break;
	case opcMTLO:
		cpu->loreg = urrs;
		break;
	case opcSYNC:
		/* No synchronisation is needed */
		break;
	case opcSYSCALL:
		res = excSys;
		break;
	case opcRES:
		res = excRI;
		break;
	case opcQRES:
		/* Quiet reserved */
		break;
	case opcTLBP:
		if ((cp0_status_cu0(cpu) == 1)
		    || (cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1)) {
			cp0_index(cpu) = 1 << cp0_index_p_shift;
			uint32_t xvpn2 = cp0_entryhi(cpu) & cp0_entryhi_vpn2_mask;
			uint32_t xasid = cp0_entryhi(cpu) & cp0_entryhi_asid_mask;
			unsigned int i;
			
			for (i = 0; i < TLB_ENTRIES; i++) {
				/*
				 * Mask the VPN2 value from EntryHi with the PageMask
				 * value from the TLB before comparing with the VPN2
				 * value from the TLB. This does not respect the official
				 * R4000 documentation, but it is compliant with the
				 * behaviour of other MIPS CPUs and it is actually
				 * necessary for proper support for multiple page
				 * sizes.
				 */
				if ((cpu->tlb[i].vpn2 == (xvpn2 & cpu->tlb[i].mask)) &&
				    ((cpu->tlb[i].global) || (cpu->tlb[i].asid == xasid))) {
					cp0_index(cpu) = i;
					break;
				}
			}
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcTLBR:
		if ((cp0_status_cu0(cpu) == 1)
		    || (cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1)) {
			uint32_t i = cp0_index_index(cpu);
			
			if (i > 47) {
				mprintf("\nTLBR: Invalid value in Index\n");
				cp0_pagemask(cpu) = 0;
				cp0_entryhi(cpu) = 0;
				cp0_entrylo0(cpu) = 0;
				cp0_entrylo1(cpu) = 0;
			} else {
				cp0_pagemask(cpu) = (~cpu->tlb[i].mask) & 0x01ffe000U;
				cp0_entryhi(cpu) = cpu->tlb[i].vpn2 | cpu->tlb[i].asid;
				
				cp0_entrylo0(cpu) = (cpu->tlb[i].pg[0].pfn >> 6)
				    | (cpu->tlb[i].pg[0].cohh << 3)
				    | ((cpu->tlb[i].pg[0].dirty ? 1 : 0) << 2)
				    | ((cpu->tlb[i].pg[0].valid ? 1 : 0) << 1)
				    | (cpu->tlb[i].global ? 1 : 0);
				
				cp0_entrylo1(cpu) = (cpu->tlb[i].pg[1].pfn >> 6)
				    | (cpu->tlb[i].pg[1].cohh << 3)
				    | ((cpu->tlb[i].pg[1].dirty ? 1 : 0) << 2)
				    | ((cpu->tlb[i].pg[1].valid ? 1 : 0) << 1)
				    | (cpu->tlb[i].global ? 1 : 0);
			}
		} else {
			/* Coprocessor unusable */
			res = excCpU;
			cp0_cause(cpu) &= ~cp0_cause_ce_mask;
		}
		break;
	case opcTLBWI:
		TLBW(cpu, false, &res);
		break;
	case opcTLBWR:
		TLBW(cpu, true, &res);
		break;
	case opcBREAK:
		res = excBp;
		break;
	case opcWAIT:
		cpu->pc_next = cpu->pc;
		cpu->stdby = true;
		break;
	case opcNOP:
		break;
	
	/*
	 * Machine debugging instructions
	 */
	case opcDVAL:
		mprintf("\nDebug: value %#" PRIx32 " (%" PRIu32 ")\n\n",
		    cpu->regs[4], cpu->regs[4]);
		break;
	case opcDTRC:
		if (!totrace) {
			reg_view(cpu);
			mprintf("\n");
		}
		cpu_update_debug(cpu);
		totrace = true;
		break;
	case opcDTRO:
		totrace = false;
		break;
	case opcDRV:
		mprintf("\nDebug: register view\n");
		reg_view(cpu);
		mprintf("\n");
		break;
	case opcDHLT:
		if (totrace)
			mprintf("\nMachine halt\n\n");
		tohalt = true;
		break;
	case opcDINT:
		interactive = true;
		break;
	
	/*
	 * Unimplemented instructions
	 */
	case opcSPECIAL:
	case opcBCOND:
	case opcSPECIAL2:
	case opcCACHE:
	case opcCOP0:
	case opcCOP1:
	case opcCOP2:
	case opcCOP3:
	case opcDMFC0:
	case opcDMFC1:
	case opcDMFC2:
	case opcDMFC3:
	case opcDMTC0:
	case opcDMTC1:
	case opcDMTC2:
	case opcDMTC3:
	case opcLDC1:
	case opcLDC2:
	case opcLWC1:
	case opcLWC2:
	case opcMFC1:
	case opcMFC2:
	case opcMFC3:
	case opcSWC1:
	case opcSWC2:
	case opcUNIMP:
	case opcIllegal:
	case opcBC:
	case opcC0:
		res = excRI;
		break;
	}
	
	/* Branch test */
	if ((cpu->branch == BRANCH_COND) || (cpu->branch == BRANCH_NONE))
		cpu->excaddr = cpu->pc;
	
	/* PC update */
	if (res == excNone) {
		cpu->pc = cpu->pc_next;
		cpu->pc_next = pca;
	}
	
	/* Register 0 is hardwired zero */
	cpu->regs[0] = 0;
	
	return res;
}

/** Change the processor state according to the exception type
 *
 */
static void handle_exception(cpu_t *cpu, exc_t res)
{
	bool tlb_refill = false;
	
	/* Convert TLB Refill exceptions */
	if ((res == excTLBLR) || (res == excTLBSR)) {
		tlb_refill = true;
		if (res == excTLBLR)
			res = excTLBL;
		else
			res = excTLBS;
	}
	
	/* The standby mode is cancelled by the exception */
	if (cpu->stdby)
		cpu_set_pc(cpu, cpu->pc + 4);
	
	cpu->stdby = false;
	
	/* User info and register fill */
	if (totrace)
		mprintf("\nRaised exception: %s\n\n", exc_text[res]);
	
	cp0_cause(cpu) &= ~cp0_cause_exccode_mask;
	cp0_cause(cpu) |= res << cp0_cause_exccode_shift;
	
	/* Exception branch control */
	cp0_cause(cpu) &= ~cp0_cause_bd_mask;
	if (cpu->branch == BRANCH_PASSED)
		cp0_cause(cpu) |= cp0_cause_bd_mask;
	
	if (!cp0_status_exl(cpu)) {
		cp0_epc(cpu) = cpu->excaddr;
		if ((res == excInt) && (cpu->branch != BRANCH_COND))
			cp0_epc(cpu) = cpu->pc;
	}
	
	ptr_t exc_pc;
	/* Exception vector base address */
	if (cp0_status_bev(cpu)) {
		/* Boot time */
		if (res != excReset)
			exc_pc = 0xbfc00200U;
		else
			exc_pc = 0xbfc00000U;
	} else {
		/* Normal time */
		if (res != excReset)
			exc_pc = 0x80000000U;
		else
			exc_pc = 0xbfc00000U;
	}
	
	/* Exception vector offsets */
	if ((cp0_status_exl(cpu)) || (!tlb_refill))
		exc_pc += EXCEPTION_OFFSET;
	
	cpu_set_pc(cpu, exc_pc);
	
	/* Switch to kernel mode */
	cp0_status(cpu) |= cp0_status_exl_mask;
}

/** React on interrupt requests, updates internal timer, random register
 *
 */
static void manage(cpu_t *cpu, exc_t res)
{
	/* Test for interrupt request */
	if ((res == excNone)
	    && (!cp0_status_exl(cpu))
	    && (!cp0_status_erl(cpu))
	    && (cp0_status_ie(cpu))
	    && ((cp0_cause(cpu) & cp0_status(cpu)) & cp0_cause_ip_mask) != 0)
		res = excInt;
	
	/* Exception control */
	if (res != excNone)
		handle_exception(cpu, res);
	
	/* Increase counter */
	cp0_count(cpu)++;
	
	/* Decrease random register */
	if (cp0_random(cpu)-- == 0)
		cp0_random(cpu) = 47;
	
	if (cp0_random(cpu) < cp0_wired(cpu))
		cp0_random(cpu) = 47;
	
	/* Timer control */
	if (cp0_count(cpu) == cp0_compare(cpu))
		/* Generate interrupt request */
		cp0_cause(cpu) |= 1 << cp0_cause_ip7_shift;
}

/* Simulate one instruction
 *
 * Four main stages are performed:
 *  - instruction fetch
 *  - instruction decode
 *  - instruction execution
 *  - debugging output
 *
 */
static void instruction(cpu_t *cpu, exc_t *res)
{
	instr_info_t ii;
	
	/* Fetch instruction */
	*res = cpu_read_ins(cpu, cpu->pc, &ii.icode, true);
	if (*res == excNone) {
		/* Decode instruction */
		decode_instr(&ii);
		
		/* Execute instruction */
		uint32_t old_pc = cpu->pc;
		*res = execute(cpu, ii);
		
		/* Debugging output */
		if (totrace) {
			char *modified_regs;
			
			if (iregch)
				modified_regs = modified_regs_dump(cpu);
			else
				modified_regs = NULL;
			
			iview(cpu, old_pc, &ii, modified_regs);
			
			if (modified_regs != NULL)
				safe_free(modified_regs);
		}
	}
}

/* Simulate one step of the processor
 *
 * This is just one instruction.
 *
 */
void cpu_step(cpu_t *cpu)
{
	exc_t res = excNone;
	
	/* Instruction execute */
	if (!cpu->stdby)
		instruction(cpu, &res);
	
	/* Processor control */
	manage(cpu, res);
	
	/* Cycle accounting */
	if (cpu->stdby)
		cpu->w_cycles++;
	else {
		if ((cp0_status_ksu(cpu) == 0)
		    || (cp0_status_exl(cpu) == 1)
		    || (cp0_status_erl(cpu) == 1))
			cpu->k_cycles++;
		else
			cpu->u_cycles++;
	}
	
	/* Branch delay slot control */
	if (cpu->branch > BRANCH_NONE)
		cpu->branch--;
}
