// SPDX-License-Identifier: GPL-2.0
/*
 * panelread: esperimenti di lettura dal controller del pannello (ILI9805)
 * attraverso l'LCDIF in modalita' PIO.  panelread CMD NLETTURE
 * Va eseguito da root con il server grafico in pausa.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile uint32_t *r;
#define CTRL 0x00
#define CTRL1 0x10
#define TC 0x30
#define DATA 0x180
#define STAT 0x1b0
#define R(o) r[(o) / 4]
#define SET(o, v) r[((o) + 4) / 4] = (v)
#define CLR(o, v) r[((o) + 8) / 4] = (v)

static int idle(void)
{
	for (int i = 0; i < 2000000; i++)
		if (!(R(CTRL) & 1) && (R(STAT) & 0x16000000) == 0x14000000)
			return 0;
	return -1;
}

static void wr(uint8_t b, int data)
{
	idle();
	R(TC) = 0x00010001;
	CLR(CTRL, 0x10010001);	/* DATA_SELECT, READ, RUN */
	if (data)
		SET(CTRL, 0x10000);
	SET(CTRL, 1);
	R(DATA) = b;
	idle();
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	int cmd = strtol(argv[1], 0, 0), n = atoi(argv[2]);
	uint32_t pk;

	r = mmap(0, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80030000);
	if (r == MAP_FAILED || idle())
		return perror("lcdif"), 1;
	pk = R(CTRL1) & 0xf0000;
	CLR(CTRL, 0x20);		/* MASTER off */
	CLR(CTRL1, 0xf0000);
	SET(CTRL1, 0x30000);
	wr(cmd, 0);
	{
		uint32_t ctrl = R(CTRL);
		int t, i;

		/* lettura: parole da 8 bit, 4 per parola della FIFO */
		CLR(CTRL1, 0xf0000);
		SET(CTRL1, 0xf0000);
		CLR(CTRL, 0x00000f00);
		SET(CTRL, 0x00000500);		/* WORD_LENGTH 8, bus 8 bit */
		R(TC) = 0x00010000 | (n * 4);
		CLR(CTRL, 0x10001);
		SET(CTRL, 0x10010000);
		SET(CTRL, 1);
		for (i = 0; i < n; i++) {
			for (t = 0; t < 300000 && (R(STAT) & (1 << 28)); t++)
				;
			printf("%08x%s ", R(DATA), t == 300000 ? "!" : "");
		}
		printf("stat=%08x ctrl=%08x", R(STAT), R(CTRL));
		CLR(CTRL, 1);
		CLR(CTRL, 0x10000f00);
		SET(CTRL, ctrl & 0xf00);
		SET(CTRL1, 0x200000); CLR(CTRL1, 0x200000);	/* FIFO_CLEAR */
	}
	printf("\n");
	CLR(CTRL, 0x10010001);
	wr(0x00, 0);			/* NOP: chiude la lettura */
	CLR(CTRL1, 0xf0000);
	SET(CTRL1, pk);
	SET(CTRL, 0x20);
	return 0;
}
