obj-m += os_hello.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
KCC ?= bash $(abspath ../../scripts/kbuild-gcc-wrapper.sh)

.PHONY: all modules clean
all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) CC="$(KCC)" HOSTCC=gcc modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
