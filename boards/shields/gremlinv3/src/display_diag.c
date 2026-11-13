/*
 * Copyright (c) 2026 Gremlin Keypad
 * SPDX-License-Identifier: MIT
 *
 * Boot-time display and SPI bus hardware diagnostic for Gremlin v3.
 * Runs at APPLICATION priority 99 (after SPI, EXT_POWER, and ls0xx init).
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>
#include <soc.h>

LOG_MODULE_REGISTER(display_diag, LOG_LEVEL_INF);

#define PIN_CS      NRF_GPIO_PIN_MAP(0, 6)   /* P0.06: J3 Pad 1 (CS) */
#define PIN_SCK     NRF_GPIO_PIN_MAP(1, 13)  /* P1.13: J3 Pad 2 (SCK) */
#define PIN_MOSI    NRF_GPIO_PIN_MAP(1, 11)  /* P1.11: J3 Pad 3 (MOSI) */

#define PIN_CNF_INPUT_CONNECT_MASK BIT(1)
#define NUM_SPI_PINS 3U

struct diag_pin_desc {
    const char *name;
    uint32_t pin;
    NRF_GPIO_Type *port;
    uint8_t bit;
};

static const struct diag_pin_desc spi_pins[NUM_SPI_PINS] = {
    { "CS",   PIN_CS,   NRF_P0, 6  },
    { "SCK",  PIN_SCK,  NRF_P1, 13 },
    { "MOSI", PIN_MOSI, NRF_P1, 11 },
};

struct diag_pin_result {
    uint32_t init_cnf;
    uint8_t init_out;
    uint8_t np;
    uint8_t pu;
    uint8_t pd;
    uint8_t drv0;
    uint8_t drv1;
    uint8_t hi_victims;
    uint8_t lo_victims;
    uint8_t row3_on_hi;
};

struct diag_report {
    bool disp_ready;
    const char *disp_name;
    uint32_t nfcpins;
    uint32_t ext_cnf;
    uint8_t ext_out;
    uint8_t ext_in;
    uint8_t row3_idle;
    uint32_t row3_cnf;
    uint32_t spim3_enable;
    uint32_t spim3_sck;
    uint32_t spim3_mosi;
    uint32_t spim0_enable;
    struct diag_pin_result pins[NUM_SPI_PINS];
};

static struct diag_report report;
static struct k_work_delayable replay_work;

static uint8_t sample_pin_input(NRF_GPIO_Type *port, uint8_t bit)
{
    uint32_t cnf = port->PIN_CNF[bit];

    port->PIN_CNF[bit] = cnf & ~PIN_CNF_INPUT_CONNECT_MASK;
    k_busy_wait(20);
    uint8_t val = (uint8_t)((port->IN >> bit) & 1U);
    port->PIN_CNF[bit] = cnf;
    return val;
}

static uint8_t read_pull_level(uint32_t pin, nrf_gpio_pin_pull_t pull)
{
    nrf_gpio_cfg_input(pin, pull);
    k_busy_wait(2000);
    return nrf_gpio_pin_read(pin) ? 1U : 0U;
}

static uint8_t drive_and_read(const struct diag_pin_desc *p, uint8_t val)
{
    nrf_gpio_pin_write(p->pin, val);
    nrf_gpio_cfg_output(p->pin);
    /* Connect input buffer so IN reflects actual physical pin level */
    p->port->PIN_CNF[p->bit] &= ~PIN_CNF_INPUT_CONNECT_MASK;
    k_busy_wait(1000);
    return (uint8_t)((p->port->IN >> p->bit) & 1U);
}

static void probe_spi_pins(void)
{
    for (size_t i = 0; i < NUM_SPI_PINS; i++) {
        report.pins[i].init_cnf = spi_pins[i].port->PIN_CNF[spi_pins[i].bit];
        report.pins[i].init_out = (uint8_t)((spi_pins[i].port->OUT >> spi_pins[i].bit) & 1U);
    }

    for (size_t i = 0; i < NUM_SPI_PINS; i++) {
        report.pins[i].np = read_pull_level(spi_pins[i].pin, NRF_GPIO_PIN_NOPULL);
        report.pins[i].pu = read_pull_level(spi_pins[i].pin, NRF_GPIO_PIN_PULLUP);
        report.pins[i].pd = read_pull_level(spi_pins[i].pin, NRF_GPIO_PIN_PULLDOWN);
    }

    for (size_t i = 0; i < NUM_SPI_PINS; i++) {
        struct diag_pin_result *res = &report.pins[i];

        for (size_t j = 0; j < NUM_SPI_PINS; j++) {
            if (j != i) {
                nrf_gpio_cfg_input(spi_pins[j].pin, NRF_GPIO_PIN_PULLDOWN);
            }
        }
        res->drv1 = drive_and_read(&spi_pins[i], 1U);
        res->row3_on_hi = (uint8_t)((NRF_P0->IN >> 10) & 1U);
        for (size_t j = 0; j < NUM_SPI_PINS; j++) {
            if ((j != i) && nrf_gpio_pin_read(spi_pins[j].pin)) {
                res->hi_victims |= (uint8_t)BIT(j);
            }
        }

        for (size_t j = 0; j < NUM_SPI_PINS; j++) {
            if (j != i) {
                nrf_gpio_cfg_input(spi_pins[j].pin, NRF_GPIO_PIN_PULLUP);
            }
        }
        res->drv0 = drive_and_read(&spi_pins[i], 0U);
        for (size_t j = 0; j < NUM_SPI_PINS; j++) {
            if ((j != i) && !nrf_gpio_pin_read(spi_pins[j].pin)) {
                res->lo_victims |= (uint8_t)BIT(j);
            }
        }

        nrf_gpio_cfg_input(spi_pins[i].pin, NRF_GPIO_PIN_PULLDOWN);
    }

    for (size_t i = 0; i < NUM_SPI_PINS; i++) {
        nrf_gpio_pin_write(spi_pins[i].pin, report.pins[i].init_out);
        spi_pins[i].port->PIN_CNF[spi_pins[i].bit] = report.pins[i].init_cnf;
    }
}

static void log_report(const char *tag)
{
    if (report.disp_ready) {
        LOG_INF("[DIAG-%s] DISPLAY READY (%s) EXT_PWR out=%u in=%u cnf=0x%08x NFCPINS=0x%08x",
                tag, report.disp_name,
                (unsigned int)report.ext_out, (unsigned int)report.ext_in,
                (unsigned int)report.ext_cnf, (unsigned int)report.nfcpins);
        LOG_INF("[DIAG-%s] SPIM3 en=%u sck=0x%08x mosi=0x%08x SPIM0 en=%u",
                tag, (unsigned int)report.spim3_enable,
                (unsigned int)report.spim3_sck, (unsigned int)report.spim3_mosi,
                (unsigned int)report.spim0_enable);
        return;
    }

    LOG_ERR("[DIAG-%s] DISPLAY NOT READY (%s) EXT_PWR out=%u in=%u cnf=0x%08x NFCPINS=0x%08x",
            tag, report.disp_name,
            (unsigned int)report.ext_out, (unsigned int)report.ext_in,
            (unsigned int)report.ext_cnf, (unsigned int)report.nfcpins);
    LOG_ERR("[DIAG-%s] SPIM3 en=%u sck=0x%08x mosi=0x%08x SPIM0 en=%u ROW3 in=%u cnf=0x%08x",
            tag, (unsigned int)report.spim3_enable,
            (unsigned int)report.spim3_sck, (unsigned int)report.spim3_mosi,
            (unsigned int)report.spim0_enable, (unsigned int)report.row3_idle,
            (unsigned int)report.row3_cnf);

    for (size_t i = 0; i < NUM_SPI_PINS; i++) {
        const struct diag_pin_result *r = &report.pins[i];
        LOG_ERR("[DIAG-%s] %s np=%u pu=%u pd=%u drv=%u/%u hi_v=0x%x lo_v=0x%x r3_hi=%u cnf=0x%08x",
                tag, spi_pins[i].name,
                (unsigned int)r->np, (unsigned int)r->pu, (unsigned int)r->pd,
                (unsigned int)r->drv0, (unsigned int)r->drv1,
                (unsigned int)r->hi_victims, (unsigned int)r->lo_victims,
                (unsigned int)r->row3_on_hi, (unsigned int)r->init_cnf);
    }
}

static void replay_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    log_report("LATE");
}

#define J3_CNF_IN_NP   0x00000000UL
#define J3_CNF_IN_PD   0x00000004UL
#define J3_CNF_IN_PU   0x0000000CUL
#define J3_CNF_OUT_IN  0x00000001UL
#define J3_CNF_OUT_HI  0x00000301UL

struct j3_gpio_ref {
    const char *name;
    uint8_t port_id;
    uint8_t pin;
};

static const struct j3_gpio_ref j3_pins[3] = {
    { "CS(P0.06)",   0U, 6U  },
    { "SCK(P1.13)",  1U, 13U },
    { "MOSI(P1.11)", 1U, 11U },
};

static inline NRF_GPIO_Type *j3_port(const struct j3_gpio_ref *p)
{
    return (p->port_id == 0U) ? NRF_P0 : NRF_P1;
}

static inline uint8_t j3_pin_read(const struct j3_gpio_ref *p)
{
    return (uint8_t)((j3_port(p)->IN >> p->pin) & 1U);
}

static inline void j3_pin_write(const struct j3_gpio_ref *p, uint8_t val)
{
    if (val != 0U) {
        j3_port(p)->OUTSET = (1UL << p->pin);
    } else {
        j3_port(p)->OUTCLR = (1UL << p->pin);
    }
}

static void j3_probe_electrical(void)
{
    /* Drain stray capacitance briefly, then test pull-ups, pull-downs, drive, and cross-shorts */
    for (int i = 0; i < 3; i++) {
        j3_port(&j3_pins[i])->PIN_CNF[j3_pins[i].pin] = J3_CNF_OUT_IN;
        j3_pin_write(&j3_pins[i], 0U);
    }
    k_busy_wait(200);

    for (int i = 0; i < 3; i++) {
        j3_port(&j3_pins[i])->PIN_CNF[j3_pins[i].pin] = J3_CNF_IN_NP;
    }
    k_busy_wait(100);

    for (int i = 0; i < 3; i++) {
        const struct j3_gpio_ref *t = &j3_pins[i];
        const struct j3_gpio_ref *o1 = &j3_pins[(i + 1) % 3];
        const struct j3_gpio_ref *o2 = &j3_pins[(i + 2) % 3];

        /* Discharge target pin before switching all 3 to Hi-Z input */
        j3_port(t)->PIN_CNF[t->pin] = J3_CNF_OUT_IN;
        j3_pin_write(t, 0U);
        k_busy_wait(100);

        for (int k = 0; k < 3; k++) {
            j3_port(&j3_pins[k])->PIN_CNF[j3_pins[k].pin] = J3_CNF_IN_NP;
        }
        NRF_P0->PIN_CNF[8] = J3_CNF_IN_PD;
        NRF_P1->PIN_CNF[15] = J3_CNF_IN_PD;
        k_busy_wait(100);

        uint8_t np = j3_pin_read(t);

        /* Pull-up rise profile: pu0=0 indicates short-to-GND or unpowered ESD clamp */
        j3_port(t)->PIN_CNF[t->pin] = J3_CNF_IN_PU;
        k_busy_wait(5);
        uint8_t pu0 = j3_pin_read(t);
        k_msleep(2);
        uint8_t pu2m = j3_pin_read(t);
        k_msleep(18);
        uint8_t pu20m = j3_pin_read(t);

        /* Pull-down check: pd=1 indicates short-to-3.3V or external pull-up */
        j3_port(t)->PIN_CNF[t->pin] = J3_CNF_IN_PD;
        k_busy_wait(200);
        uint8_t pd = j3_pin_read(t);

        /* Active drive HIGH with other lines pulled DOWN to catch bridges */
        j3_port(o1)->PIN_CNF[o1->pin] = J3_CNF_IN_PD;
        j3_port(o2)->PIN_CNF[o2->pin] = J3_CNF_IN_PD;
        j3_port(t)->PIN_CNF[t->pin] = J3_CNF_OUT_IN;
        j3_pin_write(t, 1U);
        k_busy_wait(100);
        uint8_t drvH = j3_pin_read(t);
        uint8_t x1h = j3_pin_read(o1);
        uint8_t x2h = j3_pin_read(o2);
        uint8_t d0h = (uint8_t)((NRF_P0->IN >> 8) & 1U);
        uint8_t r3h = (uint8_t)((NRF_P0->IN >> 10) & 1U);
        uint8_t d18h = (uint8_t)((NRF_P1->IN >> 15) & 1U);

        /* Active drive LOW with other lines pulled UP to catch pull-down bridges */
        j3_port(o1)->PIN_CNF[o1->pin] = J3_CNF_IN_PU;
        j3_port(o2)->PIN_CNF[o2->pin] = J3_CNF_IN_PU;
        j3_pin_write(t, 0U);
        k_busy_wait(100);
        uint8_t drvL = j3_pin_read(t);
        uint8_t x1l = j3_pin_read(o1);
        uint8_t x2l = j3_pin_read(o2);

        LOG_INF("[DIAG-PIN] %s np=%u pu=%u/%u/%u pd=%u drv=%u/%u",
                t->name, (unsigned int)np, (unsigned int)pu0, (unsigned int)pu2m,
                (unsigned int)pu20m, (unsigned int)pd, (unsigned int)drvH, (unsigned int)drvL);
        LOG_INF("[DIAG-X] %s %s=%u/%u %s=%u/%u adj=%u/%u/%u",
                t->name, o1->name, (unsigned int)x1h, (unsigned int)x1l,
                o2->name, (unsigned int)x2h, (unsigned int)x2l,
                (unsigned int)d0h, (unsigned int)r3h, (unsigned int)d18h);
    }
}

static void j3_bb_tx_lsb(uint8_t byte)
{
    for (int bit = 0; bit < 8; bit++) {
        j3_pin_write(&j3_pins[2], (uint8_t)((byte >> bit) & 1U)); /* MOSI */
        k_busy_wait(5);
        j3_pin_write(&j3_pins[1], 1); /* SCK rising edge */
        k_busy_wait(5);
        j3_pin_write(&j3_pins[1], 0); /* SCK falling edge */
        k_busy_wait(5);
    }
}

static void j3_bb_clear(void)
{
    j3_pin_write(&j3_pins[2], 0);
    j3_pin_write(&j3_pins[1], 0);
    j3_pin_write(&j3_pins[0], 1); /* CS active HIGH */
    k_busy_wait(25);              /* tsSCS >= 6 us */
    j3_bb_tx_lsb(0x04);           /* All clear */
    j3_bb_tx_lsb(0x00);
    k_busy_wait(15);              /* thSCS >= 2 us */
    j3_pin_write(&j3_pins[0], 0); /* CS idle LOW */
    k_busy_wait(100);
}

static void j3_bb_write_line(uint8_t cmd, uint8_t line1, uint8_t pat)
{
    j3_pin_write(&j3_pins[0], 1);
    k_busy_wait(25);
    j3_bb_tx_lsb(cmd);
    j3_bb_tx_lsb(line1);
    for (int col = 0; col < 20; col++) {
        j3_bb_tx_lsb(pat);
    }
    j3_bb_tx_lsb(0x00);
    j3_bb_tx_lsb(0x00);
    k_busy_wait(15);
    j3_pin_write(&j3_pins[0], 0);
    k_busy_wait(30);
}

static void j3_test_sharp_bitbang(void)
{
    for (int i = 0; i < 3; i++) {
        j3_port(&j3_pins[i])->PIN_CNF[j3_pins[i].pin] = J3_CNF_OUT_HI;
        j3_pin_write(&j3_pins[i], 0U);
    }
    k_busy_wait(200);

    j3_bb_clear();
    k_msleep(2);

    /* Draw 2 full-screen frames toggling VCOM bit (0x01 then 0x03) */
    for (int frame = 0; frame < 2; frame++) {
        uint8_t cmd = (frame & 1) ? 0x03U : 0x01U;
        for (uint8_t line = 1; line <= 68; line++) {
            uint8_t band = (uint8_t)((line - 1U) / 8U);
            uint8_t pat = ((band & 1U) == 0U) ? 0x00U : 0xFFU;
            if (band == 2U || band == 5U) {
                pat = (((line - 1U) & 4U) == 0U) ? 0x0FU : 0xF0U;
            }
            j3_bb_write_line(cmd, line, pat);
            if ((line % 16U) == 0U) {
                k_msleep(1);
            }
        }
        k_msleep(100);
    }

    LOG_INF("[DIAG-BB] SHARP 160x68 BITBANG DONE (68 lines x2, H0H1, tsSCS=25us, ~66kHz)");
}

static struct k_work_delayable j3_diag_work;
static uint8_t j3_diag_retries;

static void j3_diag_work_handler(struct k_work *work)
{
    (void)work;

    if (NRF_SPIM3->ENABLE != 0 && j3_diag_retries < 10) {
        j3_diag_retries++;
        k_work_schedule(&j3_diag_work, K_MSEC(100));
        return;
    }

    uint32_t saved_en = NRF_SPIM3->ENABLE;
    uint32_t saved_sck = NRF_SPIM3->PSEL.SCK;
    uint32_t saved_mosi = NRF_SPIM3->PSEL.MOSI;
    uint32_t cnf_cs = NRF_P0->PIN_CNF[6];
    uint32_t cnf_sck = NRF_P1->PIN_CNF[13];
    uint32_t cnf_mosi = NRF_P1->PIN_CNF[11];
    uint32_t cnf_d0 = NRF_P0->PIN_CNF[8];
    uint32_t cnf_d18 = NRF_P1->PIN_CNF[15];
    uint8_t out_cs = (uint8_t)((NRF_P0->OUT >> 6) & 1U);
    uint8_t out_sck = (uint8_t)((NRF_P1->OUT >> 13) & 1U);
    uint8_t out_mosi = (uint8_t)((NRF_P1->OUT >> 11) & 1U);

    LOG_INF("[DIAG-HW] PRE SPIM3 en=%u psel(sck=0x%x mosi=0x%x) CNF(cs=0x%08x sck=0x%08x mosi=0x%08x) OUT=%u/%u/%u",
            (unsigned int)saved_en, (unsigned int)saved_sck, (unsigned int)saved_mosi,
            (unsigned int)cnf_cs, (unsigned int)cnf_sck, (unsigned int)cnf_mosi,
            (unsigned int)out_cs, (unsigned int)out_sck, (unsigned int)out_mosi);

    NRF_SPIM3->ENABLE = 0;
    NRF_SPIM3->PSEL.SCK = 0xFFFFFFFFUL;
    NRF_SPIM3->PSEL.MOSI = 0xFFFFFFFFUL;

    j3_probe_electrical();
    j3_test_sharp_bitbang();

    j3_pin_write(&j3_pins[0], out_cs);
    j3_pin_write(&j3_pins[1], out_sck);
    j3_pin_write(&j3_pins[2], out_mosi);
    NRF_P0->PIN_CNF[8] = cnf_d0;
    NRF_P1->PIN_CNF[15] = cnf_d18;
    NRF_P0->PIN_CNF[6] = cnf_cs;
    NRF_P1->PIN_CNF[13] = cnf_sck;
    NRF_P1->PIN_CNF[11] = cnf_mosi;
    NRF_SPIM3->PSEL.SCK = saved_sck;
    NRF_SPIM3->PSEL.MOSI = saved_mosi;
    NRF_SPIM3->ENABLE = saved_en;
}

static int display_diag_init(void)
{
#if DT_HAS_CHOSEN(zephyr_display)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    report.disp_ready = device_is_ready(disp);
    report.disp_name = (disp->name != NULL) ? disp->name : "null";
#else
    report.disp_ready = false;
    report.disp_name = "no-chosen";
#endif

    report.nfcpins = NRF_UICR->NFCPINS;
    report.ext_cnf = NRF_P0->PIN_CNF[13];
    report.ext_out = (uint8_t)((NRF_P0->OUT >> 13) & 1U);
    report.ext_in = sample_pin_input(NRF_P0, 13);

    report.row3_cnf = NRF_P0->PIN_CNF[10];
    report.row3_idle = (uint8_t)((NRF_P0->IN >> 10) & 1U);

    report.spim3_enable = NRF_SPIM3->ENABLE;
    report.spim3_sck = NRF_SPIM3->PSEL.SCK;
    report.spim3_mosi = NRF_SPIM3->PSEL.MOSI;
    report.spim0_enable = NRF_SPIM0->ENABLE;

    if (!report.disp_ready) {
        probe_spi_pins();
    }

    log_report("BOOT");

    k_work_init_delayable(&replay_work, replay_work_handler);
    k_work_schedule(&replay_work, K_SECONDS(7));
    k_work_init_delayable(&j3_diag_work, j3_diag_work_handler);
    k_work_schedule(&j3_diag_work, K_SECONDS(8));
    return 0;
}

SYS_INIT(display_diag_init, APPLICATION, 99);
