# For programming into bootloader section:
# Using the same fuses/bootloader sizes as in arduino promini 5V/16MHz
BOOTSTART=0x7800
# For compiling:
CC = avr-gcc
OBJCOPY = avr-objcopy
#CFLAGS= -DCOMPILED="\"`date +'Build %H:%M:%S %d/%m/%Y'`\"" -std=c99 -O -g -Wall
CFLAGS= -std=c99 -DF_CPU=16000000UL -O -g -Wall -mmcu=`[ -f MCU ] && cat MCU || cat $*.mcu` -DBOOTSTART=$(BOOTSTART) -Wl,--section-start=.text=$(BOOTSTART)
DUDEFLAGS=-v -v -F -p `[ -f PARTNO ] && cat PARTNO || cat $*.partno` -cusbasp -F
#DUDEFLAGS=-v -v -F -p `[ -f PARTNO ] && cat PARTNO || cat $*.partno` -carduino -P /dev/ttyUSB0 -b 57600

wps-test.elf:wps-test.o timer.o
	$(CC) $(CFLAGS) $^ -o $@
	avr-size $@
%.list: %.c
	$(CC) -c $(CFLAGS) -E $< |less
%.o:%.c
	$(CC) -c $(CFLAGS) $*.c -o $@
%.elf:%.o
	$(CC) $(CFLAGS) $*.o -o $@
	avr-size $@
%.hex: %.elf
	$(OBJCOPY) -j .text -j .data -O ihex $< $@
%.bin: %.hex
	$(OBJCOPY) -I ihex -O binary $< $@
%.program:%.hex
	avrdude $(DUDEFLAGS) -U flash:w:$<

# Copy fuses (if there is a .fuse section in the executable)
%.fuses: %.elf
	$(OBJCOPY) -j .fuse -O binary $*.elf $@
%.lfuse: %.fuses
	dd bs=1  count=1 if=$< of=$@
%.hfuse: %.fuses
	dd bs=1  count=1 skip=1 if=$< of=$@
%.efuse: %.fuses
	dd bs=1  count=1 skip=2 if=$< of=$@
%.putfuses: %.lfuse %.hfuse %.efuse
	avrdude $(DUDEFLAGS) -U hfuse:w:$*.hfuse:r -U lfuse:w:$*.lfuse:r -U efuse:w:$*.efuse:r
%.partno: %.mcu
	sed -e 's/atmega\([0123456789]*\)[a]/m\1/i' \
	    -e 's/attiny\([0123456789]*\)[a]/t\1/i' <$< >$@

%.bitclean:
	rm -f $*.o $*.elf $*.hex core

.PRECIOUS:*.bitclean *.boardclean
%.clean:%.bitclean %.boardclean
	:

# For boards:
%.boardclean:
	rm -f $*.*.gbr $*.*.cnc $*.*.bak* $*.sch~ $*.pcb- $*.net \
	$*.bom $*.cmd *.'sch#' $*.xy $*.bom $*.new.pcb
%.project:
	echo schematics $*.sch >>$@
	echo output-name $* >>$@
	echo elements-dir $(HOME)/share/pcb/pcblib-newlib >>$@
%.pcb: %.sch %.project
	gsch2pcb --use-files $*.project
