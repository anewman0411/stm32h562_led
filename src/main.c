#include "stm32h5xx.h"
#include "global_constants.h"
#include "setup.h"

/* Simple busy-wait delay (not cycle-accurate, just for blinking) */
void delay(volatile uint32_t count)
{
    while (count--) {
        __NOP();
    }
}

int main(void)
{
    /* Initialize system clock and GPIO */
    setup();

    /* Main loop: blink LED */
    while (1)
    {
        /* Toggle LED pin */
        LED_PORT->ODR ^= (0x1UL << LED_PIN);

        /*
         * Rough delay ~500 ms:
         *   HSI  @ 64 MHz  -> try 1_000_000
         *   PLL1 @ 250 MHz -> try 4_000_000
         * Adjust to taste.
         */
        delay(8000000);
    }

    return 0;
}
