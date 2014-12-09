# For programming into bootloader section:
# Using the same fuses/bootloader sizes as in arduino promini 5V/16MHz
MCU=atmega328p
PARTNO=m328p
BOOTSTART=0x7800

# Where the tools are.  Assume an arduino IDE is installed
DIR_ARDUINO=~/arduino-1.0.5
#DIR_ARDUINO=~/arduino-1.5.7
DIR_TOOLS=$(DIR_ARDUINO)/hardware/tools
DIR_BIN=$(DIR_TOOLS)/avr/bin
DIR_ETC=$(DIR_TOOLS)/avr/etc
DIR_INC=$(DIR_TOOLS)/avr/lib/avr/include
AVRDUDE=$(lastword $(shell ls $(DIR_BIN)/avrdude $(DIR_TOOLS)/avrdude))
DUDECONF=$(lastword $(shell ls $(DIR_ETC)/avrdude.conf $(DIR_TOOLS)/avrdude.conf))
CC=$(DIR_BIN)/avr-gcc
OBJCOPY=$(DIR_BIN)/avr-objcopy
SIZE=$(DIR_BIN)/avr-size

#LIBS=-L$(DIR_TOOLS)/../lib -L$(DIR_TOOLS)/../lib/avr/lib
CFLAGS= -std=c99 -DF_CPU=16000000UL -O -g -Wall -mmcu=$(MCU) -DBOOTSTART=$(BOOTSTART) -Wl,--section-start=.text=$(BOOTSTART) $(LIBS) -I$(DIR_INC)
DUDEFLAGS=-v -v -F -p $(PARTNO) -cusbasp -F -C $(DUDECONF)

# Get numbers only
# Apply conversion factor (converts lsb to millivolts)
# Readings every 2 seconds.  Convert to amp-hours: /(30*60)
%.txt:%.raw
	sed '/^\[EOF/,$$d' <$< | \
	awk 'BEGIN { sec = total = 0 }; { cur = $$1 * 0.0049 }; \
	{ total = total + cur/(30*60) }; { print sec += 2, cur, total }' > $@

showvars:
	echo $(AVRDUDE) $(DUDEFLAGS)
%.list: %.c
	$(CC) -c $(CFLAGS) -E $< |less
%.o:%.c
	$(CC) -c $(CFLAGS) $*.c -o $@
%.elf:%.o
	$(CC) $(CFLAGS) $*.o -o $@
	$(SIZE) $@
%.hex: %.elf
	$(OBJCOPY) -j .text -j .data -O ihex $< $@
%.bin: %.hex
	$(OBJCOPY) -I ihex -O binary $< $@
%.program:%.hex %.eeprom.hex
	$(AVRDUDE) $(DUDEFLAGS) -U flash:w:$*.hex -U eeprom:w:$*.eeprom.hex
%.eeprom.hex:%.elf
	$(OBJCOPY) -j .eeprom -O ihex $< $@

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
	$(AVRDUDE) $(DUDEFLAGS) -U hfuse:w:$*.hfuse:r -U lfuse:w:$*.lfuse:r -U efuse:w:$*.efuse:r

%.eeprom.dump:
	$(AVRDUDE) $(DUDEFLAGS) -U eeprom:r:$@:r

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
