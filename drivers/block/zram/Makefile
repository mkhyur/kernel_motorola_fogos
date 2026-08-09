# SPDX-License-Identifier: GPL-2.0-only
zram-y	:=	zcomp.o zram_drv.o

# CONFIG_ZRAM is a compatibility alias for the hybridswap zram driver
# (drivers/moto_swap). Building the upstream driver here would clash
# on the "zram" blkdev and "zram-control" class names.
ifneq ($(CONFIG_HYBRIDSWAP_ZRAM),y)
obj-$(CONFIG_ZRAM)	+=	zram.o
endif
