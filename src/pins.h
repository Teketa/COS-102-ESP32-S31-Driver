// GPIO assignment for the COS-102 hat on the ESP32-S31-Function-CoreBoard-1.
// Verified against cos102_s31_hat (J2 pad nets) and the CoreBoard user guide:
// hat J2 pin n sits on CoreBoard J2 pin n. Every signal goes through a
// 74AHCT125 buffer and a 33 R series resistor to the panel's IDC IN connector.
//
// Colour-to-IDC-pin assignment measured from captures 33-35 (S3 project):
// IDC pin 2 = BLUE, pin 3 = GREEN, pin 4 = RED, pin 1 = copy of RED.
#pragma once

#define PIN_CLK     47  // J2.13 -> U8A  -> IDC 10 (DCLK, shift clock)
#define PIN_DATA_R  48  // J2.14 -> U8D  -> IDC 4 (+ U10B copy -> IDC 1)
#define PIN_DATA_G  45  // J2.15 -> U8C  -> IDC 3
#define PIN_DATA_B  46  // J2.16 -> U8B  -> IDC 2
#define PIN_LAT     43  // J2.17 -> U9A  -> IDC 9  (LE)
#define PIN_GCLK    44  // J2.18 -> U9B  -> IDC 11 (grayscale clock)
#define PIN_ADDR_A  40  // J2.19 -> U9C  -> IDC 8  (scan address LSB)
#define PIN_ADDR_B  42  // J2.20 -> U9D  -> IDC 7
#define PIN_ADDR_C  39  // J2.21 -> U10A -> IDC 6  (scan address MSB)

// Quad-SPI needs a fourth data line (IO3) at init. It gets a spare J2 pin,
// which is handed straight back to GPIO (input, undriven) once the bus is up:
// the frame path only routes CLK and R/G/B to the SPI peripheral.
#define PIN_QSPI_D3_SPARE 49   // J2.11, free on the hat

// Pins deliberately left alone (CoreBoard schematic + ESP32-S31 IO guide):
//   GPIO4       also the Ethernet PHY interrupt (ETH_INTN via R83)
//   GPIO36/37   strapping (VDD_SPI voltage, JTAG select)
//   GPIO60      strapping + on-board RGB LED
//   GPIO61      strapping (BOOT) + BOOT button
//   GPIO58/59   UART0 -> USB-UART bridge (console + flashing)

// The bit-bang engines write the GPIO output registers directly. All nine
// panel signals live in the second GPIO bank (32..63), so one W1TS/W1TC
// register write moves several lines at once, as on the S3 (where they were
// all in bank 0).
#define PANEL_GPIO_BANK1(p) ((p) >= 32 && (p) < 64)
_Static_assert(PANEL_GPIO_BANK1(PIN_CLK) && PANEL_GPIO_BANK1(PIN_DATA_R) &&
               PANEL_GPIO_BANK1(PIN_DATA_G) && PANEL_GPIO_BANK1(PIN_DATA_B) &&
               PANEL_GPIO_BANK1(PIN_LAT) && PANEL_GPIO_BANK1(PIN_GCLK) &&
               PANEL_GPIO_BANK1(PIN_ADDR_A) && PANEL_GPIO_BANK1(PIN_ADDR_B) &&
               PANEL_GPIO_BANK1(PIN_ADDR_C),
               "bit-bang engines expect every panel pin in GPIO bank 1 (32..63)");
#define BANK1_BIT(p) (1u << ((p) - 32))
