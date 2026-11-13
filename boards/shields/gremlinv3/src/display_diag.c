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
    return 0;
}

SYS_INIT(display_diag_init, APPLICATION, 99);
