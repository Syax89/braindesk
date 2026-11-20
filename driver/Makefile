obj-m := brain_pxp.o

KDIR ?= /kernel

all: module pxptest pxpvideo

pxpvideo: pxpvideo.c brain_pxp_uapi.h
	$(CROSS_COMPILE)gcc -O2 -Wall -static -o $@ pxpvideo.c

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

pxptest: pxptest.c brain_pxp_uapi.h
	$(CROSS_COMPILE)gcc -O2 -Wall -static -o $@ pxptest.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f pxptest pxpvideo
