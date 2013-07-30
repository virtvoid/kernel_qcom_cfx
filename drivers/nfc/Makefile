#
# Makefile for nfc devices
#

ifdef CONFIG_MACH_APQ8064_FIND5
obj-$(CONFIG_PN544_NFC)		+= oppo_pn544.o
else
obj-$(CONFIG_PN544_NFC)		+= pn544.o
endif
obj-$(CONFIG_NFC_PN533)		+= pn533.o
obj-$(CONFIG_NFC_WILINK)	+= nfcwilink.o
obj-$(CONFIG_BCM2079X)		+= bcm2079x-i2c.o

ccflags-$(CONFIG_NFC_DEBUG) := -DDEBUG
