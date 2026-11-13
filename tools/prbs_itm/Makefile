# make RAM_BASE=0x20400000   -> prbs_itm.elf and prbs_itm.bin
RAM_BASE ?= 0x20400000
RAM_SIZE ?= 0x1000
CROSS ?= arm-none-eabi-
CPU ?= cortex-m7

all: prbs_itm.bin

prbs_itm.elf: prbs_itm.c prbs_itm.ld
	$(CROSS)gcc -mcpu=$(CPU) -mthumb -O1 -nostdlib -ffreestanding \
	  -Wl,--defsym=RAM_BASE=$(RAM_BASE) -Wl,--defsym=RAM_SIZE=$(RAM_SIZE) \
	  -T prbs_itm.ld -o $@ prbs_itm.c

prbs_itm.bin: prbs_itm.elf
	$(CROSS)objcopy -O binary $< $@

clean:
	rm -f prbs_itm.elf prbs_itm.bin
