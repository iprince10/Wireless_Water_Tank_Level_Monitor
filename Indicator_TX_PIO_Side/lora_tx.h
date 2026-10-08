#ifndef lora_tx_h
#define lora_tx_h

void lora_tx_init(void);
int wait_aux_high(void);
void set_param_config_tx(void);
void check_config_tx(void);

#endif