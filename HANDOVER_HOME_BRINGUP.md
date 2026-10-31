# Gremlin v3 Gamepad — Home Bring-Up & Handover Guide

**Target Hardware:** Gremlin v3 Custom Gamepad  
**Controller:** Nice!Nano v2 (Nordic nRF52840) running ZMK Firmware  
**Joystick:** Nintendo Switch Joy-Con Hall Effect Analog Thumbstick (5-pin 0.5mm FPC at J2)  
**Display:** Hardwired Nice!View (Sharp Memory LCD 160×68 via custom SPI0)  
**Branch:** `main` | **Target Repository:** `https://github.com/AmazingSpanoMan/Gremlin_Keypad`

---

## 1. Executive Summary: What Was Happening & Why

### The Symptom You Observed
- Tilting the joystick in any direction typed nothing.
- Pressing down on the joystick shaft (L3 click) caused the keyboard to start typing `a` continuously.
- While clicked down, tilting into other directions momentarily typed that direction's key (`w`, `s`, `d`) before snapping back into continuous `a`s.

### The Root Cause (Hardware Pin Mismatch)
The Gremlin v3 PCB was designed around an **ATmega32U4 Pro Micro**, where pins 18, 19, 20, and 21 are all analog inputs (`A0`–`A3`). However, on the **Nice!Nano v2 (nRF52840)**:
1. **Silk 18 (Header Pin 17 / J2 Pin 4 — Joy-Con X-Axis) is `P1.15`:**
   * `P1.15` is a **digital-only GPIO**. The Nordic nRF52840 silicon has no analog/ADC circuitry on Port 1. The controller physically cannot read analog voltages on Silk 18.
2. **Silk 19 (Header Pin 18 / J2 Pin 3 — Joy-Con Button) is `P0.02` (`AIN0`):**
   * The joystick's L3 tactile switch is wired here.
   * In firmware, **Channel 0 was set to read `AIN0`**. The firmware was reading your **button press as the X-axis**!
   * At rest, the button pin is pulled HIGH (3.3V). When you pressed the stick down, the button shorted to GND (0V). The firmware saw the ADC plunge to 0V, interpreted it as 100% Left deflection, and latched into typing continuous `a`s.
3. **Silk 21 (Header Pin 20 / J2 Pin 1 — Joy-Con Y-Axis) is `P0.31` (`AIN7`):**
   * The Y-axis is wired to `AIN7`, but the firmware was listening to `AIN5` (Silk 20, which is completely empty and unconnected on the PCB). That is why tilting Y produced no output.

---

## 2. Hardware Jumper (Applied & Verified!)

The jumper between **Header Pin 17 (Silk 18)** and **Header Pin 19 (Silk 20)** has been **soldered and verified**:
- Clicking the stick now cleanly types `0` without any stuck `a` keys!
- Silk 19 is isolated as a pure digital button (`&kp N0`).
- Silk 20 is connected directly to the X-axis analog output.

> [!IMPORTANT]
> **No trace cutting is needed for this step.**  
> Header Pin 19 (Silk 20) is completely unconnected on the PCB, so bridging Pin 17 to Pin 19 routes the X-axis analog signal directly into `P0.29` (`AIN5`). Pin 17 (`P1.15`) is unconfigured in devicetree and remains in high-impedance floating mode with zero interference.

### Jumper Diagram (Nice!Nano Right Header, Viewed from Bottom of Board):
```
Nice!Nano Right Header:
 ┌────────────────────────────────────────────────────────┐
 │ Pin 13 / Silk 10 : Row 4 (P0.09)                       │
 │ Pin 14 / Silk 16 : Row 3 (P0.10)                       │
 │ Pin 15 / Silk 14 : Unconnected                         │
 │ Pin 16 / Silk 15 : Unconnected                         │
 │ Pin 17 / Silk 18 : Joy-Con X-Axis (J2 Pin 4)    ──┐    │
 │ Pin 18 / Silk 19 : Joy-Con Button (J2 Pin 3)      │    │ JUMPER WIRE
 │ Pin 19 / Silk 20 : UNCONNECTED -> AIN5 (P0.29)  ◄─┘    │ (Pin 17 to Pin 19)
 │ Pin 20 / Silk 21 : Joy-Con Y-Axis (J2 Pin 1) -> AIN7   │
 │ Pin 21 / 3.3V    : J2 Pin 2 (Reworked to GND)          │
 │ Pin 22 / RST     : Reset                               │
 │ Pin 23 / GND     : Ground                              │
 │ Pin 24 / B+      : Battery Positive                    │
 └────────────────────────────────────────────────────────┘
```

---

## 3. Complete Pinout & Signal Mapping Reference

| PCB Silk Label | Header Pin | Schematic Net | nRF52840 Pin | SAADC Ch | ZMK Pin Label | Physical Function | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Silk 18** | Pin 17 | `Net-(J2-Pin_4)` | `P1.15` | *None (Digital)* | — | Joy-Con X-Axis | Jumper to Silk 20 |
| **Silk 19** | Pin 18 | `Net-(J2-Pin_3)` | `P0.02` | `AIN0` | `&pro_micro 19` | Stick Push (L3 Click) | Active-Low Button |
| **Silk 20** | Pin 19 | *(Unconnected)* | `P0.29` | **`AIN5`** | `&adc 0` | Joy-Con X-Axis (via jumper) | ADC Channel 0 |
| **Silk 21** | Pin 20 | `Net-(J2-Pin_1)` | `P0.31` | **`AIN7`** | `&adc 1` | Joy-Con Y-Axis | ADC Channel 1 |
| **Silk 1** | Pin 23 | `Col 2` | `P0.06` | — | `<&gpio0 6>` | Matrix Column 2 Rescue Pin | Verified Working |

---

## 4. Multimeter Verification Baseline (Live Hardware Readings)

When measuring DC voltage with a multimeter on the Nice!Nano header pins relative to GND:

| Signal / Pin | At Rest (Neutral) | Full Tilt / Pressed | Expected Behavior |
| :--- | :--- | :--- | :--- |
| **Silk 20 (X-Axis, via Jumper)** | `~1.68 V` | **Left:** `~2.99 V`<br>**Right:** `~0.52 V` | Midpoint at rest, full 2.5V swing |
| **Silk 21 (Y-Axis)** | `~1.70 V` | **Up:** `~0.41 V`<br>**Down:** `~3.01 V` | Midpoint at rest, full 2.6V swing |
| **Silk 19 (L3 Button)** | `3.28 V` (High) | **Clicked:** `0.00 V` (GND) | Crisp active-low digital switch |

---

## 5. Firmware Configuration (`gremlinv3.overlay`)

The overlay has been committed and pushed to `origin/main` with the corrected configuration:

```dts
/* -------------------------------------------------------------------------
 * Analog Stick Driver (wdtamagi/zmk-driver-analog-stick)
 * ------------------------------------------------------------------------- */
analog_stick0: analog_stick0 {
    compatible = "zmk,analog-stick";
    io-channels = <&adc 0>, <&adc 1>; /* Channel 0 (AIN5: X) & Channel 1 (AIN7: Y) */
    invert-x;
    x-min = <500>;
    x-center = <1909>;
    x-max = <3500>;
    y-min = <400>;
    y-center = <1933>;
    y-max = <3500>;
    deadzone-percent = <20>;
    wait-period-idle = <100>;
    wait-period-active = <1>;
};

analog_stick_listener0: analog_stick_listener0 {
    compatible = "zmk,analog-stick-listener";
    device = <&analog_stick0>;

    /* Layer 0: Switch Mode (WASD emulation) */
    layer0 {
        layers = <0>;
        mode = <ANALOG_STICK_MODE_SWITCH>;
        switch-activation-point = <28>;
        switch-hysteresis = <6>;
        up-position = <23>;    /* &kp W */
        down-position = <24>;  /* &kp S */
        left-position = <25>;  /* &kp A */
        right-position = <26>; /* &kp D */
    };
};

/* -------------------------------------------------------------------------
 * SAADC Configuration (Nordic PSELP: AIN5 = 6, AIN7 = 8)
 * ------------------------------------------------------------------------- */
#undef NRF_SAADC_AIN5
#define NRF_SAADC_AIN5 6
#undef NRF_SAADC_AIN7
#define NRF_SAADC_AIN7 8

&adc {
    status = "okay";
    #address-cells = <1>;
    #size-cells = <0>;

    /* Channel 0: X-Axis on Silk 20 (P0.29 / AIN5 -> PSELP 6) */
    channel@0 {
        reg = <0>;
        zephyr,gain = "ADC_GAIN_1_6";
        zephyr,reference = "ADC_REF_INTERNAL";
        zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
        zephyr,input-positive = <NRF_SAADC_AIN5>;
        zephyr,resolution = <12>;
    };

    /* Channel 1: Y-Axis on Silk 21 (P0.31 / AIN7) */
    channel@1 {
        reg = <1>;
        zephyr,gain = "ADC_GAIN_1_6";
        zephyr,reference = "ADC_REF_INTERNAL";
        zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
        zephyr,input-positive = <NRF_SAADC_AIN7>;
        zephyr,resolution = <12>;
    };
};

/* -------------------------------------------------------------------------
 * L3 Click Button (Direct GPIO)
 * ------------------------------------------------------------------------- */
kscan_direct: kscan_direct {
    compatible = "zmk,kscan-gpio-direct";
    wakeup-source;
    input-gpios = <&pro_micro 19 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* P0.02 (Silk 19) */
};
```

---

## 6. How to Continue at Home

1. **Pull the latest code on your home computer:**
   ```bash
   git clone https://github.com/AmazingSpanoMan/Gremlin_Keypad.git
   # or if already cloned:
   git pull origin main
   ```
2. **Download the latest firmware build:**
   * Go to the **Actions** tab on `https://github.com/AmazingSpanoMan/Gremlin_Keypad`.
   * Click on the latest workflow run (triggered by this commit).
   * Download the `firmware.zip` artifact and unzip it to find `gremlinv3-nice_nano_v2-zmk.uf2`.
3. **Flash the Nice!Nano v2:**
   * Double-tap the hardware **RST** button on the Nice!Nano (or short RST to GND twice quickly).
   * A mass storage drive named `NICENANO` will appear on your computer.
   * Drag and drop `gremlinv3-nice_nano_v2-zmk.uf2` onto the `NICENANO` drive.
4. **Hardware Verification:**
   * Solder the small jumper between **Pin 17 (Silk 18)** and **Pin 19 (Silk 20)**.
   * Plug in USB and test:
     - Neutral stick: Silent (no typing).
     - Tilt Up: Types `w`.
     - Tilt Down: Types `s`.
     - Tilt Left: Types `a`.
     - Tilt Right: Types `d`.
     - Press Stick Down (Click): Types `0` (or whatever behavior is assigned to position 22).
     - Keys S1–S22: All matrix keys (including Column 2 via rescue pin Silk 1) type normally.
