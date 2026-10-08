#include <stdint.h>
#include "pico/stdlib.h"
#include "ajsr04t.pio.h"

#define GPIO2_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x014)) // trigger
#define GPIO3_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x01c)) // echo

#define PADS_GPIO3 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x04u + (3u * 4u)))  // -> 0x4001c010
#define PAD_IE      (1u << 6)  // input enable
#define PAD_PDE     (1u << 2)  // pull-down enable
#define PAD_SCHMITT (1u << 1)  // schmitt trigger

#define PIO_FUNC 6u

#define CTRL (*(volatile uint32_t *)(PIO0_BASE + 0x000))
#define CTRL_SM_ENABLE (1u << 0)

#define PIO_TXF0 (*(volatile uint32_t *)(PIO0_BASE + 0x010))  //tx fifo pio
#define PIO_RXF0 (*(volatile uint32_t *)(PIO0_BASE + 0x020))  // rx fifo pio
#define PIO_FSTAT (*(volatile uint32_t *)(PIO0_BASE + 0x004))  // status register of fifo
#define PIO_FSTAT_RXEMPTY_SM0 (1u << 8)

#define SM0_EXECCTRL (*(volatile uint32_t *)(PIO0_BASE + 0x0cc))  // state machine execution control
#define EXECCTRL_JMP_PIN (3u << 24)    // gpio 3 echo pin   // pin to execute high and low
#define EXECCTRL_WRAP_TOP (17u << 12)  // top wrap          // address to wrap to
#define EXECCTRL_WRAP_BOTTOM (1u << 7) // bottom wrap       // address to wrap from

#define SM0_PINCTRL (*(volatile uint32_t *)(PIO0_BASE + 0x0dc))   // state machine 0 pin control
#define PINCTRL_SET_COUNT (1u << 26) // single pin , set count i.e. control single pin
#define PINCTRL_SET_BASE (2u << 5)   // 1 base, 2 means gpio 2 , the 5 is the bit field of the register 

#define SM0_CLKDIV (*(volatile uint32_t *)(PIO0_BASE + 0x0c8))       // set clock for state machines
// 125 MHz / 62.5 = 2 MHz. The echo-count loop consumes two PIO cycles,
// therefore each decrement represents one microsecond.
#define CLKDIV_INT ((62u) << 16)
#define CLKDIV_FRAC ((128u) << 8)

#define INSTR_MEM(i) (*(volatile uint32_t *)(PIO0_BASE + 0x048 + ((i) * 4)))   // instruction memory to store the instructions too

void ajsr04t_pio_init(void)
{
    // Load PIO program
    for (int i = 0; i < ajsr04t_program.length; i++)
    {
        INSTR_MEM(i) = ajsr04t_program_instructions[i];  // sdk generated instruction set , consists of asm pio instruction in hex form
    }

    SM0_EXECCTRL = EXECCTRL_JMP_PIN | EXECCTRL_WRAP_TOP | EXECCTRL_WRAP_BOTTOM;   //state machine 0 init
    SM0_PINCTRL = PINCTRL_SET_COUNT | PINCTRL_SET_BASE;   // set pin 3
    SM0_CLKDIV = CLKDIV_INT | CLKDIV_FRAC;   // clock div the sm 0 has clock freq of 62.5 Mhz i.e. 1us per 2 cycles

    GPIO2_CTRL = PIO_FUNC;
    GPIO3_CTRL = PIO_FUNC;

    PADS_GPIO3 |= PAD_IE | PAD_PDE | PAD_SCHMITT;
    CTRL |= CTRL_SM_ENABLE; // state machine enable
}

void send_timeout_write_fifo(void)
{
    PIO_TXF0 = 40000;  // inital value in scratch x register to decrement from and count
}

int ajsr04t_result_ready(void)
{
    return (PIO_FSTAT & PIO_FSTAT_RXEMPTY_SM0) == 0;
    //returns 0 when not ready and 1 when ready , the rx if empty contains 1 else 0
}

uint32_t read_result(void)
{
    if (!ajsr04t_result_ready())
    {
        return 0;
    }

    uint32_t remaining = PIO_RXF0;

    // A zero countdown value is the PIO program's no-echo timeout marker.
    if (remaining == 0)
    {
        return 0;
    }

    uint32_t duration_us = 40000 - remaining;
    return duration_us;
}
