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
#include <string.h>
#include <stdio.h>

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
char           text_buffer[16 * 4]; // Stores the text that will be displayed on the LCD
char           buffer64[64]; // Temp buffer for reading UART data

void USART_init(void);
unsigned char USART_receive(void);
unsigned char USART_available(void);
void USART_send(unsigned char data);
void USART_puts(char *str);

void LCD_On();
void SetStartingLine(unsigned char line);
void SetPage(unsigned char page);
void SetAddress(unsigned char addr);
void WriteData(unsigned char data);
void LCD_Clear();
void LCD_PixelOn(unsigned char x, unsigned char y);
void LCD_WriteBuffer();
void LCD_DrawCharAt(unsigned char x, unsigned char y, char c);
void LCD_WriteTextBuffer();
void LCD_WriteMessage(char *error);

void SendCommand(char *cmd);
float ReadThrottle();
float ReadVoltage();
signed int ReadCoolantTemp();
unsigned int ReadEngineSpeed();
unsigned int ReadVelocity();

long map(long x, long in_min, long in_max, long out_min, long out_max);

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
		USART_send(*str); // Send the character
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

void LCD_PixelOff(unsigned char x, unsigned char y) {
	unsigned char off = y % 8; // This calculates the page that the pixel is on.

	y = y / 8; // This then gives you the pixel in that page
	x = x & 0x7F; // Chop off unecessary bits off of the X value

	lcd_buffer[y][x] &= ~(1 << off); // Write it to the buffer
}

// Return 0 if unset, 1 if set
char LCD_GetPixel(unsigned char x, unsigned char y) {
	unsigned char off = y % 8; // This calculates the page that the pixel is on.

	y = y / 8; // This then gives you the pixel in that page
	x = x & 0x7F; // Chop off unecessary bits off of the X value

	return (lcd_buffer[y][x] & (1 << off)) != 0; // Write it to the buffer
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
			} else {
				LCD_PixelOff(x + j, y + i); // If it's off, turn the pixel off
			}
		}
	}
}

void LCD_WriteText(unsigned char x, unsigned char y, char *s) {
	while (*s) {
		LCD_DrawCharAt(x, y, *s);
		s++;
		x += 8;
	}
}

void LCD_WriteTextBuffer() {
	int x, y;
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 16; x++) {
			LCD_DrawCharAt(x * 8, y * 16, text_buffer[(y * 16) + x]);
		}
	}
}

void LCD_WriteMessage(char *error) {
	int i = 0;
	while (*error) {
		text_buffer[i] = *error;
		error++;
	}
	
	LCD_WriteTextBuffer();
}

void LCD_XORBarGraph(unsigned char x, unsigned char y, unsigned char w, unsigned char h, unsigned char v) {
	v = map(v, 0, 100, 0, w);
	
	unsigned char i, j;
	
	for (i = 0; i < w; i++) {
		for (j = 0; j < h; j++) {
			if (i < v) {
				if (LCD_GetPixel(x + i, j + y)) {
					LCD_PixelOff(x + i, j + y);
				} else {
					LCD_PixelOn(x + i, j + y);
				}
			} else {
				LCD_PixelOn(x + i, y);
				LCD_PixelOn(x + i, y + h - 1);
			}
		}
	}
	
	i--;
	j = 0;
	
	for (j = 0; j < h; j++) {
		LCD_PixelOn(x + i, y + j);
	}
}

long map(long x, long in_min, long in_max, long out_min, long out_max) {
	return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/*
	============================================================
	OBD-II UART subroutines
*/


void SendCommand(char *cmd) {
	USART_puts(cmd);
	USART_send(0x0D);
}

float ReadThrottle() {
	SendCommand("0111");
	
	char buffer[16];
	
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
		
		buffer[i] = c;
		i++;
	}
	
	USART_receive();
	
	unsigned int b1;
	unsigned char null;
	
	sscanf(buffer, "%x %x %x", &null, &null, &b1);
	
	return (float)b1 * (100.0f / 255.0f);
}

float ReadVoltage() {
	SendCommand("atrv");
	
	char buffer[16];
	
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
		
		if (c == 'V')
			c = 0;
		
		buffer[i] = c;
		i++;
	}
	
	USART_receive();
	
	float b1;
	
	sscanf(buffer, "%f", &b1);
	
	return b1;
}

signed int ReadCoolantTemp() {
	SendCommand("0105");
	
	char buffer[16];
	
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
		
		buffer[i] = c;
		i++;
	}
	
	USART_receive();
	
	unsigned int b1;
	unsigned char null;
	
	sscanf(buffer, "%x %x %x", &null, &null, &b1);
	
	return b1 - 40;
}

unsigned int ReadEngineSpeed() {
	SendCommand("010C");
	
	char buffer[16];
	
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
		
		buffer[i] = c;
		i++;
	}
	
	USART_receive();
	
	unsigned int b1;
	unsigned int b2;
	unsigned char null;
	
	sscanf(buffer, "%x %x %x %x", &null, &null, &b1, &b2);
	
	return ((b1 * 256) + b2) / 4;//(b2 << 8) | b1;
}

unsigned int ReadVelocity() {
	SendCommand("010D");
	
	char buffer[16];
	
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
		
		buffer[i] = c;
		i++;
	}
	
	USART_receive();
	
	unsigned int b1;
	unsigned char null;
	
	sscanf(buffer, "%x %x %x", &null, &null, &b1);
	
	return b1;
}

/*
	============================================================
	Meat 'n' Potatoes
*/


int main(void) {
	char *cmd_atz   = "ATZ";   // Reset
	char *cmd_atrv  = "atrv";  // Read battery
	char *cmd_atsp0 = "atsp0"; // Autodetect protocol, returns "OK" on success
	
	
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
	
	
	LCD_WriteText(0, 0, "Prototype Boot  ");
	LCD_WriteBuffer();
	
	LCD_WriteText(0, 16, "Starting USART  ");
	LCD_WriteBuffer();
	
	USART_init();
	_delay_ms(2000);
	
	LCD_WriteText(0, 16, "USART Started   ");
	LCD_WriteBuffer();
	
	LCD_WriteText(0, 32, "Sending ATZ     ");
	LCD_WriteBuffer();
	
	SendCommand(cmd_atz);
	_delay_ms(2000);
	
	LCD_WriteText(0, 32, "ATZ Sent        ");
	LCD_WriteBuffer();
	
	_delay_ms(500);
	LCD_WriteText(0, 0, "Boot Successful ");
	LCD_WriteBuffer();
	
	_delay_ms(500);
	LCD_WriteText(0, 16, "Program Start   ");
	LCD_WriteText(0, 32, "                ");
	LCD_WriteBuffer();
	
	while (1) {
		LCD_Clear();
		
		memset(buffer64, 0, 64);

		SetPage(0);
		SetAddress(0x00);
		
		sprintf(buffer64, "%3.0f Rad/Sec", ReadEngineSpeed() / 9.55);
		LCD_WriteText(0, 0, buffer64);
		
		sprintf(buffer64, "%3.0f MPH", (ReadVelocity() * 0.621371));
		LCD_WriteText(0, 16, buffer64);
		
		sprintf(buffer64, "%d C", ReadCoolantTemp());
		LCD_WriteText(96, 16, buffer64);
		
		sprintf(buffer64, "%2.1f Volts", ReadVoltage());
		LCD_WriteText(0, 32, buffer64);
		
		LCD_WriteText(0, 48, "Throttle");
		LCD_XORBarGraph(0, 48, 64, 16, (int)ReadThrottle());
		
		LCD_WriteBuffer();
		
		_delay_ms(500);
	}
}
