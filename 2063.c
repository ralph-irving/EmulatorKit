/*
 *	Another Z80 and Zilog peripherals build
 *
 *	Z80 at 10MHz
 *
 *	8bit GPIO input at 0x00
 *	8bit GPIO output at 0x10
 *	Printer data at 0x20
 *	SIO at 0x30
 *	CTC at 0x40
 *	Flash disable at 0x70
 *
 *	Bitbang SD
 *
 *	Boots from a flash which is then kicked out. Flash is
 *	write through.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "event.h"
#include "serialdevice.h"
#include "ttycon.h"
#include "vtcon.h"
#include "16x50.h"
#include "z80sio.h"
#include "sdcard.h"
#include "system.h"
#include "event.h"
#include "2063ui.h"
#include "joystick.h"
#include "tms9918a.h"
#include "tms9918a_render.h"
#include "libz80/z80.h"
#include "z80dis.h"

static uint8_t fast = 0;
static uint8_t int_recalc = 0;
static uint8_t gpio_out;
static uint8_t gpio_in = 0xFF;		/* SD not present, printer floating */
static uint8_t flash_in = 1;
static struct sdcard *sdcard;
static struct tms9918a *vdp;
static struct tms9918a_renderer *vdprend;
static struct uart16x50 *uart;
static struct z80_sio *sio;

static uint8_t ram[16 * 32768];
static uint8_t rom[65536];
static uint16_t rom_mask = 0x3FFF;

static uint16_t tstate_steps = 50;	/* 10MHz speed */

/* IRQ source that is live in IM2 */
static uint8_t live_irq;

#define IRQ_SIO		1
#define IRQ_CTC		3	/* 3 4 5 6 */
#define INT_UART	4
/* TOOD: PIO */

#define VDP_J7		(1 << 1)	/* A8_1, U6, pin 4 (D1) */

static Z80Context cpu_z80;

volatile int emulator_done;

#define TRACE_MEM	0x000001
#define TRACE_IO	0x000002
#define TRACE_ROM	0x000004
#define TRACE_UNK	0x000008
#define TRACE_CPU	0x000010
#define TRACE_BANK	0x000020
#define TRACE_SIO	0x000040
#define TRACE_CTC	0x000080
#define TRACE_IRQ	0x000100
#define TRACE_SPI	0x000200
#define TRACE_SD	0x000400
#define TRACE_TMS9918A	0x000800
#define TRACE_JOY	0x001000
#define TRACE_UART	0x002000

static int trace = 0;

static void reti_event(void);

static uint8_t *map_addr(uint16_t addr, unsigned is_write)
{
	unsigned bank;
	if (flash_in && !is_write)
		return rom + (addr & rom_mask);
	if (addr >= 0x8000)
		bank = 15;
	else
		bank = gpio_out >> 4;
	return ram + (bank * 0x8000) + (addr & 0x7FFF);
}

static uint8_t do_mem_read(uint16_t addr, int quiet)
{
	uint8_t *p = map_addr(addr, 0);
	uint8_t r = *p;
	if ((trace & TRACE_MEM) && !quiet)
		fprintf(stderr, "R %04x = %02X\n", addr, r) ;
	return r;
}

void mem_write(int unused, uint16_t addr, uint8_t val)
{
	uint8_t *p = map_addr(addr, 1);
	if (trace & TRACE_MEM)
		fprintf(stderr, "W %04x = %02X\n",
			addr, val);
	*p = val;
}

uint8_t mem_read(int unused, uint16_t addr)
{
	static uint8_t rstate = 0;
	uint8_t r = do_mem_read(addr, 0);

	if (cpu_z80.M1) {
		/* DD FD CB see the Z80 interrupt manual */
		if (r == 0xDD || r == 0xFD || r == 0xCB) {
			rstate = 2;
			return r;
		}
		/* Look for ED with M1, followed directly by 4D and if so trigger
		   the interrupt chain */
		if (r == 0xED && rstate == 0) {
			rstate = 1;
			return r;
		}
	}
	if (r == 0x4D && rstate == 1)
		reti_event();
	rstate = 0;
	return r;
}

static unsigned int nbytes;

uint8_t z80dis_byte(uint16_t addr)
{
	uint8_t r = do_mem_read(addr, 1);
	fprintf(stderr, "%02X ", r);
	nbytes++;
	return r;
}

uint8_t z80dis_byte_quiet(uint16_t addr)
{
	return do_mem_read(addr, 1);
}

static void z80_trace(unsigned unused)
{
	static uint32_t lastpc = -1;
	char buf[256];

	if ((trace & TRACE_CPU) == 0)
		return;
	nbytes = 0;
	/* Spot XXXR repeating instructions and squash the trace */
	if (cpu_z80.M1PC == lastpc && z80dis_byte_quiet(lastpc) == 0xED &&
		(z80dis_byte_quiet(lastpc + 1) & 0xF4) == 0xB0) {
		return;
	}
	lastpc = cpu_z80.M1PC;
	fprintf(stderr, "%04X: ", lastpc);
	z80_disasm(buf, lastpc);
	while(nbytes++ < 6)
		fprintf(stderr, "   ");
	fprintf(stderr, "%-16s ", buf);
	fprintf(stderr, "[ %02X:%02X %04X %04X %04X %04X %04X %04X ]\n",
		cpu_z80.R1.br.A, cpu_z80.R1.br.F,
		cpu_z80.R1.wr.BC, cpu_z80.R1.wr.DE, cpu_z80.R1.wr.HL,
		cpu_z80.R1.wr.IX, cpu_z80.R1.wr.IY, cpu_z80.R1.wr.SP);
}

void recalc_interrupts(void)
{
	int_recalc = 1;
}

void uart16x50_signal_change(struct uart16x50 *uart, uint8_t bits)
{
}

/*
 *	Z80 CTC
 */

struct z80_ctc {
	uint16_t count;
	uint16_t reload;
	uint8_t vector;
	uint8_t ctrl;
#define CTC_IRQ		0x80
#define CTC_COUNTER	0x40
#define CTC_PRESCALER	0x20
#define CTC_RISING	0x10
#define CTC_PULSE	0x08
#define CTC_TCONST	0x04
#define CTC_RESET	0x02
#define CTC_CONTROL	0x01
	uint8_t irq;		/* Only valid for channel 0, so we know
				   if we must wait for a RETI before doing
				   a further interrupt */
};

#define CTC_STOPPED(c)	(((c)->ctrl & (CTC_TCONST|CTC_RESET)) == (CTC_TCONST|CTC_RESET))

struct z80_ctc ctc[4];
uint8_t ctc_irqmask;

static void ctc_reset(struct z80_ctc *c)
{
	c->vector = 0;
	c->ctrl = CTC_RESET;
}

static void ctc_init(void)
{
	ctc_reset(ctc);
	ctc_reset(ctc + 1);
	ctc_reset(ctc + 2);
	ctc_reset(ctc + 3);
}

static void ctc_interrupt(struct z80_ctc *c)
{
	int i = c - ctc;
	if (c->ctrl & CTC_IRQ) {
		if (!(ctc_irqmask & (1 << i))) {
			ctc_irqmask |= 1 << i;
			recalc_interrupts();
			if (trace & TRACE_CTC)
				fprintf(stderr, "CTC %d wants to interrupt.\n", i);
		}
	}
}

static void ctc_reti(int ctcnum)
{
	if (ctc_irqmask & (1 << ctcnum)) {
		ctc_irqmask &= ~(1 << ctcnum);
		if (trace & TRACE_IRQ)
			fprintf(stderr, "Acked interrupt from CTC %d.\n", ctcnum);
	}
}

/* After a RETI or when idle compute the status of the interrupt line and
   if we are head of the chain this time then raise our interrupt */

static int ctc_check_im2(void)
{
	if (ctc_irqmask) {
		int i;
		for (i = 0; i < 4; i++) {	/* FIXME: correct order ? */
			if (ctc_irqmask & (1 << i)) {
				uint8_t vector = ctc[0].vector & 0xF8;
				vector += 2 * i;
				if (trace & TRACE_IRQ)
					fprintf(stderr, "New live interrupt is from CTC %d vector %x.\n", i, vector);
				live_irq = IRQ_CTC + i;
				Z80INT(&cpu_z80, vector);
				return 1;
			}
		}
	}
	return 0;
}

/* Model the chains between the CTC devices */
static void ctc_pulse(int i)
{
}
#if 0
/* We don't worry about edge directions just a logical pulse model */
static void ctc_receive_pulse(int i)
{
	struct z80_ctc *c = ctc + i;
	if (c->ctrl & CTC_COUNTER) {
		if (CTC_STOPPED(c))
			return;
		if (c->count >= 0x0100)
			c->count -= 0x100;	/* No scaling on pulses */
		if ((c->count & 0xFF00) == 0) {
			ctc_interrupt(c);
			ctc_pulse(i);
			c->count = c->reload << 8;
		}
	} else {
		if (c->ctrl & CTC_PULSE)
			c->ctrl &= ~CTC_PULSE;
	}
}
#endif

/* Model counters */
static void ctc_tick(unsigned int clocks)
{
	struct z80_ctc *c = ctc;
	int i;
	int n;
	int decby;

	for (i = 0; i < 4; i++, c++) {
		/* Waiting a value */
		if (CTC_STOPPED(c))
			continue;
		/* Pulse trigger mode */
		if (c->ctrl & CTC_COUNTER)
			continue;
		/* 256x downscaled */
		decby = clocks;
		/* 16x not 256x downscale - so increase by 16x */
		if (!(c->ctrl & CTC_PRESCALER))
			decby <<= 4;
		/* Now iterate over the events. We need to deal with wraps
		   because we might have something counters chained */
		n = c->count - decby;
		while (n < 0) {
			ctc_interrupt(c);
			ctc_pulse(i);
			if (c->reload == 0)
				n += 256 << 8;
			else
				n += c->reload << 8;
		}
		c->count = n;
	}
}

static void ctc_write(uint8_t channel, uint8_t val)
{
	struct z80_ctc *c = ctc + channel;
	if (c->ctrl & CTC_TCONST) {
		if (trace & TRACE_CTC)
			fprintf(stderr, "CTC %d constant loaded with %02X\n", channel, val);
		c->reload = val;
		if ((c->ctrl & (CTC_TCONST|CTC_RESET)) == (CTC_TCONST|CTC_RESET)) {
			c->count = (c->reload - 1) << 8;
			if (trace & TRACE_CTC)
				fprintf(stderr, "CTC %d constant reloaded with %02X\n", channel, val);
		}
		c->ctrl &= ~CTC_TCONST|CTC_RESET;
	} else if (val & CTC_CONTROL) {
		/* We don't yet model the weirdness around edge wanted
		   toggling and clock starts */
		if (trace & TRACE_CTC)
			fprintf(stderr, "CTC %d control loaded with %02X\n", channel, val);
		c->ctrl = val;
		if ((c->ctrl & (CTC_TCONST|CTC_RESET)) == CTC_RESET) {
			c->count = (c->reload - 1) << 8;
			if (trace & TRACE_CTC)
				fprintf(stderr, "CTC %d constant reloaded with %02X\n", channel, val);
		}
		/* Undocumented */
		if (!(c->ctrl & CTC_IRQ) && (ctc_irqmask & (1 << channel))) {
			ctc_irqmask &= ~(1 << channel);
			if (ctc_irqmask == 0) {
				if (trace & TRACE_IRQ)
					fprintf(stderr, "CTC %d irq reset.\n", channel);
				if (live_irq == IRQ_CTC + channel)
					live_irq = 0;
			}
		}
	} else {
		if (trace & TRACE_CTC)
			fprintf(stderr, "CTC %d vector loaded with %02X\n", channel, val);
		/* Only works on channel 0 */
		if (channel == 0)
			c->vector = val;
	}
}

static uint8_t ctc_read(uint8_t channel)
{
	uint8_t val = ctc[channel].count >> 8;
	if (trace & TRACE_CTC)
		fprintf(stderr, "CTC %d reads %02x\n", channel, val);
	return val;
}

static uint8_t bitcnt;
static uint8_t txbits, rxbits;

static void spi_clock_high(void)
{
	txbits <<= 1;
	txbits |= gpio_out & 1;
	bitcnt++;
	if (bitcnt == 8) {
		rxbits = sd_spi_in(sdcard, txbits);
		if (trace & TRACE_SPI)
			fprintf(stderr, "spi %02X | %02X\n", rxbits, txbits);
		bitcnt = 0;
	}
}

static void spi_clock_low(void)
{
	gpio_in &= 0x7F;
	gpio_in |= (rxbits & 0x80);
	rxbits <<= 1;
	rxbits |= 1;
}

/* GPIO output lines. The bank map is handled directly
   whilst we handle bits 2-0 here. 3 is the printer
   strobe but we don't emulate a printer */
static void gpio_write(uint16_t addr, uint8_t val)
{
	uint8_t delta = gpio_out ^ val;
	gpio_out = val;
	if ((delta & 0xF0) && (trace & TRACE_BANK))
		fprintf(stderr, "bank: %d\n", val >> 4);
	if (delta & 4) {
		if (gpio_out & 4)
			sd_spi_raise_cs(sdcard);
		else {
			sd_spi_lower_cs(sdcard);
			bitcnt = 0;
		}
	}
	if (delta & 2) {
		if (gpio_out & 2)
			spi_clock_high();
		else
			spi_clock_low();
	}
}

/* Channel is A0, C/D is A1 */
static unsigned sio_port[4] = {
	SIOA_D,
	SIOB_D,
	SIOA_C,
	SIOB_C
};

uint8_t io_read(int unused, uint16_t addr)
{
	if (trace & TRACE_IO)
		fprintf(stderr, "read %02x\n", addr);
	addr &= 0xFF;

	switch(addr & 0xF0) {
		case 0x00:
			return gpio_in;
		case 0x30:
			return sio_read(sio, sio_port[addr & 3]);
		case 0x40:
			return ctc_read(addr & 3);
		case 0x50:
			if (uart && (addr & 8))
				return uart16x50_read(uart, addr & 7);
			break;
		case 0x70:
			flash_in = 0;
			return 0xFF;
		case 0x80:
			if (vdp)
				return tms9918a_read(vdp, addr & 1);
			break;
		case 0xA0:
			if (vdp) {
				if ( addr == 0xA8 ) {
					uint8_t joy0port;
					joy0port = joystick_read(0);
					if (tms9918a_irq_pending(vdp)) {
						joy0port &= ~VDP_J7;
						if (trace & TRACE_IRQ)
							fprintf (stderr,"VDP IRQ pending via J7: %02X\n", joy0port);
					}
					return joy0port;
				} else if ( addr == 0xA9 )
					return joystick_read(1);
			}
	}
	if (trace & TRACE_UNK)
		fprintf(stderr, "Unknown read from port %04X\n", addr);
	/* 2063 has pullups on the data bus */
	return 0xFF;
}

void io_write(int unused, uint16_t addr, uint8_t val)
{
	if (trace & TRACE_IO)
		fprintf(stderr, "write %02x <- %02x\n", addr, val);
	addr &= 0xFF;
	switch(addr & 0xF0) {
	case 0x10:
		gpio_write(addr, val);
		return;
	case 0x20:
		/* Printer */
		break;
	case 0x30:
		sio_write(sio, sio_port[addr & 3], val);
		return;
	case 0x40:
		ctc_write(addr & 3, val);
		return;
	case 0x50:
		if (uart && (addr & 8)) {
			uart16x50_write(uart, addr & 7, val);
			return;
		}
		break;
	case 0x80:
		if (vdp){
			tms9918a_write(vdp, addr & 1, val);
			return;
		}
		break;
	}
	if (addr == 0xFD) {
		trace &= 0xFF00;
		trace |= val;
		fprintf(stderr, "trace set to %04X\n", trace);
	} else if (addr == 0xFE) {
		trace &= 0xFF;
		trace |= val << 8;
		printf("trace set to %04X\n", trace);
	} else if (trace & TRACE_UNK)
		fprintf(stderr, "Unknown write to port %04X of %02X\n", addr, val);
}

static void poll_irq_event(void)
{
	unsigned v;

	if (live_irq)
		return;

	v = sio_check_im2(sio);
	if (v >= 0) {
		live_irq = IRQ_SIO;
		Z80INT(&cpu_z80, v);
		return;
	}
	if (!ctc_check_im2()) {
		if (uart && uart16x50_irq_pending(uart))
				Z80INT(&cpu_z80, 0xFF);
	}
	/* If a real IM2 source is live then the serial int won't be seen */
}

static void reti_event(void)
{
	if (live_irq && (trace & TRACE_IRQ))
		fprintf(stderr, "RETI\n");
	switch(live_irq) {
	case IRQ_SIO:
		sio_reti(sio);
		break;
	case IRQ_CTC:
	case IRQ_CTC + 1:
	case IRQ_CTC + 2:
	case IRQ_CTC + 3:
		ctc_reti(live_irq - IRQ_CTC);
		break;
	}
	live_irq = 0;
	poll_irq_event();
}

static struct termios saved_term, term;

static void cleanup(int sig)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
	emulator_done = 1;
}

static void exit_cleanup(void)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
}

static void usage(void)
{
	fprintf(stderr, "2063: [-1] [-r rompath] [-S sdcard] [-T] [-f] [-d debug]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	static struct timespec tc;
	int opt;
	int fd;
	char *rompath = "2063.rom";
	char *sdpath = NULL;
	unsigned have_tms = 0;
	unsigned have_16x50 = 0;
	unsigned rsize;

	while ((opt = getopt(argc, argv, "d:fr:S:T")) != -1) {
		switch (opt) {
		case 1:
			have_16x50 = 1;
			break;
		case 'r':
			rompath = optarg;
			break;
		case 'S':
			sdpath = optarg;
			break;
		case 'd':
			trace = atoi(optarg);
			break;
		case 'f':
			fast = 1;
			break;
		case 'T':
			have_tms = 1;
			break;
		default:
			usage();
		}
	}
	if (optind < argc)
		usage();

	fd = open(rompath, O_RDONLY);
	if (fd == -1) {
		perror(rompath);
		exit(EXIT_FAILURE);
	}
	rsize = read(fd, rom, sizeof(rom));

	if (rsize == 0 || (rsize & (rsize - 1))) {
		fprintf(stderr, "2063: rom image should be a power of 2.\n");
		exit(EXIT_FAILURE);
	}
	close(fd);

	rom_mask = rsize - 1;

	sdcard = sd_create("sd0");
	if (sdpath) {
		fd = open(sdpath, O_RDWR);
		if (fd == -1) {
			perror(sdpath);
			exit(1);
		}
		sd_attach(sdcard, fd);
		gpio_in &= ~0x40;	/* Pulled down by card */
	}
	if (trace & TRACE_SD)
		sd_trace(sdcard, 1);
	sd_blockmode(sdcard);

	ui_init();

	sio = sio_create();
	sio_reset(sio);
	sio_trace(sio, 0, !!(trace & TRACE_SIO));
	sio_trace(sio, 1, !!(trace & TRACE_SIO));

	ctc_init();
	if (have_16x50) {
		uart = uart16x50_create();
		uart16x50_trace(uart, trace & TRACE_UART);
		uart16x50_attach(uart, &console);
		uart16x50_reset(uart);
		sio_attach(sio, 0, vt_create("sioa", CON_VT52));
		sio_attach(sio, 1, vt_create("siob", CON_VT52));
	} else {
		sio_attach(sio, 0, &console);
		sio_attach(sio, 1, vt_create("siob", CON_VT52));
	}

	ui_init();

	if (have_tms) {
		vdp = tms9918a_create();
		tms9918a_trace(vdp, !!(trace & TRACE_TMS9918A));
		vdprend = tms9918a_renderer_create(vdp);
		/* SDL init called in tms9918a_renderer_create */
		joystick_create();
		joystick_trace( !! (trace & TRACE_JOY ));
		js2063_add_events();
	}

	/* 60Hz for the VDP */
	tc.tv_sec = 0;
	tc.tv_nsec = 16666667L;

	if (tcgetattr(0, &term) == 0) {
		saved_term = term;
		atexit(exit_cleanup);
		signal(SIGINT, cleanup);
		signal(SIGQUIT, cleanup);
		signal(SIGPIPE, cleanup);
		term.c_lflag &= ~(ICANON | ECHO);
		term.c_cc[VMIN] = 0;
		term.c_cc[VTIME] = 1;
		term.c_cc[VINTR] = 0;
		term.c_cc[VSUSP] = 0;
		term.c_cc[VSTOP] = 0;
		tcsetattr(0, TCSADRAIN, &term);
	}

	Z80RESET(&cpu_z80);
	cpu_z80.ioRead = io_read;
	cpu_z80.ioWrite = io_write;
	cpu_z80.memRead = mem_read;
	cpu_z80.memWrite = mem_write;
	cpu_z80.trace = z80_trace;

	/* This is the wrong way to do it but it's easier for the moment. We
	   should track how much real time has occurred and try to keep cycle
	   matched with that. The scheme here works fine except when the host
	   is loaded though */

	/* We run 1000000 t-states per second */
	while (!emulator_done) {
		int i;

		if (cpu_z80.halted && ! cpu_z80.IFF1) {
			/* HALT with interrupts disabled, so nothing left
			   to do, so exit simulation. If NMI was supported,
			   this might have to change. */
			emulator_done = 1;
			break;
		}
		/* This is very slightly out but then so are most can oscillators ;) */
		/* Ideal would be about 334 x 499 */
		for (i = 0; i < 333; i++) {
			int j;
			for (j = 0; j < 10; j++) {
				Z80ExecuteTStates(&cpu_z80, tstate_steps);
				ctc_tick(tstate_steps);
				sio_timer(sio);
			}
			/* We want to run UI events regularly it seems */
			if (ui_event())
				emulator_done = 1;
		}

		/* Do 20ms of I/O and delays */
		if (vdp) {
			tms9918a_rasterize(vdp);
			tms9918a_render(vdprend);
		}
		if (!fast)
			nanosleep(&tc, NULL);
		/* If there is no pending Z80 vector IRQ but we think
		   there now might be one we use the same logic as for
		   reti */
		if (!live_irq)
			poll_irq_event();
	}
	exit(0);
}
