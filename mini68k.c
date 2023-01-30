/*
 *	John Coffman's
 *	RBC Mini68K + MF/PIC board with PPIDE
 *
 *	68008 CPU @8MHz 0 or 1 ws I/O 1 or 2 ws
 *	NS32202 interrupt controller
 *	512K to 2MB RAM
 *	128-512K flash ROM
 *	4MB expanded paged memory option
 *	Autvectored interrupts off the MF/PIC
 *
 *	Mapping
 *	000000-1FFFFF	SRAM
 *	200000-2FFFFF	Banked RAM window
 *	300000-37FFFF	Off board
 *	380000-3EFFFF	Flash/EPROM
 *	3F0000-3FFFFF	I/O on the ECB bus
 *
 *	I/O space on the ECB
 *	0x40	MF/PIC board base (PPIDE 0x44)
 *		0x40	32202
 *		0x42	cfg
 *		0x43	rtc
 *		0x44	PPI
 *		0x48	sio	16x50
 *
 *	Low 1K, 4K or 64K can be protected
 *
 *	WIP
 *	TODO
 *	- Correct(ish) speeds
 *	- Emulate the ns202 timer
 *	- Emulate the ns202 interrupt delivery
 *	- Finish the ns202 register model
 *	- Work out why PPIDE isn't being detected
 *	- Clock and nvram emulation isn't right somewhere
 *	- Look if there is other hardware we should emulate
 *	  (eg the 4M card)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <m68k.h>
#include "16x50.h"
#include "ppide.h"
#include "rtc_bitbang.h"

/* IDE controller */
static struct ppide *ppide;
/* Serial */
static struct uart16x50 *uart;
/* RTC */
static struct rtc *rtc;
static unsigned rtc_loaded;

/* 2MB RAM */
static uint8_t ram[0x200000];
/* 128K ROM */
static uint8_t rom[0x20000];
/* Force ROM into low space for the first 8 reads */
static uint8_t u27;
/* Config register on the MFPIC */
static uint8_t mfpic_cfg;

static int trace = 0;

#define TRACE_MEM	1
#define TRACE_CPU	2
#define TRACE_UART	4
#define TRACE_PPIDE	8
#define TRACE_RTC	16

uint8_t fc;

/* Read/write macros */
#define READ_BYTE(BASE, ADDR) (BASE)[ADDR]
#define READ_WORD(BASE, ADDR) (((BASE)[ADDR]<<8) | \
			(BASE)[(ADDR)+1])
#define READ_LONG(BASE, ADDR) (((BASE)[ADDR]<<24) | \
			((BASE)[(ADDR)+1]<<16) | \
			((BASE)[(ADDR)+2]<<8) | \
			(BASE)[(ADDR)+3])

#define WRITE_BYTE(BASE, ADDR, VAL) (BASE)[ADDR] = (VAL)&0xff
#define WRITE_WORD(BASE, ADDR, VAL) (BASE)[ADDR] = ((VAL)>>8) & 0xff; \
			(BASE)[(ADDR)+1] = (VAL)&0xff
#define WRITE_LONG(BASE, ADDR, VAL) (BASE)[ADDR] = ((VAL)>>24) & 0xff; \
			(BASE)[(ADDR)+1] = ((VAL)>>16)&0xff; \
			(BASE)[(ADDR)+2] = ((VAL)>>8)&0xff; \
			(BASE)[(ADDR)+3] = (VAL)&0xff

unsigned int check_chario(void)
{
	fd_set i, o;
	struct timeval tv;
	unsigned int r = 0;

	FD_ZERO(&i);
	FD_SET(0, &i);
	FD_ZERO(&o);
	FD_SET(1, &o);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select(2, &i, &o, NULL, &tv) == -1) {
		perror("select");
		exit(1);
	}
	if (FD_ISSET(0, &i))
		r |= 1;
	if (FD_ISSET(1, &o))
		r |= 2;
	return r;
}

unsigned int next_char(void)
{
	char c;
	if (read(0, &c, 1) != 1) {
		printf("(tty read without ready byte)\n");
		return 0xFF;
	}
	return c;
}

static unsigned int irq_pending;

void recalc_interrupts(void)
{
	/*  UART autovector 1 */
	if (uart16x50_irq_pending(uart))
		m68k_set_irq(M68K_IRQ_1);
	else
		m68k_set_irq(0);
}

int cpu_irq_ack(int level)
{
	/* TODO */
	return M68K_INT_ACK_SPURIOUS;
}

/* Hardware emulation */

struct ns32202 {
	uint8_t reg[32];
	unsigned pri;
	uint16_t ct_l, ct_h;
};

#define R_HVCT	0
#define R_SVCT	1
#define R_ELTG	2
#define R_TPR	4
#define R_IPND	6
#define R_ISRV	8
#define R_IMSK	10
#define R_CSRC	12
#define R_FPRT	14
#define R_MCTL	16
#define R_OCASN	17
#define R_CIPTR	18
#define R_PDAT	19
#define R_IPS	20
#define	R_PDIR	21
#define	R_CCTL	22
#define R_CICTL	23
#define R_CSV	24
#define R_CCV	28


struct ns32202 ns202;

/* TODO: emulate mis-setting 8 v 16bit mode */
unsigned int ns202_read(unsigned int address)
{
	unsigned ns32_reg = (address >> 8) & 0x1F;
//	unsigned ns32_sti = (address >> 8) & 0x20;

	switch(ns32_reg) {
	case R_HVCT:
//		ns202_clear_int(ns32_sti);
//		ns32_hvct_recalc();
		return ns202.reg[R_HVCT];
	case R_SVCT:
//		ns32_hvct_recalc();
		return ns202.reg[R_HVCT];
	case R_CCV:
	case R_CCV + 1:
	case R_CCV + 2:
	case R_CCV + 3:
		/* The CCV can only be read when counter readings are
		   frozen, but the documentation says nothing about what
		   happens otherwise, so ignore this TODO */
	case R_TPR:
	case R_TPR + 1:
	case R_ELTG:
	case R_ELTG + 1:
	case R_IPND:
	case R_IPND + 1:
	case R_CSRC:
	case R_CSRC + 1:
	case R_IMSK:
	case R_IMSK + 1:
	case R_FPRT:
	case R_FPRT + 1:
	case R_MCTL:
	case R_OCASN:
	case R_CIPTR:
	case R_PDAT:
		/* We assume no input GPIO */
	case R_IPS:
	case R_PDIR:
	case R_CCTL:
	case R_CICTL:
	case R_CSV:
	case R_CSV + 1:
	case R_CSV + 2:
	case R_CSV + 3:
	default:
		return ns202.reg[ns32_reg];
	}
}

void ns202_write(unsigned int address, unsigned int value)
{
	unsigned ns32_reg = (address >> 8) & 0x1F;
//	unsigned ns32_sti = (address >> 8) & 0x20;

	switch(ns32_reg) {
	case R_HVCT:
		ns202.reg[R_HVCT] = value;
		break;
	case R_SVCT:
		ns202.reg[R_HVCT] &= 0x0F;
		ns202.reg[R_HVCT] |= (value & 0xF0);
		break;
	/* TODO: IPND write is special forms */
	case R_IPND:
	case R_IPND + 1:
		break;
	case R_FPRT:
		value &= 0x0F;
		/* TODO special processing */
		break;
	case R_FPRT + 1:
		/* Not writeable */
		break;
	case R_CCTL:
		/* Never see CDCRL or CDCRH 1 */
		ns202.reg[ns32_reg] &= 0xFC;
		ns202.reg[ns32_reg] = value;
		/* Need to process single cycle decrementer here TODO */
		break;
	case R_CICTL:
		if (value & 0x08) {
			ns202.reg[ns32_reg] &= 0xF0;
			ns202.reg[ns32_reg] |= value & 7;
		}
		if (value & 0x80) {
			ns202.reg[ns32_reg] &= 0x0F;
			ns202.reg[ns32_reg] |= value & 70;
		}
		break;
	case R_CCV:
	case R_CCV + 1:
	case R_CCV + 2:
	case R_CCV + 3:
	/* Just adjust the register */
	case R_TPR:
	case R_TPR + 1:
	case R_ELTG:
	case R_ELTG + 1:
	case R_IMSK:
	case R_IMSK + 1:
	case R_CSRC:
	case R_CSRC + 1:
	case R_MCTL:
	case R_OCASN:
	case R_CIPTR:
	case R_PDAT:
		/* We assume no output GPIO activity */
	case R_IPS:
	case R_PDIR:
	case R_CSV:
	case R_CSV + 1:
	case R_CSV + 2:
	case R_CSV + 3:
	default:
		ns202.reg[ns32_reg] = value;
	}
}

void ns202_tick(unsigned clocks)
{
}

void ns202_raise(unsigned irq)
{
	unsigned ib = 1 << (irq & 7);
	unsigned ir = (irq & 8) ? 1 : 0;
	if (ns202.reg[R_MCTL] & 0x08)	/* FRZ */
		return;
	ns202.reg[R_IPND + ir] |= ib;
}

void ns202_clear(unsigned irq)
{
}

void ns202_reset(void)
{
	ns202.reg[R_IMSK] = 0xFF;
	ns202.reg[R_IMSK + 1] = 0xFF;
	ns202.reg[R_CIPTR] = 0xFF;
}
	
static unsigned int cfg_read(void)
{
	return mfpic_cfg;
}

static void cfg_write(unsigned int value)
{
	/* 7-3 user */
	/* Bit 2 masks upper 8 interrupts */
	/* 1:0 shift value for interrupt vector */
	mfpic_cfg = value;
}

/* Remap the bits as the MF/PIC doesn't follow the usual RBC/RC2014 mapping */

static unsigned rtc_remap_w(unsigned v)
{
	unsigned r = 0;
	if (v & 1)		/* Data / Data */
		r |= 0x80;
	if (!(v & 2))		/* Write / /Write */
		r |= 0x20;
	if (v & 4)		/* Clock / Clock */
		r |= 0x40;
	if (!(v & 8))		/* Reset / /Reset */
		r |= 0x10;
	return r;
}

static unsigned rtc_remap_r(unsigned v)
{
	unsigned r = 0;
	if (v & 0x01)		/* Data in */
		r |= 0x01;
	return r;
}

/* Read data from RAM, ROM, or a device */
unsigned int do_cpu_read_byte(unsigned int address, unsigned debug)
{
	address &= 0x3FFFFF;
	if (!(u27 & 0x80)) {
		if (debug == 0) {
			u27 <<= 1;
			u27 |= 1; 
		}
		return rom[address & 0x1FFFF];
	}
	if (debug == 0) {
		u27 <<= 1;
		u27 |= 1;
	}
	if (address < 0x200000)
		return ram[address];
	if (address < 0x380000)
		return 0xFF;
	if (address < 0x3F0000)
		return rom[address & 0x1FFFF];
	/* I/O space */
	/* Disassembler doesn't trigger I/O side effects */
	if (debug)
		return 0xFF;
	address &= 0xFFFF;
	switch(address & 0xFF) {
	case 0x40:
		return ns202_read(address);
	case 0x42:
		return cfg_read();
	case 0x43:
		return rtc_remap_r(rtc_read(rtc));
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
		return ppide_read(ppide, address & 0x03);
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
	case 0x4C:
	case 0x4D:
	case 0x4E:
	case 0x4F:
		return uart16x50_read(uart, address & 0x07);
	}
	return 0xFF;
}

unsigned int cpu_read_byte(unsigned int address)
{
	unsigned int v = do_cpu_read_byte(address, 0);
	if (trace & TRACE_MEM)
		fprintf(stderr, "RB %06X -> %02X\n", address, v);
	return v;
}

unsigned int do_cpu_read_word(unsigned int address, unsigned int debug)
{
	return (do_cpu_read_byte(address, debug) << 8) | do_cpu_read_byte(address + 1, debug);
}

unsigned int cpu_read_word(unsigned int address)
{
	unsigned int v = do_cpu_read_word(address, 0);
	if (trace & TRACE_MEM)
		fprintf(stderr, "RW %06X -> %04X\n", address, v);
	return v;
}

unsigned int cpu_read_word_dasm(unsigned int address)
{
	return do_cpu_read_word(address, 1);
}

unsigned int cpu_read_long(unsigned int address)
{
	return (cpu_read_word(address) << 16) | cpu_read_word(address + 2);
}

unsigned int cpu_read_long_dasm(unsigned int address)
{
	return (cpu_read_word_dasm(address) << 16) | cpu_read_word_dasm(address + 2);
}

void cpu_write_byte(unsigned int address, unsigned int value)
{
	address &= 0x3FFFFF;
	if (!(u27 & 0x80)) {
		u27 <<= 1;
		u27 |= 1;
		return;
	}
	u27 <<= 1;
	u27 |= 1;
	if (address < 0x200000) {
		ram[address] = value;
		return;
	}
	if (address < 0x3F0000) {
		if (trace & TRACE_MEM)
			fprintf(stderr,  "%06x: write to invalid space.\n", address);
		return;
	}
	address &= 0xFF;
	switch(address) {
	case 0x40:
		ns202_write(address, value);
		return;
	case 0x42:
		cfg_write(value);
		return;
	case 0x43:
		rtc_write(rtc, rtc_remap_w(value));
		return;
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
		ppide_write(ppide, address & 0x03, value);
		return;
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
	case 0x4C:
	case 0x4D:
	case 0x4E:
	case 0x4F:
		uart16x50_write(uart, address & 7, value);
		return;
	}
}

void cpu_write_word(unsigned int address, unsigned int value)
{
	address &= 0xFFFFFF;

	if (trace & TRACE_MEM)
		fprintf(stderr, "WW %06X <- %04X\n", address, value);

	cpu_write_byte(address, value >> 8);
	cpu_write_byte(address + 1, value & 0xFF);
}

void cpu_write_long(unsigned int address, unsigned int value)
{
	address &= 0xFFFFFF;

	cpu_write_word(address, value >> 16);
	cpu_write_word(address + 2, value & 0xFFFF);
}

void cpu_write_pd(unsigned int address, unsigned int value)
{
	address &= 0xFFFFFF;

	cpu_write_word(address + 2, value & 0xFFFF);
	cpu_write_word(address, value >> 16);
}

void cpu_instr_callback(void)
{
	if (trace & TRACE_CPU) {
		char buf[128];
		unsigned int pc = m68k_get_reg(NULL, M68K_REG_PC);
		m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
		fprintf(stderr, ">%06X %s\n", pc, buf);
	}
}

static void device_init(void)
{
	irq_pending = 0;
	ppide_reset(ppide);
	uart16x50_reset(uart);
	uart16x50_set_input(uart, 1);
}

static struct termios saved_term, term;

static void cleanup(int sig)
{
	tcsetattr(0, 0, &saved_term);
	if (rtc_loaded)
		rtc_save(rtc, "mini68k.nvram");
	exit(1);
}

static void exit_cleanup(void)
{
	if (rtc_loaded)
		rtc_save(rtc, "mini68k.nvram");
	tcsetattr(0, 0, &saved_term);
}

static void take_a_nap(void)
{
	struct timespec t;
	t.tv_sec = 0;
	t.tv_nsec = 100000;
	if (nanosleep(&t, NULL))
		perror("nanosleep");
}

void cpu_pulse_reset(void)
{
	device_init();
}

void cpu_set_fc(int fc)
{
}

void usage(void)
{
	fprintf(stderr, "mini68k: [-0][-1][-2][-e][-r rompath][-i idepath][-d debug].\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int fd;
	int cputype = M68K_CPU_TYPE_68000;
	int fast = 0;
	int opt;
	const char *romname = "mini-128.rom";
	const char *diskname = NULL;

	while((opt = getopt(argc, argv, "012efd:i:r:")) != -1) {
		switch(opt) {
		case '0':
			cputype = M68K_CPU_TYPE_68000;
			break;
		case '1':
			cputype = M68K_CPU_TYPE_68010;
			break;
		case '2':
			cputype = M68K_CPU_TYPE_68020;
			break;
		case 'e':
			cputype = M68K_CPU_TYPE_68EC020;
			break;
		case 'f':
			fast = 1;
			break;
		case 'd':
			trace = atoi(optarg);
			break;
		case 'i':
			diskname = optarg;
			break;
		case 'r':
			romname = optarg;
			break;
		default:
			usage();
		}
	}

	if (tcgetattr(0, &term) == 0) {
		saved_term = term;
		atexit(exit_cleanup);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, cleanup);
		signal(SIGTSTP, SIG_IGN);
		term.c_lflag &= ~ICANON;
		term.c_iflag &= ~(ICRNL | IGNCR);
		term.c_cc[VMIN] = 1;
		term.c_cc[VTIME] = 0;
		term.c_cc[VINTR] = 0;
		term.c_cc[VSUSP] = 0;
		term.c_cc[VEOF] = 0;
		term.c_lflag &= ~(ECHO | ECHOE | ECHOK);
		tcsetattr(0, 0, &term);
	}

	if (optind < argc)
		usage();

	memset(ram, 0xA7, sizeof(ram));

	fd = open(romname, O_RDONLY);
	if (fd == -1) {
		perror(romname);
		exit(1);
	}
	if (read(fd, rom, 0x20000) != 0x20000) {
		fprintf(stderr, "%s: too short.\n", romname);
		exit(1);
	}
	close(fd);

	ppide = ppide_create("hd0");
	ppide_reset(ppide);
	if (diskname) {
		fd = open(diskname, O_RDWR);
		if (fd == -1) {
			perror(diskname);
			exit(1);
		}
		if (ppide == NULL)
			exit(1);
		if (ppide_attach(ppide, 0, fd))
			exit(1);
	}
	ppide_trace(ppide, trace & TRACE_PPIDE);

	uart = uart16x50_create();
	if (trace & TRACE_UART)
		uart16x50_trace(uart, 1);

	rtc = rtc_create();
	rtc_reset(rtc);
	rtc_trace(rtc, trace & TRACE_RTC);
	rtc_load(rtc, "mini68k.nvram");
	rtc_loaded = 1;

	m68k_init();
	m68k_set_cpu_type(cputype);
	m68k_pulse_reset();

	/* Init devices */
	device_init();

	while (1) {
		/* Approximate a 68008 */
		m68k_execute(400);
		uart16x50_event(uart);
		if (!fast)
			take_a_nap();
	}
}
