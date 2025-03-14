obj-m += UnixDOOM.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

ccflags-y += -Wno-cast-function-type
ccflags-y += -Wno-missing-prototypes
ccflags-y += -Wno-missing-declarations
ccflags-y += -Wno-implicit-fallthrough
ccflags-y += -Wno-unknown-pragmas
ccflags-y += -Wno-unused-but-set-parameter

all:
	make -C $(KERNELDIR) M=$(PWD) modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean
