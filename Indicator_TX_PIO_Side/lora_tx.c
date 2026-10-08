#include <stdint.h>
#include <timer.h>
#include <uart.h>

#define IO_BANK0_BASE 0x40014000u
#define GPIO6_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x034)) //M0
#define GPIO7_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x03c)) //M1
#define GPIO8_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x044)) //AUX

#define SIO_BASE 0xd0000000u
#define SIO_FUNC 5u
#define SIO_GPIO_IN (*(volatile uint32_t *)(SIO_BASE + 0x004))
#define SIO_GPIO_OE (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OE_SET (*(volatile uint32_t *)(SIO_BASE + 0x024))
#define SIO_GPIO_OE_CLEAR (*(volatile uint32_t *)(SIO_BASE + 0x028))
#define SIO_GPIO_OUT (*(volatile uint32_t *)(SIO_BASE + 0x010))
#define SIO_GPIO_OUT_SET (*(volatile uint32_t *)(SIO_BASE + 0x014))
#define SIO_GPIO_OUT_CLEAR (*(volatile uint32_t *)(SIO_BASE + 0x018))

#define PAD_BANK0_BASE 0x4001c000u
#define PAD_GPIO8_CTRL (*(volatile uint32_t *)(PAD_BANK0_BASE + 0x24))
#define PAD_GPIO_CTRL_IE (1u << 6)

void lora_tx_init(void);
int wait_aux_high(void);
void set_param_config_tx(void);
void check_config_rx(void);

void lora_tx_init(void)
{
    GPIO6_CTRL = SIO_FUNC;
    GPIO7_CTRL = SIO_FUNC;
    GPIO8_CTRL = SIO_FUNC;

    SIO_GPIO_OE_SET = (1u << 6) | (1u << 7);
    SIO_GPIO_OUT_CLEAR = (1u << 6) | (1u << 7);

    SIO_GPIO_OE_CLEAR = (1u << 8); // input enable for gpio 8
    PAD_GPIO8_CTRL |= PAD_GPIO_CTRL_IE;
}

int wait_aux_high(void)
{
    if (SIO_GPIO_IN & (1 << 8))
    {
        return 1;
    }
    return 0;
}

void set_param_config_tx(void)
{
    const char hex[] = "0123456789ABCDEF";
    const uint8_t packet[6] = {
        0xC0, // save configuration
        0x00, // ADDH
        0x01, // ADDL
        0x1A, // SPED
        0x17, // CHAN
        0xC4  // OPTION
    };

    // M0 = 1, M1 = 1: configuration mode
    SIO_GPIO_OUT_SET = (1u << 6) | (1u << 7);
    delay_ms(50);

    uart1_write_bytes(packet, sizeof(packet));
    delay_ms(100);

    // C0 returns six acknowledgement bytes. Remove them before reading C1.
    for (int i = 0; i < 6; i++)
    {
        while (!uart1_has_data())
        {
        }
        (void)uart1_getc();
    }

    // Read the configuration that was just saved.
    uart1_putc(0xC1);
    uart1_putc(0xC1);
    uart1_putc(0xC1);
    delay_ms(200);

    uart0_puts("TX CFG: ");
    for (int i = 0; i < 100; i++)
    {
        if (uart1_has_data())
        {
            uint8_t value = (uint8_t)uart1_getc();
            uart0_putc(hex[(value >> 4) & 0x0F]);
            uart0_putc(hex[value & 0x0F]);
            uart0_putc(' ');
        }
        delay_ms(5);
    }
    uart0_puts("\r\n");

    // Back to normal mode
    SIO_GPIO_OUT_CLEAR = (1u << 6) | (1u << 7);
    delay_ms(50);
}

void check_config_tx(void)
{
    SIO_GPIO_OUT_SET = (1u << 6) | (1u << 7);
    delay_ms(50);

    uart1_putc(0xC1);
    uart1_putc(0xC1);
    uart1_putc(0xC1);
    delay_ms(200);

    uart0_puts("TX CFG: ");
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 100; i++)
    {
        if (uart1_has_data())
        {
            uint8_t c = (uint8_t)uart1_getc();
            uart0_putc(hex[(c >> 4) & 0xF]);
            uart0_putc(hex[c & 0xF]);
            uart0_putc(' ');
        }
    }
    uart0_puts("\r\n");

    SIO_GPIO_OUT_CLEAR = (1u << 6) | (1u << 7);
    delay_ms(50);
}
