// Interactive console (UART0 / the CoreBoard's USB-UART port).
// Run `pio device monitor` and type `help`. The web UI's terminal runs the same
// commands through console_exec().
#include "console.h"
#include "datapath.h"
#include "datapath_qspi.h"
#include "scanout.h"
#include "protocol.h"
#include "framebuf.h"
#include "patterns.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdarg.h>
#include "scanout_par.h"
#include "render.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "beatsync.h"

// Output capture: when a capture buffer is armed (web terminal), command
// output goes there instead of the serial console. Every printf below this
// point is redirected through cprintf.
static char *s_cap;
static size_t s_cap_len, s_cap_pos;

static int cprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n;
    if (s_cap && s_cap_pos < s_cap_len) {
        n = vsnprintf(s_cap + s_cap_pos, s_cap_len - s_cap_pos, fmt, ap);
        if (n > 0) s_cap_pos += (size_t)n < s_cap_len - s_cap_pos ? (size_t)n
                                                                 : s_cap_len - s_cap_pos - 1;
    } else {
        n = vprintf(fmt, ap);
    }
    va_end(ap);
    return n;
}
#define printf cprintf

// Run one command line, capturing its output. Used by the web UI terminal.
int console_exec(const char *line, char *out, size_t out_len)
{
    if (out && out_len) { out[0] = 0; s_cap = out; s_cap_len = out_len; s_cap_pos = 0; }
    int ret = 0;
    esp_err_t err = esp_console_run(line, &ret);
    if (err == ESP_ERR_NOT_FOUND) cprintf("unknown command: %s\n", line);
    else if (err == ESP_ERR_INVALID_ARG) cprintf("empty command\n");
    else if (err != ESP_OK) cprintf("error: %s\n", esp_err_to_name(err));
    s_cap = NULL; s_cap_len = s_cap_pos = 0;
    return ret;
}

static uint16_t hx(const char *s) { return (uint16_t)strtoul(s, NULL, 16); }

static void send_now(void)
{
    if (!proto_auto_running()) proto_send_frame();
}

// ---- scan ----------------------------------------------------------------
static int cmd_scan(int argc, char **argv)
{
    if (argc < 2) { printf("scan start|stop|backend timer|parlio|hold <0-7|off>|burst <n>|speed <nops>|addr <0-7>\n"); return 0; }
    if (!strcmp(argv[1], "start")) { scanout_start(); printf("scan running\n"); }
    else if (!strcmp(argv[1], "stop")) { scanout_stop(); printf("scan stopped\n"); }
    else if (!strcmp(argv[1], "backend") && argc > 2) {
        int be = strcmp(argv[2], "timer") ? SCAN_BACKEND_PARLIO : SCAN_BACKEND_TIMER;
        proto_lock();
        bool ok = scanout_set_backend(be);
        proto_unlock();
        printf(ok ? "backend=%s (restart scan)\n" : "backend switch FAILED (%s)\n", argv[2]);
    }
    else if (!strcmp(argv[1], "hold") && argc > 2) {
        scanout_set_hold(strcmp(argv[2], "off") ? atoi(argv[2]) : -1);
        printf("hold=%d\n", scanout_get_hold());
    }
    else if (!strcmp(argv[1], "burst") && argc > 2) { scanout_set_burst(atoi(argv[2])); }
    else if (!strcmp(argv[1], "speed") && argc > 2) { scanout_set_halfperiod(atoi(argv[2])); }
    else if (!strcmp(argv[1], "addr") && argc > 2)  { scanout_set_addr(atoi(argv[2])); }
    return 0;
}

// ---- protocol ------------------------------------------------------------
static int cmd_boot(int argc, char **argv) { proto_boot(); printf("boot config sent (code 4, 0x%04X)\n", g_proto.cfg_a); return 0; }
static int cmd_vsync(int argc, char **argv) { proto_vsync_only(); return 0; }

static int cmd_preamble(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "on"))  { g_proto.preamble_each_frame = true;  return 0; }
    if (argc > 1 && !strcmp(argv[1], "off")) { g_proto.preamble_each_frame = false; return 0; }
    proto_preamble();
    printf("preamble sent\n");
    return 0;
}

static int cmd_cfg(int argc, char **argv)
{
    if (argc == 3) { dp_config_write(hx(argv[2]), hx(argv[2]), hx(argv[2]), atoi(argv[1])); return 0; }
    if (argc == 5) { dp_config_write(hx(argv[2]), hx(argv[3]), hx(argv[4]), atoi(argv[1])); return 0; }
    printf("cfg <code> <hex16> | cfg <code> <hexR> <hexG> <hexB>\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("set nlat <n> | latchcode <0|1> | gclkstop <0|1> | dspeed <nops> | cfga <hex> | cfgb <hexR> <hexG> <hexB>\n");
        return 0;
    }
    if (!strcmp(argv[1], "nlat"))      g_proto.n_data_latches = atoi(argv[2]);
    else if (!strcmp(argv[1], "latchcode")) g_proto.data_latch_code = atoi(argv[2]);
    else if (!strcmp(argv[1], "gclkstop"))  g_proto.stop_gclk_in_preamble = atoi(argv[2]);
    else if (!strcmp(argv[1], "dspeed"))    dp_set_halfperiod(atoi(argv[2]));
    else if (!strcmp(argv[1], "cfga"))      g_proto.cfg_a = hx(argv[2]);
    else if (!strcmp(argv[1], "cfgb") && argc == 5) {
        g_proto.cfg_b_r = hx(argv[2]); g_proto.cfg_b_g = hx(argv[3]); g_proto.cfg_b_b = hx(argv[4]);
    }
    else printf("unknown setting\n");
    return 0;
}

// ---- raw slot editing (discovery core) -----------------------------------
static int cmd_raw(int argc, char **argv)
{
    if (argc < 2) {
        printf("raw clear | raw fill <hR> <hG> <hB> | raw slot <s> <hR> <hG> <hB> | raw word <s> <w> <hR> <hG> <hB>\n");
        return 0;
    }
    if (!strcmp(argv[1], "clear")) raw_clear();
    else if (!strcmp(argv[1], "fill") && argc == 5)
        raw_fill(hx(argv[2]), hx(argv[3]), hx(argv[4]));
    else if (!strcmp(argv[1], "slot") && argc == 6)
        raw_fill_slot(atoi(argv[2]), hx(argv[3]), hx(argv[4]), hx(argv[5]));
    else if (!strcmp(argv[1], "word") && argc == 7)
        raw_set_word(atoi(argv[2]), atoi(argv[3]), hx(argv[4]), hx(argv[5]), hx(argv[6]));
    else { printf("bad args\n"); return 0; }
    send_now();
    return 0;
}

// walk helpers: one command per step so the serial prompt stays responsive
static int cmd_ws(int argc, char **argv)   // walk slot: light slot i everywhere
{
    if (argc < 2) { printf("ws <slot 0-127> — lights one SRAM slot (one row+channel) full white\n"); return 0; }
    int s = atoi(argv[1]);
    raw_clear();
    raw_fill_slot(s, 0xFFFF, 0xFFFF, 0xFFFF);
    send_now();
    printf("slot %d lit: note which panel row(s)+column-stripe pattern shows\n", s);
    return 0;
}

static int cmd_ww(int argc, char **argv)   // walk word: light word w in all slots
{
    if (argc < 2) { printf("ww <word 0-49> — lights one chain driver in every slot\n"); return 0; }
    int w = atoi(argv[1]);
    raw_clear();
    for (int s = 0; s < SRAM_SLOTS; s++)
        raw_set_word(s, w, 0xFFFF, 0xFFFF, 0xFFFF);
    send_now();
    printf("word %d lit: shows the physical extent of one driver (16 channels x all rows)\n", w);
    return 0;
}

static int cmd_wb(int argc, char **argv)   // single (slot, word) = one pixel per color line
{
    if (argc < 3) { printf("wb <slot> <word> [hexval=ffff] [r|g|b|w] — one pixel\n"); return 0; }
    uint16_t v = (argc > 3) ? hx(argv[3]) : 0xFFFF;
    char col = (argc > 4) ? argv[4][0] : 'w';
    raw_clear();
    raw_set_word(atoi(argv[1]), atoi(argv[2]),
                 (col == 'r' || col == 'w') ? v : 0,
                 (col == 'g' || col == 'w') ? v : 0,
                 (col == 'b' || col == 'w') ? v : 0);
    send_now();
    return 0;
}

// ---- framebuffer patterns ------------------------------------------------
static int cmd_pat(int argc, char **argv)
{
    if (argc < 2) {
        printf("pat solid <hR> <hG> <hB> | rows | cols | gradx | checker [cell] | border | pixel <x> <y>\n");
        return 0;
    }
    if (!strcmp(argv[1], "solid") && argc == 5) fbpat_solid(hx(argv[2]), hx(argv[3]), hx(argv[4]));
    else if (!strcmp(argv[1], "rows"))    fbpat_rows();
    else if (!strcmp(argv[1], "cols"))    fbpat_cols();
    else if (!strcmp(argv[1], "gradx"))   fbpat_grad_x();
    else if (!strcmp(argv[1], "checker")) fbpat_checker(argc > 2 ? atoi(argv[2]) : 4);
    else if (!strcmp(argv[1], "border"))  fbpat_border();
    else if (!strcmp(argv[1], "pixel") && argc >= 4)
        fbpat_pixel(atoi(argv[2]), atoi(argv[3]), 0xFFFF, 0xFFFF, 0xFFFF);
    else { printf("bad args\n"); return 0; }
    fb_pack();
    send_now();
    return 0;
}

// ---- mapping -------------------------------------------------------------
static int cmd_map(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "set")) {
        if (!map_set(argv[2], atoi(argv[3]))) printf("unknown key\n");
        else { fb_pack(); send_now(); }
    } else if (argc >= 4 && !strcmp(argv[1], "rf")) {
        if (!map_set_rf(atoi(argv[2]), atoi(argv[3]))) printf("bad rf\n");
        else { fb_pack(); send_now(); }
    } else if (argc >= 2 && !strcmp(argv[1], "reset")) {
        map_default(); fb_pack(); send_now();
    }
    map_print();
    return 0;
}

// ---- streaming / status --------------------------------------------------
static int cmd_auto(int argc, char **argv)
{
    if (argc < 2) { printf("auto <fps> | auto off\n"); return 0; }
    if (!strcmp(argv[1], "off")) { proto_auto_stop(); printf("auto off\n"); }
    else { proto_auto_start(atoi(argv[1])); printf("streaming at %s fps\n", argv[1]); }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    printf("scan: %s hold=%d burst=%lu\n", scanout_running() ? "RUN" : "stopped",
           scanout_get_hold(), (unsigned long)scanout_get_burst());
    printf("proto: nlat=%d latchcode=%d preamble=%d gclkstop=%d cfga=%04X cfgb=%04X/%04X/%04X\n",
           g_proto.n_data_latches, g_proto.data_latch_code,
           g_proto.preamble_each_frame, g_proto.stop_gclk_in_preamble,
           g_proto.cfg_a, g_proto.cfg_b_r, g_proto.cfg_b_g, g_proto.cfg_b_b);
    printf("datapath halfperiod nops=%lu, auto=%s\n",
           (unsigned long)dp_get_halfperiod(), proto_auto_running() ? "on" : "off");
    map_print();
    return 0;
}

// ---- overclocking / timing ----------------------------------------------
// The S31's PARLIO sample clock is 160 MHz / n, so GCLK snaps to the nearest
// of 40, 26.7, 20, 16, 13.3, 11.4, 10 ... MHz. The printout shows what you got.
static int cmd_oc(int argc, char **argv)
{
    scan_tuning_t t = g_tune;
    proto_lock();
    if (argc >= 2 && strcmp(argv[1], "status")) {
        bool scan_change = true;
        if (!strcmp(argv[1], "gclk") && argc >= 3)       t.pclk_hz = (uint32_t)(atof(argv[2]) * 2e6);
        else if (!strcmp(argv[1], "pclk") && argc >= 3)  t.pclk_hz = (uint32_t)(atof(argv[2]) * 1e6);
        else if (!strcmp(argv[1], "burst") && argc >= 3) t.burst = atoi(argv[2]);
        else if (!strcmp(argv[1], "pad") && argc >= 3)   t.pad = atoi(argv[2]);
        else if (!strcmp(argv[1], "gaps") && argc >= 4) {
            t.gap1 = atoi(argv[2]); t.gap2 = atoi(argv[3]);
        } else if (!strcmp(argv[1], "data") && argc >= 3) {
            int hz = (int)(atof(argv[2]) * 1e6);
            printf(dp_qspi_set_clock(hz) ? "data clock -> %.2f MHz\n"
                                         : "data clock rejected (%.2f MHz)\n",
                   dp_qspi_get_clock() / 1e6);
            scan_change = false;
        } else if (!strcmp(argv[1], "reset")) {
            // OEM-like: 12.5 MHz data; GCLK 12.5 snaps to 13.3 MHz on the S31
            t = (scan_tuning_t){ .pclk_hz = 25000000, .burst = 1041,
                                 .gap1 = 40, .gap2 = 30, .pad = 3 };
            dp_qspi_set_clock(12500000);
        } else if (!strcmp(argv[1], "fast")) {
            t.pclk_hz = 40000000;                 // GCLK 20 MHz
            dp_qspi_set_clock(20000000);
        } else { printf("oc gclk|pclk|burst|pad|gaps|data|fast|reset|status\n"); proto_unlock(); return 0; }
        if (scan_change) {
            if (scanout_get_backend() != SCAN_BACKEND_PARLIO) {
                printf("scan timing only applies to the PARLIO backend ('scan backend parlio')\n");
            } else {
                t.pclk_hz = scanout_par_quantize_pclk(t.pclk_hz);
                if (!scanout_par_set_tuning(&t)) printf("rejected (out of range / alloc failed)\n");
            }
        }
    }
    // remember for `save` so the tuning survives a reboot
    g_set.tune_pclk_hz = g_tune.pclk_hz;
    g_set.tune_data_hz = dp_qspi_get_clock();
    g_set.tune_burst = (uint16_t)g_tune.burst;
    g_set.tune_pad = (uint8_t)g_tune.pad;
    proto_unlock();
    printf("GCLK %.3f MHz (pclk %.3f)  burst %d  gaps %d/%d  pad %d\n",
           g_tune.pclk_hz / 2e6, g_tune.pclk_hz / 1e6, g_tune.burst,
           g_tune.gap1, g_tune.gap2, g_tune.pad);
    printf("panel refresh %.1f Hz   data clock %.2f MHz\n",
           scanout_par_refresh_hz(), dp_qspi_get_clock() / 1e6);
    printf("(drivers rated 30 MHz; the panel measured stable to ~19.5-20 MHz with\n"
           " the S3 controller. 'oc fast' = 20 MHz both, 'oc reset' = OEM-like.)\n");
    return 0;
}

// ---- UI render engine ----------------------------------------------------
static int cmd_ui(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "on"))  { render_start(); printf("UI render on\n"); }
    else if (argc > 1 && !strcmp(argv[1], "off")) { render_stop(); printf("UI render off (manual control)\n"); }
    else printf("UI render is %s\n", render_active() ? "on" : "off");
    return 0;
}

// ---- wifi ----------------------------------------------------------------
static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "status")) {
        printf("connecting to '%s'...\n", argv[1]);
        wifi_set_credentials(argv[1], argv[2]);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "forget")) { wifi_forget(); printf("credentials cleared\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "txpower")) {
        if (argc > 2) {
            int dbm = atoi(argv[2]);
            g_set.wifi_txpower = (uint8_t)(dbm * 4);   // esp_wifi uses 0.25 dBm units
            wifi_set_txpower(g_set.wifi_txpower);
        }
        printf("wifi tx power: %d dBm  (lower = smaller current spikes; try 8 or 5\n"
               " if transmissions visibly pulse the panel)\n", wifi_get_txpower() / 4);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status")) {   // open network
        printf("connecting to '%s' (no password)...\n", argv[1]);
        wifi_set_credentials(argv[1], "");
        return 0;
    }
    printf("wifi: %s  ssid='%s'  ip=%s  rssi=%d\n",
           wifi_is_ap_mode() ? "SETUP AP" : (wifi_is_connected() ? "connected" : "connecting"),
           wifi_ssid_str(), wifi_ip_str(), wifi_rssi());
    printf("web UI: http://%s/\n", wifi_ip_str());
    printf("usage: wifi <ssid> <password> | wifi <ssid> | wifi forget | wifi status\n");
    return 0;
}

// ---- DJ beat sync ----------------------------------------------------------
static int cmd_beat(int argc, char **argv)
{
    if (argc >= 3) {
        int v = atoi(argv[2]);
        if (!strcmp(argv[1], "on"))          g_beat.enabled = true;
        else if (!strcmp(argv[1], "offset")) g_beat.offset_ms = (int16_t)v;
        else if (!strcmp(argv[1], "pulse"))  g_beat.pulse = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
        else if (!strcmp(argv[1], "bars"))   g_beat.auto_bars = (uint8_t)(v < 0 ? 0 : v);
        else if (!strcmp(argv[1], "tempo"))  g_beat.tempo_lock = v != 0;
        else if (!strcmp(argv[1], "deck"))   g_beat.on_deck = v != 0;
    } else if (argc == 2 && !strcmp(argv[1], "off")) g_beat.enabled = false;
    else if (argc == 2 && !strcmp(argv[1], "on"))    g_beat.enabled = true;
    beat_status_t bs;
    beatsync_status(&bs);
    printf("beat sync %s: %s  bpm %.2f  deck %d  last report %ld ms ago  "
           "phase err %+.1f ms  reports %u\n",
           g_beat.enabled ? "on" : "OFF", bs.synced ? "SYNCED" : "no beat",
           bs.bpm, bs.deck, (long)bs.age_ms, bs.err_ms, (unsigned)bs.packets);
    printf("offset %d ms  pulse %d%%  auto-change every %d bars (%s)  on deck change %d  tempo lock %d\n",
           g_beat.offset_ms, g_beat.pulse, g_beat.auto_bars,
           g_beat.auto_pool ? "all" : "beat animations", g_beat.on_deck, g_beat.tempo_lock);
    printf("usage: beat on|off | beat offset <ms> | pulse <0-100> | bars <n> | tempo 0|1 | deck 0|1\n");
    return 0;
}

static int cmd_pintest(int argc, char **argv)
{
    dp_pin_test(argc > 1 ? atoi(argv[1]) : 10);
    return 0;
}

static int cmd_send(int argc, char **argv) { proto_send_frame(); return 0; }

static int cmd_data(int argc, char **argv)
{
    if (argc < 2) { printf("data backend bitbang|qspi | data niborder 0|1\n"); return 0; }
    if (!strcmp(argv[1], "backend") && argc > 2) {
        proto_lock();
        if (!strcmp(argv[2], "qspi")) {
            if (!dp_qspi_init()) { printf("qspi init FAILED\n"); proto_unlock(); return 0; }
            g_data_backend = DATA_BACKEND_QSPI;
        } else {
            g_data_backend = DATA_BACKEND_BITBANG;
        }
        proto_unlock();
        printf("data backend=%s\n", g_data_backend ? "qspi" : "bitbang");
    } else if (!strcmp(argv[1], "niborder") && argc > 2) {
        dp_qspi_set_niborder(atoi(argv[2]) != 0);
        send_now();
    }
    return 0;
}

// --------------------------------------------------------------------------
static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = name, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "cos102>";
    repl_cfg.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));
#else
    // Default on the S31 CoreBoard: UART0 (GPIO58/59) -> USB-to-UART port.
    esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &repl));
#endif

    reg("scan",    "display scan: start|stop|backend parlio|timer|hold|burst|speed|addr", cmd_scan);
    reg("boot",    "send boot config write (code 4)", cmd_boot);
    reg("vsync",   "send vsync (code 3)", cmd_vsync);
    reg("preamble","send frame preamble now, or 'preamble on|off' per-frame", cmd_preamble);
    reg("cfg",     "raw config write: cfg <code> <hex> [hexG hexB]", cmd_cfg);
    reg("set",     "settings: nlat|latchcode|gclkstop|dspeed|cfga|cfgb", cmd_set);
    reg("raw",     "raw slot buffer: clear|fill|slot|word", cmd_raw);
    reg("ws",      "walk slot: light one SRAM slot", cmd_ws);
    reg("ww",      "walk word: light one chain driver", cmd_ww);
    reg("wb",      "one (slot,word) pixel: wb <slot> <word> [hex] [r|g|b|w]", cmd_wb);
    reg("pat",     "framebuffer patterns (uses mapping)", cmd_pat);
    reg("map",     "mapping: map show|reset|set <key> <v>|rf <ch> <row>", cmd_map);
    reg("auto",    "continuous frame streaming: auto <fps>|off", cmd_auto);
    reg("send",    "send one full frame now", cmd_send);
    reg("data",    "data path: data backend bitbang|qspi, niborder 0|1", cmd_data);
    reg("status",  "show all state", cmd_status);
    reg("pintest", "slow-toggle each data pin for probing", cmd_pintest);
    reg("oc",      "overclock/timing: oc gclk|data <MHz>|burst|gaps|pad|reset|status", cmd_oc);
    reg("ui",      "UI render engine: ui on|off", cmd_ui);
    reg("wifi",    "wifi <ssid> <password> | wifi forget | wifi status", cmd_wifi);
    reg("beat",    "DJ beat sync status/settings (Mixxx bridge, UDP 4210)", cmd_beat);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
