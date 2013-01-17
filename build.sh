avr-gcc -Os -o ibe.elf main.c -mmcu=atmega644p -Wl,-u,vfprintf -lprintf_flt -lm -Wl,-u,vfscanf -lscanf_flt
avr-objcopy -j .text -j .data -O ihex ibe.elf ibe.hex
