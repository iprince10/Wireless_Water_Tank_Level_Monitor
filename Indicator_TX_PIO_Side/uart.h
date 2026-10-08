#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart0_init(void);
void uart0_putc(char);
void uart0_puts(const char *);
void uart0_putnum(uint64_t num);

void uart1_init(void);
void uart1_putc(char c);
void uart1_puts(const char *send);
void uart1_write_bytes(const uint8_t *data, uint8_t len);
char uart1_getc(void);
int uart1_has_data(void);

#endif
