# Gremlin Gamepad

Custom ergonomic analog gaming keypad powered by **ZMK Firmware**, **Nice!Nano v2** (nRF52840), **Nice!View** (Sharp Memory LCD 160×68), and a **Nintendo Switch Joy-Con analog thumbstick**.

---

## Repository Structure

```text
Gremlin_Gamepad/
├── .github/workflows/          # Automated ZMK cloud firmware build via GitHub Actions
├── boards/shields/gremlinv3/   # Gremlin v3 hardware shield definition
│   ├── src/                    # Custom LVGL status screen & 64x64 Gremlin mascot animations
│   ├── gremlinv3.overlay       # Devicetree pin assignments, matrix, ADC, and SPI display
│   ├── gremlinv3.conf          # Default shield Kconfig options
│   └── gremlinv3.keymap        # Default matrix layout and bindings
├── config/                     # User configuration & keymap
│   ├── gremlinv3.conf          # Active ZMK features (ZMK Studio, Display, SAADC, BLE)
│   ├── gremlinv3.keymap        # User-customizable keymap with ZMK Studio support
│   └── west.yml                # Manifest pinning ZMK v0.3 & Joy-Con analog stick driver
├── zephyr/                     # Module configuration for CMake and Zephyr build system
├── build.yaml                  # ZMK automated build matrix definition
├── CMakeLists.txt              # CMake build targets for custom display widgets and art
│
├── Gremlin_v3/                 # Gremlin v3 Hardware Engineering Files
│   ├── KiCad/                  # Schematic, PCB layout, JLCPCB Gerbers, CPL (Pick & Place), BOM
│   ├── CAD_Files/              # 3D enclosure models, ScottoCaps keycaps, and STEP assemblies
│   └── CNC_Files/              # Carvera CNC isolation milling NC files and tooling
│
├── Gremlin_v2/                 # Legacy Gremlin v2 design files & firmware archive
└── Gremlin_v1/                 # Legacy Gremlin v1 firmware archive
```

---

## Firmware Features

* **ZMK Studio Ready:** Live key remapping on the fly without reflashing.
* **Analog Thumbstick:** Direct SAADC sampling of Nintendo Switch Joy-Con analog stick with 8-way digital emulation or mouse emulation.
* **Custom Nice!View Display:**
  * **Boot Screen:** Animated Gremlin startup logo.
  * **Active Screen:** 64×64 animated Gremlin mascot (Prowl, Twitch, Snarl, Claw Slash) alongside real-time Battery %, BLE profile, and active Layer name.
  * **Standby Screen:** Curled-up gargoyle sleeping mode with low-power screen idle to maximize battery life.

---

## Hardware Production Files

All hardware manufacturing files are located in [`Gremlin_v3/`](Gremlin_v3/):
* **PCB Fabrication & SMT Assembly:** [`Gremlin_v3/KiCad/Gremlin_v3/`](Gremlin_v3/KiCad/Gremlin_v3/)
  * Ready-to-order Gerbers: `Gremlin_v3_JLCPCB.zip`
  * Complete PCBA Assembly Package (Gerbers + CPL + BOM): `Gremlin_v3_JLCPCB_Assembly_Package.zip`
* **3D Printing & CAD:** [`Gremlin_v3/CAD_Files/`](Gremlin_v3/CAD_Files/)
* **CNC Milling:** [`Gremlin_v3/CNC_Files/`](Gremlin_v3/CNC_Files/)
