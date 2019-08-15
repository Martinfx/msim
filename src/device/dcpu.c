/*
 * Copyright (c) 2003-2004 Viliam Holub
 * All rights reserved.
 *
 * Distributed under the terms of GPL.
 *
 *
 *  R4000 microprocessor (32bit) device
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "../main.h"
#include "device.h"
#include "../cpu/cpu.h"
#include "../debug/debug.h"
#include "../debug/breakpoint.h"
#include "../io/output.h"
#include "../utils.h"

#include "dcpu.h"


/*
 * Device structure initialization
 */

static bool dcpu_init(parm_link_s *parm, device_s *dev);
static bool dcpu_info(parm_link_s *parm, device_s *dev);
static bool dcpu_stat(parm_link_s *parm, device_s *dev);
static bool dcpu_cp0d(parm_link_s *parm, device_s *dev);
static bool dcpu_tlbd(parm_link_s *parm, device_s *dev);
static bool dcpu_md(parm_link_s *parm, device_s *dev);
static bool dcpu_id(parm_link_s *parm, device_s *dev);
static bool dcpu_rd(parm_link_s *parm, device_s *dev);
static bool dcpu_goto(parm_link_s *parm, device_s *dev);
static bool dcpu_break(parm_link_s *parm, device_s *dev);
static bool dcpu_bd(parm_link_s *parm, device_s *dev);
static bool dcpu_br(parm_link_s *parm, device_s *dev);

cmd_s dcpu_cmds[] = {
	{
		"init",
		(cmd_f) dcpu_init,
		DEFAULT,
		DEFAULT,
		"Initialization",
		"Initialization",
		REQ STR "pname/processor name" END
	},
	{
		"help",
		(cmd_f) dev_generic_help,
		DEFAULT,
		DEFAULT,
		"Display this help text",
		"Display this help text",
		OPT STR "cmd/command name" END
	},
	{
		"info",
		(cmd_f) dcpu_info,
		DEFAULT,
		DEFAULT,
		"Display configuration information",
		"Display configuration information",
		NOCMD
	},
	{
		"stat",
		(cmd_f) dcpu_stat,
		DEFAULT,
		DEFAULT,
		"Display processor statistics",
		"Display processor statistics",
		NOCMD
	},
	{
		"cp0d",
		(cmd_f) dcpu_cp0d,
		DEFAULT,
		DEFAULT,
		"Dump contents of the coprocessor 0 register(s)",
		"Dump contents of the coprocessor 0 register(s)",
		OPT INT "rn/register number" END
	},
	{
		"tlbd",
		(cmd_f)dcpu_tlbd,
		DEFAULT,
		DEFAULT,
		"Dump content of TLB",
		"Dump content of TLB",
		NOCMD
	},
	{
		"md",
		(cmd_f) dcpu_md,
		DEFAULT,
		DEFAULT,
		"Dump specified TLB mapped memory block",
		"Dump specified TLB mapped memory block",
		REQ INT "saddr/starting address" NEXT
		REQ INT "size/size" END
	},
	{
		"id",
		(cmd_f) dcpu_id,
		DEFAULT,
		DEFAULT,
		"Dump instructions from specified TLB mapped memory",
		"Dump instructions from specified TLB mapped memory",
		REQ INT "saddr/starting address" NEXT
		REQ INT "cnt/count" END
	},
	{
		"rd",
		(cmd_f) dcpu_rd,
		DEFAULT,
		DEFAULT,
		"Dump contents of CPU general registers",
		"Dump contents of CPU general registers",
		NOCMD
	},
	{
		"goto",
		(cmd_f) dcpu_goto,
		DEFAULT,
		DEFAULT,
		"Go to address",
		"Go to address",
		REQ INT "addr/address" END
	},
	{
		"break",
		(cmd_f) dcpu_break,
		DEFAULT,
		DEFAULT,
		"Add code breakpoint",
		"Add code breakpoint",
		REQ INT "addr/address" END
	},
	{
		"bd",
		(cmd_f) dcpu_bd,
		DEFAULT,
		DEFAULT,
		"Dump code breakpoints",
		"Dump code breakpoints",
		NOCMD
	},
	{
		"br",
		(cmd_f) dcpu_br,
		DEFAULT,
		DEFAULT,
		"Remove code breakpoint",
		"Remove code breakpoint",
		REQ INT "addr/address" END
	},
	LAST_CMD
};

const char id_dcpu[] = "dcpu";

static void dcpu_done(device_s *dev);
static void dcpu_step(device_s *dev);

device_type_s dcpu = {
	/* Type name */
	.name = id_dcpu,
	
	/* Brief description*/
	.brief = "MIPS R4000 processor",
	
	/* Full description */
	.full = "MIPS R4000 processor restricted to 32 bits without FPU",
	
	/* Functions */
	.done  = dcpu_done,	/* done */
	.step  = dcpu_step,	/* step */
	.step4 = NULL, 		/* step4 */
	.read  = NULL,		/* read */
	.write = NULL,		/* write */
	
	/* Commands */
	dcpu_cmds
};


/** Get first available CPU id
 *
 * @return First available CPU id or MAX_CPU if no
 *         more CPU slots available.
 *
 */
static unsigned int dcpu_get_free_id(void)
{
	unsigned int c;
	unsigned int id_mask = 0;
	device_s *dev = NULL;

	while (dev_next(&dev, DEVICE_FILTER_PROCESSOR))
		id_mask |= 1 << ((cpu_t *) dev->data)->procno;
	
	for (c = 0; c < MAX_CPU; c++, id_mask >>= 1)
		if (!(id_mask & 1))
			return c;

	return MAX_CPU;
}

	


/** Initialization
 *
 */
static bool dcpu_init(parm_link_s *parm, device_s *dev)
{
	unsigned int id = dcpu_get_free_id();
	
	if (id == MAX_CPU) {
		mprintf("Maximum CPU count exceeded (%u)", MAX_CPU);
		return false;
	}
	
	cpu_t *cpu = safe_malloc_t(cpu_t);
	cpu_init(cpu, id);
	
	dev->data = cpu;
	
	return true;
}


/** Info command implementation
 *
 */
static bool dcpu_info(parm_link_s *parm, device_s *dev)
{
	mprintf("type:R4000.32\n");
	return true;
}


/** Stat command implementation
 *
 */
static bool dcpu_stat(parm_link_s *parm, device_s *dev)
{
	cpu_t *cpu = (cpu_t *) dev->data;
	
	mprintf("Total cycles         In kernel space      In user space\n");
	mprintf("-------------------- -------------------- --------------------\n");
	mprintf("%20" PRIu64 " %20" PRIu64 " %20" PRIu64 "\n\n",
	    (uint64_t) cpu->k_cycles + cpu->u_cycles + cpu->w_cycles,
	    cpu->k_cycles, cpu->u_cycles);
	
	mprintf("Wait cycles          TLB Refill exc       TLB Invalid exc\n");
	mprintf("-------------------- -------------------- --------------------\n");
	mprintf("%20" PRIu64 " %20" PRIu64 " %20" PRIu64 "\n\n",
	    cpu->w_cycles, cpu->tlb_refill, cpu->tlb_invalid);
	
	mprintf("TLB Modified exc     Interrupt 0          Interrupt 1\n");
	mprintf("-------------------- -------------------- --------------------\n");
	mprintf("%20" PRIu64 " %20" PRIu64 " %20" PRIu64 "\n\n",
	    cpu->tlb_modified, cpu->intr[0], cpu->intr[1]);
	
	mprintf("Interrupt 2          Interrupt 3          Interrupt 4\n");
	mprintf("-------------------- -------------------- --------------------\n");
	mprintf("%20" PRIu64 " %20" PRIu64 " %20" PRIu64 "\n\n",
	    cpu->intr[2], cpu->intr[3], cpu->intr[4]);
	
	mprintf("Interrupt 5          Interrupt 6          Interrupt 7\n");
	mprintf("-------------------- -------------------- --------------------\n");
	mprintf("%20" PRIu64 " %20" PRIu64 " %20" PRIu64 "\n",
	    cpu->intr[5], cpu->intr[6], cpu->intr[7]);
	
	return true;
}


/** Cp0d command implementation
 *
 */
static bool dcpu_cp0d(parm_link_s *parm, device_s *dev)
{
	int no = -1;
	
	if (parm->token.ttype == tt_int) {
		no = parm->token.tval.i;
		if ((no < 0) || (no > 31)) {
			mprintf("Out of range (0..31)");
			return false;
		}
	}
	
	cp0_dump((cpu_t *) dev->data, no);
	
	return true;
}


/** Tlbd command implementation
 *
 */
static bool dcpu_tlbd(parm_link_s *parm, device_s *dev)
{
	tlb_dump((cpu_t *) dev->data);
	
	return true;
}


/** Md command implementation
 *
 */
static bool dcpu_md(parm_link_s *parm, device_s *dev)
{
	int j;
	uint32_t val, addr, siz;
	exc_t res;
	
	addr = parm->token.tval.i & ~0x3;
	siz = parm->next->token.tval.i;
	
	for (j = 0; siz; siz--, addr+=4, j++) {
		if (!(j & 0x3))
			mprintf("  %#10" PRIx32 "    ", addr);
		
		res = cpu_read_mem((cpu_t *) dev->data, addr, 4, &val, false);
		mprintf("res: %d\n", res);
		
		if (res == excNone)
			mprintf("%08" PRIx32 " ", val);
		else
			mprintf("xxxxxxxx ");
		
		if ((j & 0x3) == 3)
			mprintf("\n");
	}

	if (j)  
		mprintf("\n");

	return true;
}


/** Id command implementation
 *
 */
static bool dcpu_id(parm_link_s *parm, device_s *dev)
{
	exc_t res;
	instr_info_t ii;
	uint32_t addr, siz;
	
	addr = parm->token.tval.i & ~0x3;
	siz = parm->next->token.tval.i;
	ii.rt = 0;
	ii.rd = 0;
	ii.rs = 0;
	
	for (; siz; siz--, addr += 4) {
		res = cpu_read_ins((cpu_t *) dev->data, addr, &ii.icode, false);
		
		if (res != excNone) {
			ii.icode = 0;
			ii.opcode = opcIllegal;
		} else
			decode_instr(&ii);
		
		iview((cpu_t *) dev->data, addr, &ii, 0);
	}

	return true;
}


/** Rd command implementation
 *
 */
static bool dcpu_rd(parm_link_s *parm, device_s *dev)
{
	reg_view((cpu_t *) dev->data);
	
	return true;
}


/** Goto command implementation
 *
 */
static bool dcpu_goto(parm_link_s *parm, device_s *dev)
{
	cpu_t *cpu = (cpu_t *) dev->data;
	ptr_t addr = parm->token.tval.i;
	
	cpu_set_pc(cpu, addr);
	return true;
}


/** Break command implementation
 *
 */
static bool dcpu_break(parm_link_s *parm, device_s *dev)
{
	breakpoint_t *bp = breakpoint_init(parm->token.tval.i,
	    BREAKPOINT_KIND_SIMULATOR);
	cpu_t *cpu = (cpu_t *) dev->data;
	
	list_append(&cpu->bps, &bp->item);
	return true;
}


/** Bd command implementation
 *
 */
static bool dcpu_bd(parm_link_s *parm, device_s *dev)
{
	cpu_t *cpu = (cpu_t *) dev->data;
	breakpoint_t *bp;
	
	mprintf("Address    Hits                 Kind\n");
	mprintf("---------- -------------------- ----------\n");
	
	for_each(cpu->bps, bp, breakpoint_t) {
		const char *kind = (bp->kind == BREAKPOINT_KIND_SIMULATOR)
		    ? "Simulator" : "Debugger";
		
		mprintf("%#010" PRIx32 " %20" PRIu64 " %s\n",
		    bp->pc, bp->hits, kind);
	}
	
	return true;
}


/** Br command implementation
 *
 */
static bool dcpu_br(parm_link_s *parm, device_s *dev)
{
	cpu_t *cpu = (cpu_t *) dev->data;
	ptr_t addr = parm->token.tval.i;
	
	bool fnd = false;
	breakpoint_t *bp;
	for_each(cpu->bps, bp, breakpoint_t) {
		if (bp->pc == addr) {
			list_remove(&cpu->bps, &bp->item);
			safe_free(bp);
			fnd = true;
			break;
		}
	}
	
	if (!fnd)
		mprintf("Unknown breakpoint\n");
	
	return true;
}


/** Done
 *
 */
static void dcpu_done(device_s *dev)
{
	safe_free(dev->name);
	safe_free(dev->data);
}


/** Execute one processor step
 *
 */
static void dcpu_step(device_s *dev)
{
	cpu_step((cpu_t *) dev->data);
}

cpu_t *dcpu_find_no(unsigned int no)
{
	device_s *dev = NULL;
	
	while (dev_next(&dev, DEVICE_FILTER_PROCESSOR)) {
		cpu_t* cpu = (cpu_t *) dev->data;
		if (cpu->procno == no)
			return cpu;
	}
	
	return NULL;
}

void dcpu_interrupt_up(unsigned int cpuno, unsigned int no)
{
	cpu_t *cpu = dcpu_find_no(cpuno);
	
	if (cpu != NULL)
		cpu_interrupt_up(cpu, no);
}

void dcpu_interrupt_down(unsigned int cpuno, unsigned int no)
{
	cpu_t *cpu = dcpu_find_no(cpuno);
	
	if (cpu != NULL)
		cpu_interrupt_down(cpu, no);
}
