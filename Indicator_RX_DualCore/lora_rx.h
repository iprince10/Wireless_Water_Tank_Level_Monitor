#ifndef lora_rx_h
#define lora_rx_h

void lora_rx_init(void);
int wait_aux_high(void);
int uart1_has_data(void);
void set_param_config_rx(void);
void check_config_rx(void);

#endif