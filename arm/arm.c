/*
 * Copyright (c) 2005 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include <sys/sys.h>
#include <arm/arm.h>
#include <arm/mmu.h>
#include <arm/uops.h>
#include <util/atomic.h>

struct cpu_struct cpu; // the cpu

struct cpu_types {
	const char *name;
	enum arm_instruction_set isa;
	enum arm_core core;
	bool with_cp15;
	bool with_mmu;
};

static const struct cpu_types cpu_types[] = {
	{ "armv4", ARM_V4, ARM7, FALSE, FALSE },
	{ "armv5", ARM_V5, ARM9, TRUE, TRUE },
	{ "armv5e", ARM_V5e, ARM9, TRUE, TRUE },
	{ "armv6", ARM_V6, ARM9, TRUE, TRUE }, // not correct, but no arm11 support yet

	{ "arm7tdmi", ARM_V4, ARM7, FALSE, FALSE },
	{ "arm7", ARM_V4, ARM7, FALSE, FALSE },
	{ "arm9tdmi", ARM_V4, ARM9, TRUE, TRUE },
	{ "arm9", ARM_V4, ARM9, TRUE, TRUE },
	{ "arm9e", ARM_V5e, ARM9e, TRUE, TRUE },
	{ "arm926ejs", ARM_V5e, ARM9e, TRUE, TRUE },
	{ "arm926", ARM_V5e, ARM9e, TRUE, TRUE },

	{ NULL, 0, 0, 0, 0 },
};

int initialize_cpu(const char *cpu_type)
{
	const struct cpu_types *t;

	memset(&cpu, 0, sizeof(cpu));

	// build the condition table
	build_condition_table();

	// set the instruction set. Default to ARM7
	cpu.isa = ARM_V4;
	cpu.core = ARM7;

	if(cpu_type) {
		for(t = cpu_types; t->name; t++) {
			if(!strcasecmp(t->name, cpu_type)) {
				cpu.isa = t->isa;
				cpu.core = t->core;
				
				// initialize cp15
				if(t->with_cp15 )
					install_cp15();

				// initialize mmu
				mmu_init(t->with_mmu);
				break;
			}
		}
	}

	/* initialize the uop cache */
	uop_init();

	return 0;
}

void reset_cpu(void)
{
	atomic_or(&cpu.pending_exceptions, EX_RESET); // schedule a reset	
}

static int cpu_startup_thread_entry(void *args)
{
	SDL_Event event;

	// start the uop engine
	uop_dispatch_loop();

	// run the decoder loop
//	decoder_loop();

	// the cpu bailed for some reason, push a quit event into the SDL event queue
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);

	return 0;
}

static Uint32 speedtimer(Uint32 interval, void *param)
{
	static struct perf_counters old_perf_counters;
	struct perf_counters delta_perf_counter;
	int i;
	
	for(i=0; i<MAX_PERF_COUNTER; i++) {
		delta_perf_counter.count[i] = cpu.perf_counters.count[i] - old_perf_counters.count[i];
		old_perf_counters.count[i] = cpu.perf_counters.count[i];
	}

#if COUNT_CYCLES
	printf("%d cycles/sec, ",
		delta_perf_counter.count[CYCLE_COUNT]);
#endif
	printf("%d ins/sec, %d ins decodes/sec, %d slow mmu translates/sec\n", 
		delta_perf_counter.count[INS_COUNT],
		delta_perf_counter.count[INS_DECODE],
		delta_perf_counter.count[MMU_SLOW_TRANSLATE]);
#if COUNT_ARM_OPS
	printf("\tSC %d NOP %d L %d S %d DP %d MUL %d B %d MISC %d, exceptions %d\n",
		delta_perf_counter.count[OP_SKIPPED_CONDITION],
		delta_perf_counter.count[OP_NOP],
		delta_perf_counter.count[OP_LOAD],
		delta_perf_counter.count[OP_STORE],
		delta_perf_counter.count[OP_DATA_PROC],
		delta_perf_counter.count[OP_MUL],
		delta_perf_counter.count[OP_BRANCH],
		delta_perf_counter.count[OP_MISC],
		delta_perf_counter.count[EXCEPTIONS]);
#endif

#if COUNT_UOPS
	for(i=0; i < MAX_UOP_OPCODE; i++) {
		printf("\tuop opcode %d (%s): %d\n", i, uop_opcode_to_str(i), delta_perf_counter.count[UOP_BASE + i]);
	}
#endif
#if COUNT_ARITH_UOPS
	for(i=0; i < 16; i++) {
		printf("\tuop arith opcode %d (%s): %d\n", i, dp_op_to_str(i), delta_perf_counter.count[UOP_ARITH_OPCODE + i]);
	}
#endif

	return interval;
}

int start_cpu(int cycle_count)
{
	if(cycle_count > 0)
		cpu.stop_at_cycle = cycle_count;
	else
		cpu.stop_at_cycle = -1;

	// spawn a new thread for the cpu
	SDL_CreateThread(&cpu_startup_thread_entry, NULL);

	// add a function that goes off once a second
	SDL_AddTimer(1000, &speedtimer, NULL);

	return 0;
}

void set_cpu_mode(int new_mode)
{
	struct banked_regs *regs_from;
	struct banked_regs *regs_to;
	int old_mode = cpu.cpsr & PSR_MODE_MASK;

	CPU_TRACE(4, "mode change: 0x%x to 0x%x\n", old_mode, new_mode);

	if(old_mode == new_mode)
		return;

	// save the regs from the mode we're coming from
	switch(old_mode) {
		case PSR_MODE_user:
		case PSR_MODE_sys:
			regs_from = &cpu.usr_regs;
			break;
		case PSR_MODE_fiq:
			regs_from = &cpu.fiq_regs;
			break;
		case PSR_MODE_irq:
			regs_from = &cpu.irq_regs;
			break;
		case PSR_MODE_svc:
			regs_from = &cpu.svc_regs;
			break;
		case PSR_MODE_abt:
			regs_from = &cpu.abt_regs;
			break;
		case PSR_MODE_und:
			regs_from = &cpu.und_regs;
			break;
		default:
			regs_from = NULL;
			break;
	}

	// restore the regs from the mode we're going to
	switch(new_mode) {
		case PSR_MODE_user:
		case PSR_MODE_sys:
			regs_to = &cpu.usr_regs;
			break;
		case PSR_MODE_fiq:
			regs_to = &cpu.fiq_regs;
			break;
		case PSR_MODE_irq:
			regs_to = &cpu.irq_regs;
			break;
		case PSR_MODE_svc:
			regs_to = &cpu.svc_regs;
			break;
		case PSR_MODE_abt:
			regs_to = &cpu.abt_regs;
			break;
		case PSR_MODE_und:
			regs_to = &cpu.und_regs;
			break;
		default:
			regs_to = NULL;
			break;
	}

	// save and copy the banked regs
	if(regs_from) {
		regs_from->r13 = cpu.r[13];
		regs_from->r14 = cpu.r[14];
		regs_from->spsr = cpu.spsr;
	}
	if(regs_to) {
		cpu.r[13] = regs_to->r13;
		cpu.r[14] = regs_to->r14;
		cpu.spsr = regs_to->spsr;
	}

	// set the mode bits
	cpu.cpsr &= ~PSR_MODE_MASK;
	cpu.cpsr |= new_mode;
}

void raise_irq(void)
{
	CPU_TRACE(5, "raise_irq\n");
	atomic_or(&cpu.pending_exceptions, EX_IRQ);
}

void lower_irq(void)
{
	CPU_TRACE(5, "lower_irq\n");
	atomic_and(&cpu.pending_exceptions, ~EX_IRQ);
}

void raise_fiq(void)
{
	CPU_TRACE(5, "raise_fiq\n");
	atomic_or(&cpu.pending_exceptions, EX_FIQ);
}

void lower_fiq(void)
{
	CPU_TRACE(5, "lower_fiq\n");
	atomic_and(&cpu.pending_exceptions, ~EX_FIQ);
}

void signal_data_abort(armaddr_t addr)
{
	CPU_TRACE(4, "data abort at 0x%08x\n", addr);
	atomic_or(&cpu.pending_exceptions, EX_DATA_ABT);
}

void signal_prefetch_abort(armaddr_t addr)
{
	CPU_TRACE(4, "prefetch abort at 0x%08x\n", addr);
	atomic_or(&cpu.pending_exceptions, EX_PREFETCH);
}

void install_coprocessor(int cp_num, struct arm_coprocessor *coproc)
{
	if(cp_num < 0 || cp_num > 15)
		panic_cpu("install_coprocessor: bad cp num %d\n", cp_num);

	memcpy(&cpu.coproc[cp_num], coproc, sizeof(struct arm_coprocessor));
}

/* build the condition table for checking conditional instructions */
int build_condition_table(void)
{
	int i, j;
	reg_t cpsr;

	for(i=0; i<16; i++) {
		cpu.condition_table[i] = 0;
		for(j=0; j<16; j++) {
			int bit;

			// check the condition field and skip the instruction if necessary
			bit = 0;
			cpsr = i << COND_SHIFT;
			switch(j) {
				case COND_EQ: // Zero set
					if(cpsr & PSR_CC_ZERO)
						bit = 1;
					break;
				case COND_NE: // Zero clear
					if((cpsr & PSR_CC_ZERO) == 0)
						bit = 1;
					break;
				case COND_CS: // Carry set
					if(cpsr & PSR_CC_CARRY)
						bit = 1;
					break;
				case COND_CC: // Carry clear
					if((cpsr & PSR_CC_CARRY) == 0)
						bit = 1;
					break;
				case COND_MI: // Negative set
					if(cpsr & PSR_CC_NEG)
						bit = 1;
					break;
				case COND_PL : // Negative clear
					if((cpsr & PSR_CC_NEG) == 0)
						bit = 1;
					break;
				case COND_VS: // Overflow set
					if(cpsr & PSR_CC_OVL)
						bit = 1;
					break;
				case COND_VC: // Overflow clear
					if((cpsr & PSR_CC_OVL) == 0)
						bit = 1;
					break;
				case COND_HI: // Carry set and Zero clear
					if((cpsr & (PSR_CC_CARRY|PSR_CC_ZERO)) == PSR_CC_CARRY)
						bit = 1;
					break;
				case COND_LS: // Carry clear or Zero set
					if(((cpsr & PSR_CC_CARRY) == 0) || (cpsr & PSR_CC_ZERO))
						bit = 1;
					break;
				case COND_GE: { // Negative set and Overflow set, or Negative clear and Overflow clear (N==V)
					reg_t val = cpsr & (PSR_CC_NEG|PSR_CC_OVL);
					if(val == 0 || val == (PSR_CC_NEG|PSR_CC_OVL))
						bit = 1;
					break;
				}
				case COND_LT: { // Negative set and Overflow clear, or Negative clear and Overflow set (N!=V)
					reg_t val = cpsr & (PSR_CC_NEG|PSR_CC_OVL);
					if(val == PSR_CC_NEG || val == PSR_CC_OVL)
						bit = 1;
					break;
				}
				case COND_GT: { // Zero clear, and either Negative set or Overflow set, or Negative clear and Overflow clear (Z==0 and N==V)
					if((cpsr & PSR_CC_ZERO) == 0) {
						reg_t val = cpsr & (PSR_CC_NEG|PSR_CC_OVL);
						if(val == 0 || val == (PSR_CC_NEG|PSR_CC_OVL))
							bit = 1;				
					}
					break;
				}
				case COND_LE: { // Zero set, or Negative set and Overflow clear, or Negative clear and Overflow set (Z==1 or N!=V)
					if(cpsr & PSR_CC_ZERO)
						bit = 1;

					reg_t val = cpsr & (PSR_CC_NEG|PSR_CC_OVL);
					if(val == PSR_CC_NEG || val == PSR_CC_OVL)
						bit = 1;
					break;
				}
				case COND_SPECIAL:
				case COND_AL:
					bit = 1;
					break;
			}

//			printf("i %d j %d bit %d\n", i, j, bit);
			
			if(bit)
				cpu.condition_table[i] |= (1 << j);
		}
		printf("condition_table[%d] = 0x%x\n", i, cpu.condition_table[i]);
	}

	return 0;
}

// return value of true means something was processed, and thus a possible mode change
bool process_pending_exceptions(void)
{
	// a irq or fiq may happen asychronously in another thread

	CPU_TRACE(5, "process_pending_exceptions: pending ex 0x%x\n", cpu.pending_exceptions);

	// system reset
	if(cpu.pending_exceptions & EX_RESET) {
		// go to a default state
		cpu.cpsr = PSR_IRQ_MASK | PSR_FIQ_MASK;
		put_reg(PC, 0x0);
		cpu.curr_cp = NULL;

		set_cpu_mode(PSR_MODE_svc);

		// mask all other pending exceptions except for irq or fiq
		atomic_and(&cpu.pending_exceptions, (EX_FIQ|EX_IRQ));

		CPU_TRACE(3, "EX: cpu reset!\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// undefined instruction
	if(cpu.pending_exceptions & EX_UNDEFINED) {
		cpu.und_regs.r14 = cpu.pc + (get_condition(PSR_THUMB) ? 1 : 0); // next instruction after the undefined instruction
		cpu.und_regs.spsr = cpu.cpsr;
		put_reg(PC, 0x4);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_und);

		atomic_and(&cpu.pending_exceptions, ~EX_UNDEFINED);

		CPU_TRACE(3, "EX: undefined instruction at 0x%08x\n", cpu.und_regs.r14 - 4);
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// SWI instruction
	if(cpu.pending_exceptions & EX_SWI) {
		cpu.svc_regs.r14 = cpu.pc + (get_condition(PSR_THUMB) ? 1 : 0); // next instruction after the swi instruction
		cpu.svc_regs.spsr = cpu.cpsr;
		put_reg(PC, 0x8);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_svc);

		atomic_and(&cpu.pending_exceptions, ~EX_SWI);

		CPU_TRACE(5, "EX: swi\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// prefetch abort
	if(cpu.pending_exceptions & EX_PREFETCH) {
		cpu.abt_regs.r14 = cpu.pc + 4 + (get_condition(PSR_THUMB) ? 1 : 0); // next instruction after the aborted instruction
		cpu.abt_regs.spsr = cpu.cpsr;
		put_reg(PC, 0xc);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_abt);

		atomic_and(&cpu.pending_exceptions, ~EX_PREFETCH);

		CPU_TRACE(4, "EX: prefetch abort\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// data abort
	if(cpu.pending_exceptions & EX_DATA_ABT) {
		cpu.abt_regs.r14 = cpu.pc + 4 + (get_condition(PSR_THUMB) ? 1 : 0); // +8 from faulting instruction
		cpu.abt_regs.spsr = cpu.cpsr;
		put_reg(PC, 0x10);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_abt);

		atomic_and(&cpu.pending_exceptions, ~EX_DATA_ABT);

		CPU_TRACE(4, "EX: data abort\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// fiq
	if(cpu.pending_exceptions & EX_FIQ && !(cpu.cpsr & PSR_FIQ_MASK)) {
		cpu.fiq_regs.r14 = cpu.pc + 4 + (get_condition(PSR_THUMB) ? 1 : 0); // address of next instruction + 4
		cpu.fiq_regs.spsr = cpu.cpsr;
		put_reg(PC, 0x1c);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_fiq);

		CPU_TRACE(5, "EX: FIQ\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}

	// irq
	if(cpu.pending_exceptions & EX_IRQ && !(cpu.cpsr & PSR_IRQ_MASK)) {
		cpu.irq_regs.r14 = cpu.pc + 4 + (get_condition(PSR_THUMB) ? 1 : 0); // address of next instruction + 4
		cpu.irq_regs.spsr = cpu.cpsr;
		put_reg(PC, 0x18);

		if(get_condition(PSR_THUMB))
			cpu.curr_cp = NULL; // reset the codepage
		set_condition(PSR_THUMB, FALSE); // move to arm
		set_condition(PSR_IRQ_MASK, TRUE);
		set_cpu_mode(PSR_MODE_irq);

		CPU_TRACE(5, "EX: IRQ\n");
		inc_perf_counter(EXCEPTIONS);

		return TRUE;
	}
	
	return FALSE;	
}

void dump_cpu(void)
{
	printf("cpu_dump: ins %d\n", get_instruction_count());
	printf("r0:   0x%08x r1:   0x%08x r2:   0x%08x r3:   0x%08x\n", cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
	printf("r4:   0x%08x r5:   0x%08x r6:   0x%08x r7:   0x%08x\n", cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7]);
	printf("r8:   0x%08x r9:   0x%08x r10:  0x%08x r11:  0x%08x\n", cpu.r[8], cpu.r[9], cpu.r[10], cpu.r[11]);
	printf("r12:  0x%08x sp:   0x%08x lr:   0x%08x r15:  0x%08x pc:   0x%08x\n", cpu.r[12], cpu.r[13], cpu.r[14], cpu.r[15], cpu.pc);
	printf("cpsr: 0x%08x (%c %c%c%c%c) spsr: 0x%08x\n",
		cpu.cpsr,
		get_condition(PSR_THUMB) ? 'T':' ',
		get_condition(PSR_CC_NEG) ? 'N':' ',
		get_condition(PSR_CC_ZERO) ? 'Z':' ',
		get_condition(PSR_CC_CARRY) ? 'C':' ',
		get_condition(PSR_CC_OVL) ? 'O':' ',
		cpu.spsr);
}

void panic_cpu(const char *fmt, ...)
{
	va_list ap;	

	printf("panic: ");

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	dump_cpu();

	dump_sys();

	// push a quit message to the SDL event loop
	SDL_Event event;
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);

	sleep(10);

	exit(1);
}

