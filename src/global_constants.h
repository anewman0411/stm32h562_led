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