#pragma once
#include <stdint.h>
#include <stdbool.h>

bool dp_qspi_init(void);
bool dp_qspi_set_clock(int hz);   // runtime data-clock change (overclocking)
int  dp_qspi_get_clock(void);
void dp_qspi_route_spi(void);    // CLK/R/G/B -> SPI peripheral
void dp_qspi_route_gpio(void);   // CLK/R/G/B -> GPIO (bit-bang commands)
void dp_qspi_begin(void);        // acquire bus + route to SPI (per frame)
void dp_qspi_end(void);          // route back to GPIO + release bus
void dp_qspi_set_niborder(bool high_first);
bool dp_qspi_get_niborder(void);
// Shift one 800-bit slot on all three colors and latch it. Pins must be
// routed to SPI (dp_qspi_route_spi) around a run of these.
bool dp_qspi_send_slot(const uint16_t *r, const uint16_t *g, const uint16_t *b);
