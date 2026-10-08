#ifndef AJSR04T_H
#define AJSR04T_H

#include <stdint.h>

void ajsr04t_pio_init(void);
void send_timeout_write_fifo(void);
int ajsr04t_result_ready(void);
uint32_t read_result(void);


#endif
