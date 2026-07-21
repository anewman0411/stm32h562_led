This file is a merged representation of a subset of the codebase, containing specifically included files, combined into a single document by Repomix.

# File Summary

## Purpose
This file contains a packed representation of a subset of the repository's contents that is considered the most important context.
It is designed to be easily consumable by AI systems for analysis, code review,
or other automated processes.

## File Format
The content is organized as follows:
1. This summary section
2. Repository information
3. Directory structure
4. Repository files (if enabled)
5. Multiple file entries, each consisting of:
  a. A header with the file path (## File: path/to/file)
  b. The full contents of the file in a code block

## Usage Guidelines
- This file should be treated as read-only. Any changes should be made to the
  original repository files, not this packed version.
- When processing this file, use the file path to distinguish
  between different files in the repository.
- Be aware that this file may contain sensitive information. Handle it with
  the same level of security as you would the original repository.

## Notes
- Some files may have been excluded based on .gitignore rules and Repomix's configuration
- Binary files are not included in this packed representation. Please refer to the Repository Structure section for a complete list of file paths, including binary files
- Only files matching these patterns are included: main.c, global_constants.h, setup.c, setup.h
- Files matching patterns in .gitignore are excluded
- Files matching default ignore patterns are excluded
- Files are sorted by Git change count (files with more changes are at the bottom)

# Directory Structure
```
global_constants.h
main.c
setup.c
setup.h
```

# Files

## File: global_constants.h
```c
#ifndef GLOBAL_CONSTANTS_H_
#define GLOBAL_CONSTANTS_H_

// ------------------------------------------------------------------
// LED pin configuration - WeAct Studio STM32H562RGT6 v1.1
// LED is on PB2 (active-low: pin LOW = LED on)
// ------------------------------------------------------------------

#define LED_PORT            GPIOB
#define LED_PORT_CLK_EN     RCC_AHB2ENR_GPIOBEN
#define LED_PIN             2

// ------------------------------------------------------------------
// Select the external crystal frequency used on your board
// WeAct Studio boards typically use 8 MHz HSE
// ------------------------------------------------------------------

//#define HSE_VALUE    25
#define HSE_VALUE    8

// ------------------------------------------------------------------

#endif /* GLOBAL_CONSTANTS_H_ */
```

## File: main.c
```c
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
```

## File: setup.c
```c
#include "stm32h5xx.h"
#include "global_constants.h"
#include "setup.h"

/*
 * STM32H562RGT6 Bare-Metal Blink Setup
 *
 * Default configuration: HSI at 64 MHz (no external crystal needed).
 * To use HSE + PLL1 at 250 MHz, uncomment the #define below.
 */

// Uncomment to enable HSE + PLL1 at 250 MHz instead of HSI 64 MHz
// #define USE_HSE_PLL

static void setup_clock(void);
static void setup_GPIO_LED(void);

#ifdef USE_HSE_PLL
static void setup_HSE(void);
static void setup_PLL1(void);
#endif

void setup(void)
{
    setup_clock();
    setup_GPIO_LED();
}

// ---------------------------------------------------------------
// Clock Configuration
// ---------------------------------------------------------------

static void setup_clock(void)
{
#ifdef USE_HSE_PLL
    setup_HSE();
    setup_PLL1();
#else
    /*
     * HSI is already running at 64 MHz after reset.
     * Flash latency and voltage scaling are fine at defaults.
     * Nothing to do here for the simple case.
     */
    (void)0;
#endif
}

#ifdef USE_HSE_PLL

static void setup_HSE(void)
{
    /* Ensure HSI is the system clock before touching PLL */
    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (0x00 << RCC_CFGR1_SW_Pos); /* SW = HSI */
    while ((RCC->CFGR1 & RCC_CFGR1_SWS) != (0x00 << RCC_CFGR1_SWS_Pos)) {
        ; /* wait for HSI as SYSCLK */
    }

    /* Enable HSE */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        ; /* wait for HSE ready */
    }

    /* Enable power interface clock and set voltage scaling to VOS0
     * (required for 250 MHz operation) */
    RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;

    /* Set VOS0 (highest performance): set VOSRDY, then VOS bits */
    PWR->VOSCR = (PWR->VOSCR & ~PWR_VOSCR_VOS) | (0x3 << PWR_VOSCR_VOS_Pos);
    while (!(PWR->VOSSR & PWR_VOSSR_VOSRDY)) {
        ; /* wait for voltage scaling ready */
    }

    /* Set bus prescalers for 250 MHz SYSCLK:
     *   AHB  = SYSCLK / 1 = 250 MHz
     *   APB1 = SYSCLK / 2 = 125 MHz (max 250 MHz, but /2 is safer for peripherals)
     *   APB2 = SYSCLK / 2 = 125 MHz
     *   APB3 = SYSCLK / 2 = 125 MHz
     */
    RCC->CFGR2 = 0; /* AHB prescaler = 1 (default) */
    RCC->CFGR2 |= (0x4 << RCC_CFGR2_PPRE1_Pos);  /* APB1 /2 */
    RCC->CFGR2 |= (0x4 << RCC_CFGR2_PPRE2_Pos);  /* APB2 /2 */
    RCC->CFGR2 |= (0x4 << RCC_CFGR2_PPRE3_Pos);  /* APB3 /2 */
}

static void setup_PLL1(void)
{
    /*
     * PLL1 Configuration for SYSCLK = 250 MHz
     *
     * PLL1R = HSE / PLLM * PLLN / PLLR
     *
     * For 25 MHz HSE:
     *   PLLM=5, PLLN=100, PLLR=2  ->  25/5*100/2 = 250 MHz
     *
     * For 8 MHz HSE:
     *   PLLM=2, PLLN=125, PLLR=2  ->   8/2*125/2 = 250 MHz
     *
     * VCO input  must be 1-16 MHz
     * VCO output must be 128-560 MHz
     */

    /* Disable PLL1 */
    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) {
        ; /* wait for PLL1 off */
    }

    /* Set flash latency for 250 MHz at VOS0: 5 wait states */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_5WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_5WS) {
        ; /* wait */
    }

    /* Configure PLL1 source and input divider */
#if HSE_VALUE == 25
    RCC->PLL1CFGR = (RCC->PLL1CFGR & ~(RCC_PLL1CFGR_PLL1SRC | RCC_PLL1CFGR_PLL1M))
                   | (0x3 << RCC_PLL1CFGR_PLL1SRC_Pos)   /* HSE as PLL1 source */
                   | (5   << RCC_PLL1CFGR_PLL1M_Pos);     /* PLLM = 5 -> 5 MHz VCO in */
#elif HSE_VALUE == 8
    RCC->PLL1CFGR = (RCC->PLL1CFGR & ~(RCC_PLL1CFGR_PLL1SRC | RCC_PLL1CFGR_PLL1M))
                   | (0x3 << RCC_PLL1CFGR_PLL1SRC_Pos)   /* HSE as PLL1 source */
                   | (2   << RCC_PLL1CFGR_PLL1M_Pos);     /* PLLM = 2 -> 4 MHz VCO in */
#else
    #error "Unsupported HSE_VALUE. Define 8 or 25 in global_constants.h"
#endif

    /* Set VCO input range: 0 = 1-2 MHz, 1 = 2-4 MHz, 2 = 4-8 MHz, 3 = 8-16 MHz */
#if HSE_VALUE == 25
    RCC->PLL1CFGR = (RCC->PLL1CFGR & ~RCC_PLL1CFGR_PLL1RGE) | (2 << RCC_PLL1CFGR_PLL1RGE_Pos);
#elif HSE_VALUE == 8
    RCC->PLL1CFGR = (RCC->PLL1CFGR & ~RCC_PLL1CFGR_PLL1RGE) | (1 << RCC_PLL1CFGR_PLL1RGE_Pos);
#endif

    /* Enable wide VCO range (VCOSEL=0 for wide 128-560 MHz) */
    RCC->PLL1CFGR &= ~RCC_PLL1CFGR_PLL1VCOSEL;

    /* Enable PLL1R output (used as SYSCLK) */
    RCC->PLL1CFGR |= RCC_PLL1CFGR_PLL1REN;

    /* Disable fractional mode */
    RCC->PLL1CFGR &= ~RCC_PLL1CFGR_PLL1FRACEN;

    /* Configure PLL1 multiplier and dividers
     * PLL1DIVR: DIVN = N-1, DIVP = P-1, DIVQ = Q-1, DIVR = R-1
     */
#if HSE_VALUE == 25
    RCC->PLL1DIVR = ((100 - 1) << RCC_PLL1DIVR_PLL1N_Pos)   /* PLLN = 100 -> VCO = 500 MHz */
                   | ((2 - 1)   << RCC_PLL1DIVR_PLL1R_Pos);  /* PLLR = 2   -> 250 MHz */
#elif HSE_VALUE == 8
    RCC->PLL1DIVR = ((125 - 1) << RCC_PLL1DIVR_PLL1N_Pos)   /* PLLN = 125 -> VCO = 500 MHz */
                   | ((2 - 1)   << RCC_PLL1DIVR_PLL1R_Pos);  /* PLLR = 2   -> 250 MHz */
#endif

    /* Enable PLL1 */
    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) {
        ; /* wait for PLL1 lock */
    }

    /* Switch system clock to PLL1R */
    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (0x3 << RCC_CFGR1_SW_Pos);
    while ((RCC->CFGR1 & RCC_CFGR1_SWS) != (0x3 << RCC_CFGR1_SWS_Pos)) {
        ; /* wait for PLL1 as SYSCLK */
    }
}

#endif /* USE_HSE_PLL */

// ---------------------------------------------------------------
// GPIO Configuration
// ---------------------------------------------------------------

static void setup_GPIO_LED(void)
{
    /* Enable GPIO port clock (AHB2 bus) */
    RCC->AHB2ENR |= LED_PORT_CLK_EN;

    /* Brief delay after enabling clock (2 dummy reads) */
    volatile uint32_t dummy;
    dummy = RCC->AHB2ENR;
    dummy = RCC->AHB2ENR;
    (void)dummy;

    /* Configure LED pin as general-purpose output (mode 01) */
    LED_PORT->MODER &= ~(0x3UL << (LED_PIN * 2));      /* Clear mode bits */
    LED_PORT->MODER |=  (0x1UL << (LED_PIN * 2));      /* Set as output   */

    /* Push-pull output type */
    LED_PORT->OTYPER &= ~(0x1UL << LED_PIN);

    /* High speed */
    LED_PORT->OSPEEDR |= (0x3UL << (LED_PIN * 2));

    /* No pull-up / pull-down */
    LED_PORT->PUPDR &= ~(0x3UL << (LED_PIN * 2));

    /* Start with LED off */
    LED_PORT->ODR &= ~(0x1UL << LED_PIN);
}
```

## File: setup.h
```c
#ifndef SETUP_H_
#define SETUP_H_

void setup(void);

#endif /* SETUP_H_ */
```
