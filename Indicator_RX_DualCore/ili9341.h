#ifndef ili9341_h
#define ili9341_h

void ili9341_init();
void spi0_init();
void ili9341_hw_reset();
void spi_write_byte(uint8_t byte);
void ili9341_write_command(uint8_t cmd);
void ili9341_write_data(uint8_t data);
void ili9341_send_init_commands();
void ili9341_fill_white(void);
void ili9341_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg_color, uint16_t bg_color, uint8_t scale);
void ili9341_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg_color, uint16_t bg_color, uint8_t scale);

void ili9341_fill_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void format_distance(char *buff, uint32_t distance);
void display_init_log(void);
void display_add_line(const char *str);
void display_redraw_all(void);

extern char screen_lines[8][32]; // 8 reading slots , 32 chars each
extern uint8_t line_count;   // slots filled

void draw_tank_border(void);
void update_tank_gauge(uint8_t percent);
void draw_tx_offline(void);

#endif

