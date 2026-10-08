#include <stdint.h>
#include <bitmap.h>
#include <timer.h>
#include <uart.h>
#include <ili9341.h>
#include <lora_rx.h>

#define SIO_BASE 0xd0000000u
#define SIO_GPIO_IN (*(volatile uint32_t *)(SIO_BASE + 0x004))
#define SIO_GPIO_OE (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OUT_SET (*(volatile uint32_t *)(SIO_BASE + 0x14))
#define SIO_GPIO_OUT_CLR (*(volatile uint32_t *)(SIO_BASE + 0x18))
#define SIO_GPIO_OUT_XOR (*(volatile uint32_t *)(SIO_BASE + 0x01c)) // toggle LED
#define SIO_GPIO_OE_SET (*(volatile uint32_t *)(SIO_BASE + 0x24))
#define SIO_GPIO_OE_CLR (*(volatile uint32_t *)(SIO_BASE + 0x28))

#define IO_BANK0_BASE 0x40014000u
#define GPIO_FUNC_SIO 5u
#define GPIO25_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x0cc))
#define GPIO9_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x04c))
#define GPIO10_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x054))
#define GPIO25 (1u << 25u) // LED
#define GPIO9 (1u << 9)    // BUZZER
#define GPIO10 (1u << 10)  // switch

#define PAD_BANK0_BASE 0x4001c000u
#define PAD_GPIO10_CTRL (*(volatile uint32_t *)(PAD_BANK0_BASE + 0x2c))
#define PAD_GPIO_CTRL_IE (1u << 6)
#define PAD_GPIO_CTRL_PUE (1u << 3)
#define PAD_GPIO_CTRL_PDE (1u << 2)

#define DEBOUNCE_US 30000UL            // 30 ms debounce window
#define ALERT_TIMEOUT_US 15000000ULL   // 15 s auto-silence
#define OFFLINE_TIMEOUT_US 15000000ULL // 1 minute offline tx side timeout

#define FULL_CM 20
#define EMPTY_CM 150

// the core 1 defines blocks
#define FIFO_ST (*(volatile uint32_t *)(SIO_BASE + 0x050u)) // status
#define FIFO_WR (*(volatile uint32_t *)(SIO_BASE + 0x054u)) // core0 -> core1 write
#define FIFO_RD (*(volatile uint32_t *)(SIO_BASE + 0x058u)) // core1 -> core0 read
#define VTOR_REG 0xE000ED08u                                // vector table register
// FIFO_ST bits
#define FIFO_VLD (1u << 0)          // data waiting in FIFO_RD
#define FIFO_RDY (1u << 1)          // room to write FIFO_WR
#define SEV() __asm volatile("sev") // inline assembly instruction to send event or wake the cpu
#define CORE1_STACK_WORDS 256u
uint32_t core1_stack[CORE1_STACK_WORDS]; // stack for core 1 which is 256 entries deep each entry is 4 bytes
__attribute__((aligned(256)))            // means this array is placed at a memory address that is a multiple of 256 bytes. This is a hard requirement from the ARM architecture for vector tables — the CPU can only look them up if they're 256-byte aligned the lsb should be 00 only
uint32_t core1_vector[48];               // 48 = full RP2040 vector table , the declared length of cortex m0 cores is 48 entries

// Core 1 entry: decode measurement, drive display/switch/buzzer/offline
void core1_main(void)
{
    // LED is handled by core 1, core 0 leaves GPIO25 alone
    GPIO25_CTRL = GPIO_FUNC_SIO; // GPIO25 -> SIO
    SIO_GPIO_OE_SET = GPIO25;    // output enable for LED pin
    SIO_GPIO_OUT_SET = GPIO25;   // LED starts on

    // core1-only state
    uint8_t tank_full = 0;
    uint8_t silenced = 0;
    uint64_t alert_start_time = 0;
    uint64_t last_valid_time = 0;
    uint8_t offline_shown = 0;
    uint8_t last_percent = 255; // impossible value, guarantees first reading always draws

    uint8_t switch_confirmed = 1;   // 1 = not pressed, 0 = pressed
    uint8_t switch_before = 1;      // raw pin reading from previous loop pass
    uint64_t switch_start_time = 0; // timestamp of last raw reading change

    uint64_t next_led_time = 0; // LED heartbeat timer

    while (1)
    {
        uint64_t now = read_timer();

        // ---- LED heartbeat ----
        if (now >= next_led_time)
        {
            SIO_GPIO_OUT_XOR = GPIO25;
            next_led_time = now + 500000ULL;
        }

        // ---- poll the FIFO for a new measurement from core 0 ----
        if (FIFO_ST & FIFO_VLD)
        {
            uint32_t word = FIFO_RD; // reads and clears VLD

            uint8_t status = (uint8_t)(word & 0xFFu);
            uint32_t distance_cm = (word >> 8) & 0xFFFFu;

            uart0_putnum(distance_cm);
            uart0_puts(" cm\r\n");

            // any valid frame from core 0 means the transmitter is alive
            last_valid_time = now;

            if (offline_shown)
            {
                offline_shown = 0;
                last_percent = 255; // force the gauge to fully repaint, covering the offline screen
            }

            if (status == 1)
            {
                tank_full = 0;
                silenced = 0;

                int32_t diff = (int32_t)EMPTY_CM - (int32_t)distance_cm;
                int32_t p = (diff * 100) / (EMPTY_CM - FULL_CM);
                if (p < 0)
                    p = 0;
                if (p > 100)
                    p = 100;
                uint8_t percent_full = (uint8_t)p;

                if (percent_full != last_percent)
                {
                    update_tank_gauge(percent_full);
                    last_percent = percent_full;
                }
            }
            else if (status == 2)
            {
                if (!tank_full)
                {
                    tank_full = 1;
                    alert_start_time = now;
                }

                if (last_percent != 100)
                {
                    update_tank_gauge(100);
                    last_percent = 100;
                }
            }
            else
            {
                // no measurement / invalid — leave gauge and buzzer as they are
            }
        }

        // ---- read the switch ----
        uint8_t switch_now = (SIO_GPIO_IN & GPIO10) ? 1 : 0; // 1 = not pressed

        // debounce: only trust a change once it holds steady for a while
        if (switch_now != switch_before)
        {
            switch_start_time = now;
            switch_before = switch_now;
        }

        if ((now - switch_start_time) >= DEBOUNCE_US && switch_confirmed != switch_now)
        {
            uint8_t old = switch_confirmed;
            switch_confirmed = switch_now;

            // only act on the moment it goes from "not pressed" to "pressed"
            if (old == 1 && switch_confirmed == 0 && tank_full && !silenced)
            {
                silenced = 1; // button silences the current tank-full alert
            }
        }

        // ---- auto-silence if full too long with no button press ----
        if (tank_full && !silenced && (now - alert_start_time) >= ALERT_TIMEOUT_US)
        {
            silenced = 1;
        }

        // ---- drive the buzzer ----
        if (tank_full && !silenced)
        {
            SIO_GPIO_OUT_SET = GPIO9; // GPIO HIGH -> BC547 on -> buzzer ON
        }
        else
        {
            SIO_GPIO_OUT_CLR = GPIO9; // GPIO LOW -> BC547 off -> buzzer OFF
        }

        // ---- offline detection ----
        if (!offline_shown && (now - last_valid_time) >= OFFLINE_TIMEOUT_US)
        {
            draw_tx_offline();
            offline_shown = 1;
        }
    }
}

// 4) Launch core 1
void launch_core1(void)
{
    uint32_t sp = (uint32_t)(core1_stack + CORE1_STACK_WORDS);
    uint32_t entry = (uint32_t)core1_main | 1u; // Thumb bit

    core1_vector[0] = sp;
    core1_vector[1] = entry;

    const uint32_t cmd[] = {
        0u, 0u, 1u,
        (uint32_t)core1_vector, // -> VTOR
        sp,                     // -> MSP
        entry                   // -> PC
    };

    uint32_t seq = 0;

    do
    {
        uint32_t word = cmd[seq];

        if (word == 0u)
        {
            // drain core1's replies before sending a sync 0
            while (FIFO_ST & FIFO_VLD)
            {
                (void)(FIFO_RD);
            }
            SEV();
        }

        // wait until we can write
        while (!(FIFO_ST & FIFO_RDY))
        {
            __asm volatile("nop");
        }
        FIFO_WR = word;
        SEV(); // wake Core 1 after sending command

        // wait until core1 echoes back
        while (!(FIFO_ST & FIFO_VLD))
        {
            __asm volatile("nop");
        }
        uint32_t echo = FIFO_RD;

        if (word == echo)
        {
            seq += 1u;
        }
        else
        {
            seq = 0;
        }

    } while (seq < 6u);
}

void buzzer_switch_led_init(void)
{
    // buzzer block
    GPIO9_CTRL = GPIO_FUNC_SIO; // GPIO9 -> SIO
    SIO_GPIO_OUT_CLR = GPIO9;   // GPIO LOW: BC547 off, buzzer off
    SIO_GPIO_OE_SET = GPIO9;    // now enable output

    // switch block
    GPIO10_CTRL = GPIO_FUNC_SIO;
    SIO_GPIO_OE_CLR = GPIO10;
    PAD_GPIO10_CTRL = (PAD_GPIO10_CTRL & ~PAD_GPIO_CTRL_PDE) | PAD_GPIO_CTRL_IE | PAD_GPIO_CTRL_PUE; // internal pull-up: idle=HIGH, pressed=LOW

    // LED is handled by core 1
}

int main(void)
{
    ili9341_init();
    ili9341_fill_white();
    draw_tank_border();
    display_init_log();
    lora_rx_init();
    delay_ms(1000);
    uart1_init();
    uart0_init();
    // set_param_config_rx();
    check_config_rx();
    buzzer_switch_led_init();

    // Temporary power-on buzzer test.  A BC547 low-side switch is active-HIGH.
    // SIO_GPIO_OUT_SET = GPIO9; // buzzer ON
    // delay_ms(500);
    // SIO_GPIO_OUT_CLR = GPIO9; // buzzer OFF

    launch_core1();

    // parser state persistent variables
    uint8_t frame_index = 0;
    uint8_t frame_buffer[6];

    while (1)
    {
        while (uart1_has_data())
        {
            uint8_t byte = (uint8_t)uart1_getc();

            if (frame_index == 0 && byte == 0xAA)
            {
                frame_buffer[frame_index] = byte;
                frame_index++;
            }
            else if (frame_index >= 1 && frame_index < 6)
            {
                frame_buffer[frame_index] = byte;
                frame_index++;

                if (frame_index == 6)
                {
                    uint8_t checksum = frame_buffer[1] ^ frame_buffer[2] ^ frame_buffer[3];

                    if (checksum == frame_buffer[4] && frame_buffer[5] == 0x55)
                    {
                        uint8_t status = frame_buffer[1];
                        uint32_t distance_cm = (frame_buffer[2] << 8) | frame_buffer[3];

                        // pack into one 32-bit word: status in low byte, distance in next 16 bits
                        uint32_t word = ((uint32_t)distance_cm << 8) | (uint32_t)status;

                        // send the measurement to core 1
                        while (!(FIFO_ST & FIFO_RDY))
                        {
                            __asm volatile("nop");
                        }
                        FIFO_WR = word;
                    }
                    frame_index = 0;
                }
            }
            else
            {
            }
        }
    }
}