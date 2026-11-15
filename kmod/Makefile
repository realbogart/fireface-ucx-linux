KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# Installs into /lib/modules/<kernel>/updates; run as root
install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
