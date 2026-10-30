# Gremlin v3: Troubleshooting, Status & Handover Guide

> **Note for AI Assistant (Home / Work):**  
> Read this document to resume work with 100% context on recent hardware findings, firmware fixes, multimeter diagnostics, and next steps for the Gremlin v3 analog gamepad.

---

## 1. Summary of Current Git & Firmware State

### Latest Status: Column 2 Pin 1 Hardware Rescue (VERIFIED WORKING!)
- **File:** `boards/shields/gremlinv3/gremlinv3.overlay`
  - Explicitly mapped **Matrix Column 2** to **Nice!Nano Silk `1` (`P0.06` / Header Pin 2 / RX)**: `<&gpio0 6 GPIO_ACTIVE_HIGH>`.
  - **Hardware Diagnostic Confirmed:** Touching Silk `1` to Silk `7` (Row 0) typed for Column 2! The rescue GPIO is 100% active and working.
  - **Hardware Rescue Procedure:** Solder a single thin jumper wire from **Nice!Nano Silk `1` (Header Pin 2)** to the **left pad of any Column 2 switch** (e.g. S3, S8, S13, S18, or S21). Since all 5 switch left pads are already verified connected by the PCB trace, this single wire restores all 5 keys (`2`, `W`, `S`, `X`, `Space`).
- **File:** `boards/shields/gremlinv3/src/custom_status_screen.c`
  - Fixed blank status screen by allocating `user_virt_buf` as `LV_IMG_CF_TRUE_COLOR` (`LV_CANVAS_BUF_SIZE_TRUE_COLOR(68, 88)`). LVGL's vector font engine (`lv_canvas_draw_text`) requires True Color to render fonts like Montserrat; the pixels are then 1:1 packed into the 1-bit physical landscape display buffer (`status_phys_buf`).
- **Action Required:** Push this commit to GitHub using **GitHub Desktop** so the GitHub Actions workflow builds the latest `.uf2` firmware.

---

## 2. Key Matrix: Column 2 Root Cause Analysis

### Physical Pin Layout on Nice!Nano (Pads 5 to 9)
```
[Pad 5 / Silk 2]  -> Col 0 (Esc, Tab, Shift, Ctrl)
[Pad 6 / Silk 3]  -> Col 1 (1, Q, A, Z)
[Pad 7 / Silk 4]  -> Col 2 (2, W, S, X, Space)  <--- Sandwiched between Col 1 & Col 3!
[Pad 8 / Silk 5]  -> Col 3 (3, E, D, C)
[Pad 9 / Silk 6]  -> Col 4 (4, R, F, V)
```

### Why Column 2 Stopped Working During Col 1 & 3 Troubleshooting:
1. **The Physical Sandwich:** Column 2 (Pad 7) is physically located right between Column 1 (Pad 6) and Column 3 (Pad 8). Any probing, soldering, or socket wiggling while troubleshooting Columns 1 and 3 placed mechanical and thermal stress directly on Pad 7.
2. **Why Remapping to Pin 1 Failed:** On the PCB, the copper track from keys `2`, `W`, `S`, `X`, `Space` runs directly to **Pad 7 (`P0.22`)**. When the software was mapped to Pin 1 (`P0.06` / `TX0`), the controller drove Pin 1 (an empty pad), while Pad 7 received no scanning pulse. Remapping to Pin 1 could only work if a physical jumper wire was connected from Pin 1 to the switches.

### 2-Minute Multimeter Diagnostic Checklist
With the board unplugged, set a multimeter to **Continuity (Beep Mode)**:

| Step | Probe Point A | Probe Point B | Expected Result | Diagnosis if Failed |
| :---: | :--- | :--- | :---: | :--- |
| **Test 1** | Nice!Nano Pin 7 (Silk `4`) | Switch `S3` (Key `2`) Top Pin | **BEEP (~0 Ω)** | If **OL / Open**: The socket contact is loose, cold solder joint on Pad 7, or cracked trace. |
| **Test 2** | Switch `S3` (Key `2`) Top Pin | Switch `S8` (Key `W`), `S13` (`S`), `S18` (`X`), `S21` (`Space`) | **BEEP (~0 Ω)** | All 5 switches share the Col 2 trace. If one doesn't beep, trace is broken between keys. |
| **Test 3** | Nice!Nano Pin 7 (Silk `4`) | Nice!Nano Pin 6 (Silk `3`) & Pin 8 (Silk `5`) | **NO BEEP (OL)** | If it **Beeps**: Solder bridge between adjacent header pins from Col 1/3 rework! |
| **Test 4** | Nice!Nano Pin 7 (Silk `4`) | Nice!Nano Pin 4 (`GND`) | **NO BEEP (OL)** | If it **Beeps**: Column 2 trace is shorted to Ground plane. |

---

## 3. Joy-Con 5-Pin Analog Joystick Pinout & PCB Rescue

### The Dissected Ribbon Cable Pinout (Verified)
- **Pin 1 (White):** Horizontal Axis (X) potentiometer wiper
- **Pin 2 (Purple):** Ground
- **Pin 3 (Yellow):** Pushbutton switch / L3 Click (switches to Ground)
- **Pin 4 (Green):** Vertical Axis (Y) potentiometer wiper
- **Pin 5 (Red):** VCC (+3.3V power rail)

### Conflict on Existing Manufactured PCB (Connector J2)
- **J2 Pad 2** connects to **3.3V Power Rail** $\rightarrow$ Shorts Joy-Con Ground to 3.3V!
- **J2 Pad 5** connects to **GND Rail** $\rightarrow$ Shorts Joy-Con 3.3V VCC to Ground!
- **J2 Pad 4** connects to **Nice!Nano Pin 17 (`P1.15`)** $\rightarrow$ `P1.15` is a digital GPIO only; Nordic nRF52840 SAADC channels (`AIN0` - `AIN7`) are located exclusively on Port 0 (`P0.xx`).

### Non-Destructive PCB Rescue Plan (Save the Manufactured Boards)
To avoid re-ordering PCBs, make simple modifications at the header pins:
1. **Power & Ground Swap:**
   - Cut the small copper trace connecting J2 Pad 5 to the GND plane. Wire J2 Pad 5 to 3.3V (Nice!Nano Pin 21).
   - Lift or snip Nice!Nano Pin 21 from PCB Pad 21; wire PCB Pad 21 (which feeds J2 Pad 2) to GND (Nice!Nano Pin 4).
2. **Y-Axis Analog Line:**
   - Lift Nice!Nano Pin 17 (`P1.15`) so it does not touch the PCB.
   - Run a jumper wire from PCB Pad 17 (J2 Pad 4) to **Nice!Nano Pin 19 (`P0.29` / `AIN5`)**—an unused analog-capable pin.
3. **Firmware Update:**
   - In `gremlinv3.overlay`, update ADC Channel for Y-axis to `AIN5` (`channel@5`), and set L3 button (`kscan_direct`) to `&pro_micro 19` (`P0.02` with internal pull-up).

---

## 4. How to Resume on Another Machine
1. Open **GitHub Desktop** on your work computer.
2. Click **Fetch / Pull** to bring in all commits and this guide.
3. In your conversation with Antigravity, say:
   > *"I just pulled the latest changes. Please read `TROUBLESHOOTING_AND_NEXT_STEPS.md` and review where we left off on the column 2 multimeter test and Joy-Con pinout rescue."*
