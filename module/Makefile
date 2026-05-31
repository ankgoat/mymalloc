obj-m += mymalloc.o

KDIR := $(HOME)/mymalloc/linux-6.6.30

all:
	$(MAKE) -C $(KDIR) ARCH=arm64 M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=arm64 M=$(PWD) clean