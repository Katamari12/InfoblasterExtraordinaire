#define F_CPU 16000000

// The OBD-II UART board runs at 9.6 kBaud
#define BAUD 9600
#define BAUD_PRESCALLER (((F_CPU / (BAUD * 16UL))) - 1)


#include <stdlib.h>
#include <avr/interrupt.h>
#include <util/setbaud.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "font.h"

#define DATA_PORT PORTA
#define CTRL_PORT PORTD
#define CHIP_PORT PORTC

#define ENABLE_CSEL1()  CHIP_PORT |= (1 << 6);
#define DISABLE_CSEL1() CHIP_PORT &= ~(1 << 6);

#define ENABLE_CSEL2()  CHIP_PORT |= (1 << 7);
#define DISABLE_CSEL2() CHIP_PORT &= ~(1 << 7);

#define ENABLE_RS()  CTRL_PORT |= (1 << 4);
#define DISABLE_RS() CTRL_PORT &= ~(1 << 4);

#define ENABLE_RW()  CTRL_PORT |= (1 << 5);
#define DISABLE_RW() CTRL_PORT &= ~(1 << 5);

#define ENABLE_EN()  CTRL_PORT |= (1 << 6);
#define DISABLE_EN() CTRL_PORT &= ~(1 << 6);

#define GET_BIT(ADDRESS,BIT) (ADDRESS & (1<<BIT)) 

unsigned char lcd_buffer[8][128]; // Stores the framebuffer for the LCD
char          text_buffer[16 * 4]; // Stores the text that will be displayed on the LCD

/*
	============================================================
	USART subroutines
*/

void USART_init(void) {
	// Set the baud and mode registers
	UBRR0H = (uint8_t)(BAUD_PRESCALLER>>8);
	UBRR0L = (uint8_t)(BAUD_PRESCALLER);
	UCSR0B = (1<<RXEN0)|(1<<TXEN0);
	UCSR0C = ((1<<UCSZ00)|(1<<UCSZ01));
}

unsigned char USART_receive(void) {
	while(!(UCSR0A & (1<<RXC0))); // Wait until there is a character to be read
	return UDR0; // Read it
}

unsigned char USART_available(void) {
	return (UCSR0A & (1<<RXC0)); // Is there a character available?
}

void USART_send(unsigned char data) {
	while(!(UCSR0A & (1<<UDRE0))); // Wait until it is done sending the previous character
	UDR0 = data; // Send the new one
}

void USART_puts(char *str) {
	while (*str) { // Loop while *str != 0
		USART_send(str[i]); // Send the character
		str++; // Advance the pointer
	}
}


/*
	============================================================
	LCD subroutines
*/



// Enable the LCD driver
void LCD_On() {
	DISABLE_RS();
	DISABLE_RW();
	DATA_PORT = 0b00111111;
	ENABLE_EN();
	_delay_us(4);
	DISABLE_EN();
	_delay_us(4);
}

// Set the first line that it will write to
void SetStartingLine(unsigned char line) {
	line = line & 0b00111111;

	DISABLE_RW();
	DISABLE_RS();
	DATA_PORT = 0b11000000 + line;
	ENABLE_EN();
	_delay_us(4);
	DISABLE_EN();
	_delay_us(4);
}

// Set the page that it will write to.  This is basically the Y value.
// It works by specifying which row will be written to.  Each row is 8 pixels high
void SetPage(unsigned char page) {
	page = page & 0b00000111;

	DISABLE_RW();
	DISABLE_RS();
	DATA_PORT = 0b10111000 + page;
	ENABLE_EN();
	_delay_us(4);
	DISABLE_EN();
	_delay_us(4);
}

// Set the X value of the next data write
void SetAddress(unsigned char addr) {
	addr = addr & 0b00111111;

	DISABLE_RW();
	DISABLE_RS();
	DATA_PORT = 0b01000000 + addr;
	ENABLE_EN();
	_delay_us(4);
	DISABLE_EN();
	_delay_us(4);
}

// Write the actual data
void WriteData(unsigned char data) {
	DISABLE_RW();
	ENABLE_RS();
	DATA_PORT = data;
	ENABLE_EN();
	_delay_us(4);
	DISABLE_EN();
	_delay_us(4);
}

// NULL-out the buffer
void LCD_Clear() {
	unsigned char addr = 0;
	unsigned char page = 0;

	int x, y;
	for (x = 0; x < 8; x++) {
		SetPage(x);
		for (y = 0; y < 128; y++) {
			lcd_buffer[x][y] = 0x00;
		}
	}
}

void LCD_PixelOn(unsigned char x, unsigned char y) {
	unsigned char off = y % 8; // This calculates the page that the pixel is on.

	y = y / 8; // This then gives you the pixel in that page
	x = x & 0x7F; // Chop off unecessary bits off of the X value

	lcd_buffer[y][x] |= (1 << off); // Write it to the buffer
}


// Write the framebuffer to the LCD
void LCD_WriteBuffer() {
	SetPage(0); // Start at (0, 0)
	SetAddress(0);

	int x, y;
	
	ENABLE_CSEL1(); DISABLE_CSEL2(); // Enable the first half of the screen
	for (x = 0; x < 8; x++) { // Write the data from the frame buffer
		SetPage(x);
		for (y = 0; y < 64; y++) {
			WriteData(lcd_buffer[x][y]);
		}
	}
	
	ENABLE_CSEL2(); DISABLE_CSEL1(); // Do the same for the second half
	for (x = 0; x < 8; x++) {
		SetPage(x);
		for (y = 64; y < 128; y++) {
			WriteData(lcd_buffer[x][y]);
		}
	}
}

// Blit a character onto the LCD
void LCD_DrawCharAt(unsigned char x, unsigned char y, char c) {
	// A bit of pointer magic.  Figure out where the character is in the font
	short ch = ((short)c) * 16;
	
	unsigned char i;
	unsigned char j;
	for (i = 0; i < 16; i++) { // The letter is 16 pixels tall
		for (j = 0; j < 8; j++) { // ...and 8 pixels wide
			if (GET_BIT(pgm_read_byte(vgafont16 + ch + i), 8-j)) { // Figure out if the pixel is on or off
				LCD_PixelOn(x + j, y + i); // If it's on, turn the pixel on
			}
		}
	}
}

/*
	============================================================
	OBD-II UART subroutines
*/


void SendCommand(char *cmd) {
	USART_puts(cmd);
	USART_send(0x0D);
}



/*
	============================================================
	Meat 'n' Potatoes
*/


int main(void) {
	char *cmd_atz = "ATZ";
	char *cmd_atrv = "atrv";
	
	DDRA = 0xFF;
	DDRC = 0xFF;
	DDRD = 0xFF;
	
	ENABLE_CSEL1();
	ENABLE_CSEL2();

	LCD_On();

	LCD_Clear();
	
	SetStartingLine(0);
	SetPage(0);
	SetAddress(0x00);
	
	SetPage(0);
	SetAddress(0);
	
	USART_init();
	_delay_ms(500);
	
	
	SendCommand(cmd_atz);
	_delay_ms(2000);
	while (1) {
		LCD_Clear();

		SetPage(0);
		SetAddress(0x00);
		unsigned char x, y;
		
		for (y = 0; y < 4; y++) {
			for (x = 0; x < 16; x++) {
				LCD_DrawCharAt(x * 8, y * 16, text_buffer[(y * 16) + x]);
			}
		}
		
		LCD_WriteBuffer();
		
		SendCommand(cmd_atrv);
		
		int i = 0;
		char c = 0;
		while (1) {
			c = USART_receive();
			if (c == '\r') {
				break;
			}
		}
		
		while (1) {
			c = USART_receive();
			if (c == '\r') {
				break;
			}
			
			text_buffer[i] = c;
			i++;
		}
		
		USART_receive();
		
		_delay_ms(500);
	}
}
