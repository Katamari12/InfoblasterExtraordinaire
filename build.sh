avr-gcc -Os -o ibe.elf main.c -mmcu=atmega644p
avr-objcopy -j .text -j .data -O ihex ibe.elf ibe.hex
