#include "patterns.h"
#include "protocol.h"
#include "framebuf.h"
#include <string.h>

void raw_clear(void) { memset(g_slots, 0, sizeof(g_slots)); }

void raw_fill(uint16_t r, uint16_t g, uint16_t b)
{
    for (int s = 0; s < SRAM_SLOTS; s++)
        raw_fill_slot(s, r, g, b);
}

void raw_fill_slot(int slot, uint16_t r, uint16_t g, uint16_t b)
{
    if (slot < 0 || slot >= SRAM_SLOTS) return;
    for (int w = 0; w < WORDS_PER_SHIFT; w++) {
        g_slots[slot].r[w] = r;
        g_slots[slot].g[w] = g;
        g_slots[slot].b[w] = b;
    }
}

void raw_set_word(int slot, int word, uint16_t r, uint16_t g, uint16_t b)
{
    if (slot < 0 || slot >= SRAM_SLOTS) return;
    if (word < 0 || word >= WORDS_PER_SHIFT) return;
    g_slots[slot].r[word] = r;
    g_slots[slot].g[word] = g;
    g_slots[slot].b[word] = b;
}

void fbpat_solid(uint16_t r, uint16_t g, uint16_t b)
{
    for (int y = 0; y < PANEL_H; y++)
        for (int x = 0; x < PANEL_W; x++)
            fb_set(x, y, r, g, b);
}

void fbpat_rows(void)
{
    fb_clear();
    for (int y = 0; y < PANEL_H; y++) {
        uint16_t v = (uint16_t)((y + 1) * 65535 / PANEL_H);
        for (int x = 0; x < PANEL_W; x++) fb_set(x, y, v, v, v);
    }
}

void fbpat_cols(void)
{
    fb_clear();
    for (int x = 0; x < PANEL_W; x++) {
        // color-code the column mod 4 so direction and order are visible
        uint16_t r = (x % 4 == 0) ? 0xFFFF : 0;
        uint16_t g = (x % 4 == 1) ? 0xFFFF : 0;
        uint16_t b = (x % 4 == 2) ? 0xFFFF : 0;
        if (x % 4 == 3) { r = g = b = 0xFFFF; }
        for (int y = 0; y < PANEL_H; y++) fb_set(x, y, r, g, b);
    }
}

void fbpat_grad_x(void)
{
    for (int y = 0; y < PANEL_H; y++)
        for (int x = 0; x < PANEL_W; x++) {
            uint16_t v = (uint16_t)(x * 65535 / (PANEL_W - 1));
            fb_set(x, y, v, v, v);
        }
}

void fbpat_checker(int cell)
{
    if (cell < 1) cell = 4;
    for (int y = 0; y < PANEL_H; y++)
        for (int x = 0; x < PANEL_W; x++) {
            int on = ((x / cell) + (y / cell)) & 1;
            fb_set(x, y, on ? 0xFFFF : 0, on ? 0xFFFF : 0, on ? 0xFFFF : 0);
        }
}

void fbpat_border(void)
{
    fb_clear();
    for (int x = 0; x < PANEL_W; x++) {
        fb_set(x, 0, 0xFFFF, 0, 0);            // top row red
        fb_set(x, PANEL_H - 1, 0, 0, 0xFFFF);  // bottom row blue
    }
    for (int y = 0; y < PANEL_H; y++) {
        fb_set(0, y, 0, 0xFFFF, 0);            // left col green
        fb_set(PANEL_W - 1, y, 0xFFFF, 0xFFFF, 0xFFFF); // right col white
    }
}

void fbpat_pixel(int x, int y, uint16_t r, uint16_t g, uint16_t b)
{
    fb_clear();
    fb_set(x, y, r, g, b);
}
