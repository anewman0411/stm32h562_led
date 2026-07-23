#include "stm32h5xx.h"
#include "usb_cdc.h"
#include "bootloader.h"


/* ------------------------------------------------------------------ */
/* Packet memory layout                                                */
/* ------------------------------------------------------------------ */
/*
 * The USB_DRD peripheral keeps its buffer descriptor table at the very start
 * of the 2 KB PMA (there is no BTABLE register on this IP), 8 bytes per
 * endpoint. Everything below is a byte offset into the PMA and is kept 4-byte
 * aligned so the word-based copy helpers stay simple.
 */
#define PMA_BTABLE_BYTES    (8U * 8U)   /* 8 endpoints * (TXBD + RXBD) */

#define PMA_EP0_TX          0x0040U
#define PMA_EP0_RX          0x0080U
#define PMA_EP1_TX          0x00C0U     /* CDC notification (unused traffic) */
#define PMA_EP2_TX          0x0100U
#define PMA_EP2_RX          0x0140U

#define EP0_MAX_PACKET      64U
#define EP_NOTIFY_SIZE      8U
#define EP_DATA_MAX_PACKET  64U

#define EP_CTRL             0U
#define EP_NOTIFY           1U
#define EP_DATA             2U

/* ------------------------------------------------------------------ */
/* Endpoint register helpers                                           */
/* ------------------------------------------------------------------ */
/*
 * CHEPnR is a minefield: VTRX/VTTX are rc_w0 (write 1 to keep), while
 * STAT_RX/STAT_TX/DTOG_RX/DTOG_TX are toggle-on-write-1. So every update must
 * mask off the toggle bits (writing 0 leaves them alone) and write 1 back into
 * the VT bits it does not intend to clear.
 */
#define EPR(n)              (*((volatile uint32_t *)&USB_DRD_FS->CHEP0R + (n)))

#define EPR_EA_MASK         0x0FUL
#define EPR_NOTOG_MASK      (EPR_EA_MASK | USB_CHEP_KIND | USB_CHEP_UTYPE)

static inline void ep_set_stat_tx(uint32_t n, uint32_t stat)
{
    uint32_t r = EPR(n) & (EPR_NOTOG_MASK | USB_CHEP_TX_STTX);
    EPR(n) = (r ^ stat) | USB_CHEP_VTRX | USB_CHEP_VTTX;
}

static inline void ep_set_stat_rx(uint32_t n, uint32_t stat)
{
    uint32_t r = EPR(n) & (EPR_NOTOG_MASK | USB_CHEP_RX_STRX);
    EPR(n) = (r ^ stat) | USB_CHEP_VTRX | USB_CHEP_VTTX;
}

static inline void ep_clear_vtrx(uint32_t n)
{
    EPR(n) = (EPR(n) & EPR_NOTOG_MASK) | USB_CHEP_VTTX;
}

static inline void ep_clear_vttx(uint32_t n)
{
    EPR(n) = (EPR(n) & EPR_NOTOG_MASK) | USB_CHEP_VTRX;
}

/* ------------------------------------------------------------------ */
/* PMA access                                                          */
/* ------------------------------------------------------------------ */

static void pma_write(uint16_t off, const uint8_t *src, uint16_t len)
{
    volatile uint32_t *dst = (volatile uint32_t *)(USB_DRD_PMAADDR + off);
    uint16_t i = 0;

    while ((uint16_t)(i + 4U) <= len) {
        dst[i >> 2] = (uint32_t)src[i]
                    | ((uint32_t)src[i + 1] << 8)
                    | ((uint32_t)src[i + 2] << 16)
                    | ((uint32_t)src[i + 3] << 24);
        i = (uint16_t)(i + 4U);
    }

    if (i < len) {
        /* Tail bytes. The padding written past len is never transmitted,
         * because COUNT_TX bounds the packet. */
        uint32_t w = 0;
        for (uint16_t b = 0; (uint16_t)(i + b) < len; b++) {
            w |= (uint32_t)src[i + b] << (8U * b);
        }
        dst[i >> 2] = w;
    }
}

static void pma_read(uint16_t off, uint8_t *dst, uint16_t len)
{
    const volatile uint32_t *src = (const volatile uint32_t *)(USB_DRD_PMAADDR + off);

    for (uint16_t i = 0; i < len; i++) {
        dst[i] = (uint8_t)(src[i >> 2] >> (8U * (i & 3U)));
    }
}

/* RX buffer size field of RXBD: BLSIZE (bit 31) + NUM_BLOCK (bits 30:26). */
static uint32_t pma_rx_blocks(uint16_t size)
{
    if (size >= 32U) {
        return 0x80000000UL | ((uint32_t)((size / 32U) - 1U) << 26);
    }
    return (uint32_t)((size + 1U) / 2U) << 26;
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

static const uint8_t device_descriptor[18] = {
    18, 0x01,                   /* bLength, DEVICE                     */
    0x00, 0x02,                 /* bcdUSB 2.00                         */
    0x02, 0x00, 0x00,           /* class CDC, subclass 0, protocol 0   */
    EP0_MAX_PACKET,
    0x83, 0x04,                 /* idVendor  0x0483 (ST)               */
    0x40, 0x57,                 /* idProduct 0x5740 (stock VCP)        */
    0x00, 0x02,                 /* bcdDevice 2.00                      */
    1, 2, 3,                    /* iManufacturer, iProduct, iSerial    */
    1,                          /* bNumConfigurations                  */
};

#define CONFIG_DESC_LEN 67

static const uint8_t config_descriptor[CONFIG_DESC_LEN] = {
    /* Configuration */
    9, 0x02, CONFIG_DESC_LEN, 0x00, 2, 1, 0, 0x80, 50,

    /* Interface 0: CDC communications */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,

    /* CDC header functional */
    5, 0x24, 0x00, 0x10, 0x01,
    /* CDC call management: no call management, data on interface 1 */
    5, 0x24, 0x01, 0x00, 1,
    /* CDC ACM functional: supports Set/Get_Line_Coding + Set_Control_Line_State */
    4, 0x24, 0x02, 0x02,
    /* CDC union: control interface 0, subordinate interface 1 */
    5, 0x24, 0x06, 0, 1,

    /* Notification endpoint 0x81, interrupt, 8 bytes, 16 ms */
    7, 0x05, 0x81, 0x03, EP_NOTIFY_SIZE, 0x00, 0x10,

    /* Interface 1: CDC data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,

    /* Bulk OUT 0x02 */
    7, 0x05, 0x02, 0x02, EP_DATA_MAX_PACKET, 0x00, 0x00,
    /* Bulk IN 0x82 */
    7, 0x05, 0x82, 0x02, EP_DATA_MAX_PACKET, 0x00, 0x00,
};

static const uint8_t string_langid[4]  = { 4, 0x03, 0x09, 0x04 };
static const uint8_t string_vendor[]   = {
    20, 0x03, 'S',0, 'T',0, 'M',0, '3',0, '2',0, 'H',0, '5',0, '6',0, '2',0
};
static const uint8_t string_product[]  = {
    26, 0x03, 'H',0, '5',0, '6',0, '2',0, ' ',0, 'D',0, 'F',0, 'U',0,
    ' ',0, 'V',0, 'C',0, 'P',0
};
static const uint8_t string_serial[]   = {
    10, 0x03, '0',0, '0',0, '0',0, '1',0
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} setup_packet_t;

static const uint8_t *ctrl_tx_ptr;
static uint16_t       ctrl_tx_remaining;
static uint8_t        pending_address;
static uint8_t        pending_out_request;

/* CDC line coding: 115200 8N1 by default. */
static uint8_t  line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
static volatile bool bootloader_request;

static uint8_t ep_data_out_buf[EP_DATA_MAX_PACKET];

bool usb_cdc_bootloader_requested(void)
{
    return bootloader_request;
}

/* ------------------------------------------------------------------ */
/* Control transfers                                                   */
/* ------------------------------------------------------------------ */

static void ep0_stall(void)
{
    ep_set_stat_tx(EP_CTRL, USB_EP_TX_STALL);
    ep_set_stat_rx(EP_CTRL, USB_EP_RX_STALL);
}

static void ep0_send_chunk(void)
{
    uint16_t n = ctrl_tx_remaining;

    if (n > EP0_MAX_PACKET) {
        n = EP0_MAX_PACKET;
    }

    pma_write(PMA_EP0_TX, ctrl_tx_ptr, n);
    USB_DRD_PMA_BUFF[EP_CTRL].TXBD = ((uint32_t)n << 16) | PMA_EP0_TX;

    ctrl_tx_ptr       += n;
    ctrl_tx_remaining = (uint16_t)(ctrl_tx_remaining - n);

    ep_set_stat_tx(EP_CTRL, USB_EP_TX_VALID);
}

static void ep0_send(const uint8_t *data, uint16_t len, uint16_t requested)
{
    ctrl_tx_ptr       = data;
    ctrl_tx_remaining = (len < requested) ? len : requested;
    ep0_send_chunk();
}

static void ep0_send_status(void)
{
    USB_DRD_PMA_BUFF[EP_CTRL].TXBD = PMA_EP0_TX;    /* zero-length packet */
    ctrl_tx_ptr       = NULL;
    ctrl_tx_remaining = 0;
    ep_set_stat_tx(EP_CTRL, USB_EP_TX_VALID);
}

static void configure_data_endpoints(void)
{
    /* Notification endpoint (interrupt IN). Nothing is ever sent on it, but
     * the host expects it to exist and to NAK politely. */
    EPR(EP_NOTIFY) = EP_NOTIFY | USB_EP_INTERRUPT;
    USB_DRD_PMA_BUFF[EP_NOTIFY].TXBD = PMA_EP1_TX;
    ep_set_stat_tx(EP_NOTIFY, USB_EP_TX_NAK);
    ep_set_stat_rx(EP_NOTIFY, USB_EP_RX_DIS);

    /* Bulk data endpoint pair. */
    EPR(EP_DATA) = EP_DATA | USB_EP_BULK;
    USB_DRD_PMA_BUFF[EP_DATA].TXBD = PMA_EP2_TX;
    USB_DRD_PMA_BUFF[EP_DATA].RXBD = pma_rx_blocks(EP_DATA_MAX_PACKET) | PMA_EP2_RX;
    ep_set_stat_tx(EP_DATA, USB_EP_TX_NAK);
    ep_set_stat_rx(EP_DATA, USB_EP_RX_VALID);
}

static void handle_standard_request(const setup_packet_t *s)
{
    switch (s->bRequest) {
    case 0x06: { /* GET_DESCRIPTOR */
        const uint8_t type  = (uint8_t)(s->wValue >> 8);
        const uint8_t index = (uint8_t)(s->wValue & 0xFF);

        if (type == 0x01) {
            ep0_send(device_descriptor, sizeof(device_descriptor), s->wLength);
        } else if (type == 0x02) {
            ep0_send(config_descriptor, sizeof(config_descriptor), s->wLength);
        } else if (type == 0x03) {
            switch (index) {
            case 0:  ep0_send(string_langid,  sizeof(string_langid),  s->wLength); break;
            case 1:  ep0_send(string_vendor,  sizeof(string_vendor),  s->wLength); break;
            case 2:  ep0_send(string_product, sizeof(string_product), s->wLength); break;
            case 3:  ep0_send(string_serial,  sizeof(string_serial),  s->wLength); break;
            default: ep0_stall(); break;
            }
        } else {
            /* Device qualifier and friends: a full-speed-only device must
             * stall these rather than answer them. */
            ep0_stall();
        }
        break;
    }

    case 0x05: /* SET_ADDRESS -- applied only after the status stage */
        pending_address = (uint8_t)(s->wValue & 0x7F);
        ep0_send_status();
        break;

    case 0x09: /* SET_CONFIGURATION */
        if (s->wValue != 0) {
            configure_data_endpoints();
        }
        ep0_send_status();
        break;

    case 0x08: { /* GET_CONFIGURATION */
        static const uint8_t cfg = 1;
        ep0_send(&cfg, 1, s->wLength);
        break;
    }

    case 0x00: { /* GET_STATUS */
        static const uint8_t status[2] = { 0, 0 };
        ep0_send(status, 2, s->wLength);
        break;
    }

    case 0x0A: { /* GET_INTERFACE */
        static const uint8_t alt = 0;
        ep0_send(&alt, 1, s->wLength);
        break;
    }

    case 0x01: /* CLEAR_FEATURE */
    case 0x0B: /* SET_INTERFACE */
        ep0_send_status();
        break;

    default:
        ep0_stall();
        break;
    }
}

static void handle_class_request(const setup_packet_t *s)
{
    switch (s->bRequest) {
    case 0x20: /* SET_LINE_CODING -- 7 bytes arrive in an OUT data stage */
        pending_out_request = 0x20;
        ep_set_stat_rx(EP_CTRL, USB_EP_RX_VALID);
        break;

    case 0x21: /* GET_LINE_CODING */
        ep0_send(line_coding, sizeof(line_coding), s->wLength);
        break;

    case 0x22: { /* SET_CONTROL_LINE_STATE */
        const bool dtr = (s->wValue & 0x0001) != 0;
        const uint32_t baud = (uint32_t)line_coding[0]
                            | ((uint32_t)line_coding[1] << 8)
                            | ((uint32_t)line_coding[2] << 16)
                            | ((uint32_t)line_coding[3] << 24);

        /*
         * The 1200-baud touch. The host opens the port at 1200 baud and then
         * drops DTR; that combination means "reboot to bootloader" and nothing
         * else, so it is safe to treat as a command.
         *
         * The reset itself is deferred to the main loop -- issuing it here
         * would kill the device before this control transfer's status stage
         * completes, and the host would report an I/O error instead of a clean
         * disconnect.
         */
        if (!dtr && baud == 1200U) {
            bootloader_request = true;
        }

        ep0_send_status();
        break;
    }

    case 0x23: /* SEND_BREAK */
        ep0_send_status();
        break;

    default:
        ep0_stall();
        break;
    }
}

static void handle_setup(void)
{
    uint8_t raw[8];
    setup_packet_t s;

    pma_read(PMA_EP0_RX, raw, sizeof(raw));

    s.bmRequestType = raw[0];
    s.bRequest      = raw[1];
    s.wValue        = (uint16_t)(raw[2] | (raw[3] << 8));
    s.wIndex        = (uint16_t)(raw[4] | (raw[5] << 8));
    s.wLength       = (uint16_t)(raw[6] | (raw[7] << 8));

    pending_out_request = 0;

    switch (s.bmRequestType & 0x60) {
    case 0x00: handle_standard_request(&s); break;
    case 0x20: handle_class_request(&s);    break;
    default:   ep0_stall();                 break;
    }
}

static void handle_ep0_out(void)
{
    const uint16_t count = (uint16_t)((USB_DRD_PMA_BUFF[EP_CTRL].RXBD >> 16) & 0x3FF);

    if (pending_out_request == 0x20 && count >= sizeof(line_coding)) {
        pma_read(PMA_EP0_RX, line_coding, sizeof(line_coding));
        pending_out_request = 0;
        ep0_send_status();
    }

    ep_set_stat_rx(EP_CTRL, USB_EP_RX_VALID);
}

static void handle_ep0_in(void)
{
    if (ctrl_tx_remaining > 0) {
        ep0_send_chunk();
        return;
    }

    if (pending_address != 0) {
        /* Only now is it legal to move to the assigned address. */
        USB_DRD_FS->DADDR = (uint32_t)pending_address | USB_DADDR_EF;
        pending_address = 0;
    }

    ep_set_stat_rx(EP_CTRL, USB_EP_RX_VALID);
}

static void handle_data_out(void)
{
    const uint16_t count = (uint16_t)((USB_DRD_PMA_BUFF[EP_DATA].RXBD >> 16) & 0x3FF);
    const uint16_t n = (count > sizeof(ep_data_out_buf)) ? sizeof(ep_data_out_buf) : count;

    pma_read(PMA_EP2_RX, ep_data_out_buf, n);

    /*
     * Data-token trigger, matching the "DFU!" convention already used by the
     * F4 boards' flash_dfu.sh. Matched as a stream so it still works when the
     * token is split across packets. 'R' is kept as a single-key escape hatch
     * for poking the port by hand.
     */
    static const char token[] = "DFU!";
    static uint8_t matched;

    for (uint16_t i = 0; i < n; i++) {
        const uint8_t c = ep_data_out_buf[i];

        if (c == 'R') {
            bootloader_request = true;
            continue;
        }

        if (c == (uint8_t)token[matched]) {
            matched++;
            if (token[matched] == '\0') {
                matched = 0;
                bootloader_request = true;
            }
        } else {
            /* Restart the match, allowing for the mismatch itself being a
             * fresh start (e.g. "DDFU!"). */
            matched = (c == (uint8_t)token[0]) ? 1 : 0;
        }
    }

    ep_set_stat_rx(EP_DATA, USB_EP_RX_VALID);
}

/* ------------------------------------------------------------------ */
/* Reset / interrupt                                                   */
/* ------------------------------------------------------------------ */

static void usb_handle_reset(void)
{
    ctrl_tx_ptr         = NULL;
    ctrl_tx_remaining   = 0;
    pending_address     = 0;
    pending_out_request = 0;

    /* Control endpoint 0. */
    EPR(EP_CTRL) = EP_CTRL | USB_EP_CONTROL;
    USB_DRD_PMA_BUFF[EP_CTRL].TXBD = PMA_EP0_TX;
    USB_DRD_PMA_BUFF[EP_CTRL].RXBD = pma_rx_blocks(EP0_MAX_PACKET) | PMA_EP0_RX;
    ep_set_stat_tx(EP_CTRL, USB_EP_TX_NAK);
    ep_set_stat_rx(EP_CTRL, USB_EP_RX_VALID);

    /* Answer on address 0 until the host assigns one. */
    USB_DRD_FS->DADDR = USB_DADDR_EF;
}

void USB_DRD_FS_IRQHandler(void)
{
    uint32_t istr;

    while ((istr = USB_DRD_FS->ISTR) & (USB_ISTR_CTR | USB_ISTR_RESET | USB_ISTR_SUSP | USB_ISTR_WKUP)) {

        if (istr & USB_ISTR_RESET) {
            USB_DRD_FS->ISTR = (uint32_t)~USB_ISTR_RESET;
            usb_handle_reset();
            continue;
        }

        if (istr & USB_ISTR_SUSP) {
            USB_DRD_FS->ISTR = (uint32_t)~USB_ISTR_SUSP;
            continue;
        }

        if (istr & USB_ISTR_WKUP) {
            USB_DRD_FS->ISTR = (uint32_t)~USB_ISTR_WKUP;
            continue;
        }

        if (istr & USB_ISTR_CTR) {
            const uint32_t ep = istr & USB_ISTR_IDN;

            if (ep == EP_CTRL) {
                if (istr & USB_ISTR_DIR) {
                    /* SETUP or OUT. Read SETUP before clearing VTRX. */
                    const bool is_setup = (EPR(EP_CTRL) & USB_CHEP_SETUP) != 0;
                    ep_clear_vtrx(EP_CTRL);

                    if (is_setup) {
                        handle_setup();
                    } else {
                        handle_ep0_out();
                    }
                } else {
                    ep_clear_vttx(EP_CTRL);
                    handle_ep0_in();
                }
            } else if (ep == EP_DATA) {
                if (EPR(EP_DATA) & USB_CHEP_VTRX) {
                    ep_clear_vtrx(EP_DATA);
                    handle_data_out();
                }
                if (EPR(EP_DATA) & USB_CHEP_VTTX) {
                    ep_clear_vttx(EP_DATA);
                }
            } else {
                if (EPR(ep) & USB_CHEP_VTRX) {
                    ep_clear_vtrx(ep);
                }
                if (EPR(ep) & USB_CHEP_VTTX) {
                    ep_clear_vttx(ep);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static void usb_clock_and_power_init(void)
{
    /*
     * The H5 isolates VDDUSB at reset. If the transceiver is left unpowered the
     * host still sees the pull-up but descriptor exchange fails outright
     * ("Device Descriptor Request Failed"), so this must happen before the
     * peripheral is clocked. PWR has no RCC enable bit on H5 -- it is always
     * clocked -- unlike F4/F7.
     */
    PWR->USBSCR |= PWR_USBSCR_USB33DEN;
    PWR->USBSCR |= PWR_USBSCR_USB33SV;

    /* 48 MHz from HSI48. Wait for it: selecting it before HSI48RDY means the
     * peripheral starts on a dead clock. */
    RCC->CR |= RCC_CR_HSI48ON;
    while ((RCC->CR & RCC_CR_HSI48RDY) == 0) {
        ;
    }

    /*
     * Select HSI48 as the USB kernel clock. This is USBSEL = 0b11 -- BOTH bits
     * set (ST's RCC_USBCLKSOURCE_HSI48 is the full RCC_CCIPR4_USBSEL mask, not
     * zero). Getting this wrong is silent and vicious: the D+ pull-up runs off
     * the APB clock, so the host still detects the device and tries to
     * enumerate, but the USB core has no clock and answers nothing. The host
     * reports "device not accepting address, error -71", which looks like a
     * cable or wiring fault rather than a clock-select fault.
     */
    RCC->CCIPR4 |= RCC_CCIPR4_USBSEL;

    /*
     * Raw HSI48 is only trimmed to about +/-1%, but full-speed USB wants
     * +/-0.25%. CRS disciplines it against the host's start-of-frame packets,
     * which is what makes enumeration reliable rather than intermittent.
     */
    RCC->APB1LENR |= RCC_APB1LENR_CRSEN;
    (void)RCC->APB1LENR;

    CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | (0x2UL << CRS_CFGR_SYNCSRC_Pos);
    CRS->CR  |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    /* PA11 = USB_DM, PA12 = USB_DP, alternate function 10. */
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    (void)RCC->AHB2ENR;

    GPIOA->MODER   = (GPIOA->MODER & ~((0x3UL << (11 * 2)) | (0x3UL << (12 * 2))))
                   | (0x2UL << (11 * 2)) | (0x2UL << (12 * 2));
    GPIOA->OTYPER &= ~((0x1UL << 11) | (0x1UL << 12));
    GPIOA->OSPEEDR |= (0x3UL << (11 * 2)) | (0x3UL << (12 * 2));
    GPIOA->PUPDR  &= ~((0x3UL << (11 * 2)) | (0x3UL << (12 * 2)));
    GPIOA->AFR[1]  = (GPIOA->AFR[1] & ~((0xFUL << ((11 - 8) * 4)) | (0xFUL << ((12 - 8) * 4))))
                   | (10UL << ((11 - 8) * 4)) | (10UL << ((12 - 8) * 4));

    RCC->APB2ENR |= RCC_APB2ENR_USBEN;
    (void)RCC->APB2ENR;
}

void usb_cdc_init(void)
{
    usb_clock_and_power_init();

    /* Exit power-down, then release the peripheral reset. */
    USB_DRD_FS->CNTR = USB_CNTR_USBRST;
    for (volatile uint32_t i = 0; i < 1000; i++) {
        ; /* > 1 us transceiver startup */
    }
    USB_DRD_FS->CNTR = 0;
    USB_DRD_FS->ISTR = 0;

    /* Zero the buffer descriptor table before anything can reference it.
     * Word writes only -- the PMA does not reliably support byte access, so
     * memset() here is not safe. */
    for (uint32_t i = 0; i < PMA_BTABLE_BYTES / 4U; i++) {
        ((volatile uint32_t *)USB_DRD_PMAADDR)[i] = 0;
    }

    USB_DRD_FS->CNTR = USB_CNTR_RESETM | USB_CNTR_CTRM | USB_CNTR_SUSPM | USB_CNTR_WKUPM;

    NVIC_SetPriority(USB_DRD_FS_IRQn, 6);
    NVIC_EnableIRQ(USB_DRD_FS_IRQn);

    /* Attach: pull D+ high so the host starts enumeration. */
    USB_DRD_FS->BCDR |= USB_BCDR_DPPU;
}
