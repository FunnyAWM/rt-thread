/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board bring-up self test for the ESP32-C3 BSP.
 *
 * Exercises the features the BSP README claims to support:
 *   GPIO, UART, I2C, SPI, CLOCK_TIMER, WIFI, BLE
 * and reports the ones that need extra configuration or hardware
 * (GDBStub, JTAG).
 *
 * Usage from the msh prompt:
 *   bsp_test              run every test
 *   bsp_test wifi         run a single test
 *   bsp_test list         list the test names
 *
 * Where a test talks to a peripheral that is not wired, it reports SKIP
 * instead of failing, so the summary reflects "what this board can do".
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>

#ifdef BSP_USING_BLE
/* NimBLE host, supplied by the ESP-IDF package's bt component. The host is
 * brought up here rather than in main() so `bsp_test ble` can scan on demand;
 * main.c still calls esp_timer_init() before app_main(), which NimBLE needs. */
#include "esp_err.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#endif /* BSP_USING_BLE */

#define TEST_GPIO_PIN       3       /* free pin, only driven during the test */
#define TEST_SPI_CS_PIN     10      /* free pin, used as CS by the SPI test */
#define TEST_I2C_BUS        "swi2c0"   /* README recommends software I2C */
#define TEST_SPI_BUS        "spi2"
#define TEST_TIMER_DEV      "timer0"
#define TEST_UART_DEV       "uart"
#define TEST_TIMER_CYCLES   1000    /* 1 kHz period on a 1 MHz timer */
#define TEST_TIMER_WINDOW   100     /* ms; expect ~100 interrupts at 1 kHz */

#define TEST_WIFI_LIST_MAX  5       /* APs printed per scan */
#define TEST_BLE_SYNC_WAIT  5000    /* ms to wait for the host to sync */
#define TEST_BLE_SCAN_MS    3000    /* active discovery duration */

#define RESULT_PASS         0
#define RESULT_FAIL         1
#define RESULT_SKIP         2

struct bsp_test
{
    const char *name;
    int (*run)(void);
};

static void test_log(const char *name, int result, const char *fmt, ...)
{
    char buffer[128];
    va_list args;
    const char *tag;

    va_start(args, fmt);
    rt_vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    switch (result)
    {
    case RESULT_PASS:
        tag = "[PASS]";
        break;
    case RESULT_SKIP:
        tag = "[SKIP]";
        break;
    default:
        tag = "[FAIL]";
        break;
    }

    rt_kprintf("  %s %-6s %s\n", tag, name, buffer);
}

/* ---------------------------------------------------------------- GPIO ---- */

static int test_gpio(void)
{
#if defined(RT_USING_PIN) && defined(BSP_USING_GPIO)
    rt_int32_t level;
    int result = RESULT_PASS;

    rt_pin_mode(TEST_GPIO_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(TEST_GPIO_PIN, PIN_HIGH);
    rt_thread_mdelay(1);
    rt_pin_write(TEST_GPIO_PIN, PIN_LOW);
    rt_thread_mdelay(1);

    /* The output-only path cannot be read back on ESP-IDF, so check the
     * input path with the internal pull resistors instead. */
    rt_pin_mode(TEST_GPIO_PIN, PIN_MODE_INPUT_PULLUP);
    rt_thread_mdelay(1);
    level = rt_pin_read(TEST_GPIO_PIN);
    if (level != PIN_HIGH)
    {
        test_log("gpio", RESULT_FAIL, "pull-up read %d, expect 1", level);
        result = RESULT_FAIL;
    }

    rt_pin_mode(TEST_GPIO_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(1);
    level = rt_pin_read(TEST_GPIO_PIN);
    if (level != PIN_LOW)
    {
        test_log("gpio", RESULT_FAIL, "pull-down read %d, expect 0", level);
        result = RESULT_FAIL;
    }

    rt_pin_mode(TEST_GPIO_PIN, PIN_MODE_INPUT);

    if (result == RESULT_PASS)
    {
        test_log("gpio", RESULT_PASS, "pin %d output/pull-up/pull-down ok",
                 TEST_GPIO_PIN);
    }
    return result;
#else
    test_log("gpio", RESULT_SKIP, "GPIO driver disabled");
    return RESULT_SKIP;
#endif
}

/* ---------------------------------------------------------------- UART ---- */

static int test_uart(void)
{
#if defined(RT_USING_SERIAL) && defined(BSP_USING_UART)
    rt_device_t dev;
    struct rt_serial_device *serial;
    rt_err_t err;

    dev = rt_device_find(TEST_UART_DEV);
    if (dev == RT_NULL)
    {
        test_log("uart", RESULT_FAIL, "device '%s' not found", TEST_UART_DEV);
        return RESULT_FAIL;
    }

    err = rt_device_open(dev, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX);
    if (err != RT_EOK)
    {
        test_log("uart", RESULT_FAIL, "open failed (%d)", err);
        return RESULT_FAIL;
    }

    serial = (struct rt_serial_device *)dev;
    test_log("uart", RESULT_PASS, "open ok, %d baud, %d bit, bufsz %d",
             serial->config.baud_rate, serial->config.data_bits,
             serial->config.bufsz);

    /* The console is the same device; close only drops the reference taken
     * above so the shell keeps working. */
    rt_device_close(dev);
    return RESULT_PASS;
#else
    test_log("uart", RESULT_SKIP, "UART driver disabled");
    return RESULT_SKIP;
#endif
}

/* ----------------------------------------------------------------- I2C ---- */

static int test_i2c(void)
{
#if defined(RT_USING_I2C) && defined(BSP_USING_I2C)
    struct rt_i2c_bus_device *bus;
    rt_uint8_t probe = 0;
    int found = 0;
    int addr;

    bus = rt_i2c_bus_device_find(TEST_I2C_BUS);
    if (bus == RT_NULL)
    {
        /*
         * The hardware bus ("i2c0") is unusable with this ESP-IDF fork: its
         * i2c ISR calls xQueueOverwriteFromISR(), which the RT-Thread FreeRTOS
         * wrapper does not implement, so the first transfer trips a
         * configASSERT.  Only probe the software bus.
         */
        if (rt_i2c_bus_device_find("i2c0") != RT_NULL)
        {
            test_log("i2c", RESULT_FAIL,
                     "hw bus 'i2c0' unusable: IDF i2c ISR needs "
                     "xQueueOverwriteFromISR (wrapper unimplemented)");
            return RESULT_FAIL;
        }
        test_log("i2c", RESULT_SKIP, "bus '%s' not registered", TEST_I2C_BUS);
        return RESULT_SKIP;
    }

    for (addr = 0x08; addr <= 0x77; addr++)
    {
        struct rt_i2c_msg msg;

        msg.addr  = (rt_uint16_t)addr;
        msg.flags = RT_I2C_WR;
        msg.len   = 1;
        msg.buf   = &probe;

        if (rt_i2c_transfer(bus, &msg, 1) == 1)
        {
            if (found < 8)
            {
                rt_kprintf("        i2c device found at 0x%02x\n", addr);
            }
            found++;
        }
    }

    /* An open bus with no pull-ups floats high and ACKs every address. */
    if (found == 0x77 - 0x08 + 1)
    {
        test_log("i2c", RESULT_SKIP,
                 "%s replies to every address, bus is floating "
                 "(add pull-ups / a device)", TEST_I2C_BUS);
        return RESULT_SKIP;
    }

    test_log("i2c", RESULT_PASS, "%s scan ok, %d device(s) replied",
             TEST_I2C_BUS, found);
    return RESULT_PASS;
#else
    test_log("i2c", RESULT_SKIP, "I2C driver disabled");
    return RESULT_SKIP;
#endif
}

/* ----------------------------------------------------------------- SPI ---- */

static int test_spi(void)
{
#if defined(RT_USING_SPI) && defined(BSP_USING_SPI)
    struct rt_spi_device *spi;
    struct rt_spi_configuration cfg;
    rt_uint8_t tx[4] = { 0xAA, 0x55, 0x00, 0xFF };
    rt_uint8_t rx[4] = { 0 };
    rt_ssize_t transferred;
    rt_device_t bus;

    bus = rt_device_find(TEST_SPI_BUS);
    if (bus == RT_NULL)
    {
        test_log("spi", RESULT_SKIP, "bus '%s' not registered", TEST_SPI_BUS);
        return RESULT_SKIP;
    }

    if (rt_device_find("spi_test") == RT_NULL)
    {
        extern rt_err_t rt_hw_spi_device_attach(const char *bus_name,
                                                const char *device_name,
                                                rt_base_t cs_pin);
        if (rt_hw_spi_device_attach(TEST_SPI_BUS, "spi_test",
                                    TEST_SPI_CS_PIN) != RT_EOK)
        {
            test_log("spi", RESULT_FAIL, "attach spi_test to %s failed",
                     TEST_SPI_BUS);
            return RESULT_FAIL;
        }
    }

    spi = (struct rt_spi_device *)rt_device_find("spi_test");
    if (spi == RT_NULL)
    {
        test_log("spi", RESULT_FAIL, "spi_test device missing");
        return RESULT_FAIL;
    }

    rt_memset(&cfg, 0, sizeof(cfg));
    cfg.data_width = 8;
    cfg.mode       = RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz     = 1000 * 1000;

    if (rt_spi_configure(spi, &cfg) != RT_EOK)
    {
        test_log("spi", RESULT_FAIL, "configure failed");
        return RESULT_FAIL;
    }

    transferred = rt_spi_transfer(spi, tx, rx, sizeof(tx));
    if (transferred != (rt_ssize_t)sizeof(tx))
    {
        test_log("spi", RESULT_FAIL, "transfer returned %d, expect %d",
                 (int)transferred, (int)sizeof(tx));
        return RESULT_FAIL;
    }

    test_log("spi", RESULT_PASS,
             "%s 8bit mode0 1MHz, %d bytes clocked (CS=GPIO%d)",
             TEST_SPI_BUS, (int)transferred, TEST_SPI_CS_PIN);
    return RESULT_PASS;
#else
    test_log("spi", RESULT_SKIP, "SPI driver disabled");
    return RESULT_SKIP;
#endif
}

/* ---------------------------------------------------------------- WiFi ---- */

static int test_wifi(void)
{
#if defined(RT_USING_WIFI) && defined(BSP_USING_WIFI) && defined(RT_WLAN_MANAGE_ENABLE)
    struct rt_wlan_scan_result *result;
    rt_err_t err;
    int i;

    /* drv_wifi.c registers both station and AP; the station role drives
     * the scan, and setting it up front makes `wifi scan` work afterwards. */
    err = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    if (err != RT_EOK)
    {
        test_log("wifi", RESULT_FAIL, "station mode on '%s' failed (%d)",
                 RT_WLAN_DEVICE_STA_NAME, err);
        return RESULT_FAIL;
    }

    rt_kprintf("        scanning, this blocks for a few seconds ...\n");

    /* Waits for RT_WLAN_EVT_SCAN_DONE internally; the ESP driver reports
     * each AP from its WIFI_EVENT_SCAN_DONE handler. */
    result = rt_wlan_scan_sync();
    if (result == RT_NULL)
    {
        test_log("wifi", RESULT_FAIL, "scan produced no result "
                 "(WIFI_EVENT_SCAN_DONE never arrived)");
        return RESULT_FAIL;
    }

    for (i = 0; i < result->num && i < TEST_WIFI_LIST_MAX; i++)
    {
        const struct rt_wlan_info *info = &result->info[i];
        char ssid[RT_WLAN_SSID_MAX_LENGTH + 1];
        rt_uint8_t len = info->ssid.len;

        if (len > RT_WLAN_SSID_MAX_LENGTH)
        {
            len = RT_WLAN_SSID_MAX_LENGTH;
        }
        rt_memcpy(ssid, info->ssid.val, len);
        ssid[len] = '\0';

        rt_kprintf("        ch %2d  %4d dBm  %s%s\n",
                   (int)info->channel, (int)info->rssi, ssid,
                   info->hidden ? " (hidden)" : "");
    }

    if (result->num == 0)
    {
        test_log("wifi", RESULT_SKIP, "scan ok, no AP in range");
        return RESULT_SKIP;
    }

    test_log("wifi", RESULT_PASS, "station up, %d AP(s) in range",
             (int)result->num);
    return RESULT_PASS;
#elif defined(RT_USING_WIFI) && defined(BSP_USING_WIFI)
    /* RT_WLAN_MANAGE_ENABLE adds the scan API; without it only the driver
     * registration can be checked. */
    if (rt_device_find(RT_WLAN_DEVICE_STA_NAME) == RT_NULL)
    {
        test_log("wifi", RESULT_FAIL, "no '%s' device registered",
                 RT_WLAN_DEVICE_STA_NAME);
        return RESULT_FAIL;
    }

    test_log("wifi", RESULT_PASS, "driver registered, connection management "
             "off (RT_WLAN_MANAGE_ENABLE) so no scan");
    return RESULT_PASS;
#elif defined(RT_USING_WIFI)
    test_log("wifi", RESULT_SKIP, "Wi-Fi framework on but no driver "
             "(menuconfig: BSP_USING_WIFI)");
    return RESULT_SKIP;
#else
    test_log("wifi", RESULT_SKIP, "Wi-Fi disabled (menuconfig: BSP_USING_WIFI)");
    return RESULT_SKIP;
#endif
}

/* ----------------------------------------------------------------- BLE ---- */

#ifdef BSP_USING_BLE
static struct rt_semaphore ble_sync_sem;
static struct rt_semaphore ble_disc_sem;
static rt_uint8_t   ble_own_addr_type;
static rt_uint32_t  ble_report_count;
static rt_bool_t    ble_sem_ready;
static rt_bool_t    ble_host_started;

static void ble_host_task(void *param)
{
    (void)param;

    /* Returns once nimble_port_stop() is called, which nothing here does:
     * the host stays up so a second `bsp_test ble` reuses it. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_reset(int reason)
{
    rt_kprintf("        ble host reset, reason %d\n", reason);
}

static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &ble_own_addr_type);
    rt_sem_release(&ble_sync_sem);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_DISC)
    {
        const struct ble_gap_disc_desc *disc = &event->disc;

        if (ble_report_count == 0)
        {
            rt_kprintf("        first report %02x:%02x:%02x:%02x:%02x:%02x "
                       "type 0x%02x rssi %d\n",
                       disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
                       disc->addr.val[2], disc->addr.val[1], disc->addr.val[0],
                       disc->event_type, (int)disc->rssi);
        }
        ble_report_count++;
    }
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
    {
        rt_sem_release(&ble_disc_sem);
    }

    return 0;
}
#endif /* BSP_USING_BLE */

static int test_ble(void)
{
#ifdef BSP_USING_BLE
    struct ble_gap_disc_params params;
    esp_err_t err;
    int rc;

    /* The host keeps running after a run, so a second `bsp_test ble` only
     * re-scans; both the semaphores and the host are brought up once. */
    if (!ble_sem_ready)
    {
        if (rt_sem_init(&ble_sync_sem, "blesync", 0, RT_IPC_FLAG_FIFO) != RT_EOK ||
            rt_sem_init(&ble_disc_sem, "bledisc", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
        {
            test_log("ble", RESULT_FAIL, "semaphore init failed");
            return RESULT_FAIL;
        }
        ble_sem_ready = RT_TRUE;
    }

    if (!ble_host_started)
    {
        err = nimble_port_init();
        if (err == ESP_ERR_INVALID_STATE)
        {
            /* main.c (or a sample app_main) already owns the host; scanning
             * would collide with it, so leave it alone. */
            test_log("ble", RESULT_SKIP, "NimBLE host already up, owned by the app");
            return RESULT_SKIP;
        }
        if (err != ESP_OK)
        {
            test_log("ble", RESULT_FAIL, "nimble_port_init failed (%d)", (int)err);
            return RESULT_FAIL;
        }

        ble_hs_cfg.reset_cb = ble_on_reset;
        ble_hs_cfg.sync_cb  = ble_on_sync;

        nimble_port_freertos_init(ble_host_task);

        if (rt_sem_take(&ble_sync_sem,
                        rt_tick_from_millisecond(TEST_BLE_SYNC_WAIT)) != RT_EOK)
        {
            test_log("ble", RESULT_FAIL, "host did not sync within %d ms",
                     TEST_BLE_SYNC_WAIT);
            return RESULT_FAIL;
        }
        ble_host_started = RT_TRUE;
    }

    ble_report_count = 0;

    rt_memset(&params, 0, sizeof(params));
    params.passive = 0;             /* active scan, so scan responses arrive */
    params.filter_duplicates = 1;

    rc = ble_gap_disc(ble_own_addr_type, TEST_BLE_SCAN_MS, &params,
                      ble_gap_event_cb, RT_NULL);
    if (rc != 0)
    {
        test_log("ble", RESULT_FAIL, "ble_gap_disc failed (%d)", rc);
        return RESULT_FAIL;
    }

    if (rt_sem_take(&ble_disc_sem,
                    rt_tick_from_millisecond(TEST_BLE_SCAN_MS + 3000)) != RT_EOK)
    {
        ble_gap_disc_cancel();
        test_log("ble", RESULT_FAIL, "discovery did not complete in time");
        return RESULT_FAIL;
    }

    if (ble_report_count == 0)
    {
        test_log("ble", RESULT_SKIP, "host ok, no advertiser heard in %d ms",
                 TEST_BLE_SCAN_MS);
        return RESULT_SKIP;
    }

    test_log("ble", RESULT_PASS, "host ok, %u report(s) in %d ms",
             (unsigned int)ble_report_count, TEST_BLE_SCAN_MS);
    return RESULT_PASS;
#else
    test_log("ble", RESULT_SKIP, "BLE disabled (menuconfig: BSP_USING_BLE)");
    return RESULT_SKIP;
#endif
}

/* --------------------------------------------------------- CLOCK_TIMER ---- */

static int test_timer(void)
{
#ifdef RT_USING_CLOCK_TIME
    rt_device_t dev;
    rt_clock_timer_t *timer;
    rt_uint32_t before, after;
    rt_err_t err;

    dev = rt_device_find(TEST_TIMER_DEV);
    if (dev == RT_NULL)
    {
        test_log("timer", RESULT_SKIP, "'%s' not registered", TEST_TIMER_DEV);
        return RESULT_SKIP;
    }

    err = rt_device_open(dev, RT_DEVICE_OFLAG_RDWR);
    if (err != RT_EOK)
    {
        test_log("timer", RESULT_FAIL, "open failed (%d)", err);
        return RESULT_FAIL;
    }

    timer = (rt_clock_timer_t *)dev;

    /* Tell the framework it is periodic, otherwise its ISR stops the timer
     * after the first overflow (the mode defaults to oneshot). */
    rt_uint32_t mode = CLOCK_TIMER_MODE_PERIOD;
    rt_device_control(dev, CLOCK_TIMER_CTRL_MODE_SET, &mode);

    before = timer->overflow;

    err = timer->ops->start(timer, TEST_TIMER_CYCLES, CLOCK_TIMER_MODE_PERIOD);
    if (err != RT_EOK)
    {
        rt_device_close(dev);
        test_log("timer", RESULT_FAIL, "start failed (%d)", err);
        return RESULT_FAIL;
    }

    rt_thread_mdelay(TEST_TIMER_WINDOW);

    if (timer->ops->stop != RT_NULL)
    {
        timer->ops->stop(timer);
    }
    after = timer->overflow;
    rt_device_close(dev);

    if (after == before)
    {
        test_log("timer", RESULT_FAIL, "no interrupt in %dms (%u -> %u)",
                 TEST_TIMER_WINDOW, before, after);
        return RESULT_FAIL;
    }
    if (after - before < TEST_TIMER_WINDOW / 10)
    {
        test_log("timer", RESULT_FAIL,
                 "only %u interrupt(s) in %dms, period mode not reloading",
                 after - before, TEST_TIMER_WINDOW);
        return RESULT_FAIL;
    }

    test_log("timer", RESULT_PASS, "%u interrupt(s) in %dms, range %d..%d Hz",
             after - before, TEST_TIMER_WINDOW,
             timer->info->minfreq, timer->info->maxfreq);
    return RESULT_PASS;
#else
    /* RT_USING_CLOCK_TIME pulls components/drivers/clock_time/arch/risc-v/
     * cputimer.c, which needs riscv_timer.h; that port is not available here,
     * so the feature has to stay disabled until the framework is fixed. */
    test_log("timer", RESULT_SKIP,
             "'%s' not built (RT_USING_CLOCK_TIME disabled)", TEST_TIMER_DEV);
    return RESULT_SKIP;
#endif
}

/* --------------------------------------------- configuration / hardware ---- */

static int test_status(void)
{
    rt_kprintf("  [INFO] RT-Thread %d.%d.%d, tick %d Hz\n",
               RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH,
               RT_TICK_PER_SECOND);

    rt_kprintf("  [INFO] RT_BSP_LED_PIN = %d\n", RT_BSP_LED_PIN);

#ifdef BSP_ENABLE_GDBSTUB
    rt_kprintf("  [PASS] GDBStub  enabled: a panic drops into gdbstub\n");
#else
    rt_kprintf("  [SKIP] GDBStub  disabled (menuconfig: BSP_ENABLE_GDBSTUB)\n");
#endif

#if defined(BSP_USING_WIFI) && defined(BSP_USING_BLE)
    rt_kprintf("  [WARN] WIFI+BLE both enabled: the BSP README says they are\n"
               "         not usable together, keep only one\n");
#endif

#ifdef BSP_USING_WIFI
    rt_kprintf("  [INFO] WIFI     enabled, run `bsp_test wifi` to scan\n");
#else
    rt_kprintf("  [SKIP] WIFI     disabled (menuconfig: BSP_USING_WIFI)\n");
#endif

#ifdef BSP_USING_BLE
    rt_kprintf("  [INFO] BLE      enabled, run `bsp_test ble` to scan\n");
#else
    rt_kprintf("  [SKIP] BLE      disabled (menuconfig: BSP_USING_BLE)\n");
#endif

    rt_kprintf("  [SKIP] JTAG     needs a debugger on the USB-JTAG pins\n");

    return RESULT_PASS;
}

/* ------------------------------------------------------------- runner ---- */

static const struct bsp_test tests[] =
{
    { "gpio",   test_gpio   },
    { "uart",   test_uart   },
    { "i2c",    test_i2c    },
    { "spi",    test_spi    },
    { "timer",  test_timer  },
    { "wifi",   test_wifi   },
    { "ble",    test_ble    },
    { "status", test_status },
};

static int run_one(const struct bsp_test *test)
{
    rt_kprintf("[%s]\n", test->name);
    return test->run();
}

static int run_all(void)
{
    int pass = 0, fail = 0, skip = 0;
    rt_size_t i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        switch (run_one(&tests[i]))
        {
        case RESULT_PASS: pass++; break;
        case RESULT_SKIP: skip++; break;
        default:          fail++; break;
        }
    }

    rt_kprintf("\n==== summary: %d pass, %d fail, %d skip ====\n",
               pass, fail, skip);
    return fail;
}

static int bsp_test(int argc, char **argv)
{
    rt_size_t i;

    if (argc >= 2 && rt_strcmp(argv[1], "list") == 0)
    {
        rt_kprintf("available tests:");
        for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        {
            rt_kprintf(" %s", tests[i].name);
        }
        rt_kprintf("\n");
        return 0;
    }

    if (argc == 1)
    {
        return run_all();
    }

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        if (rt_strcmp(argv[1], tests[i].name) == 0)
        {
            return run_one(&tests[i]);
        }
    }

    rt_kprintf("unknown test '%s', try `bsp_test list`\n", argv[1]);
    return -1;
}
MSH_CMD_EXPORT(bsp_test, ESP32-C3 BSP self test: bsp_test [list|gpio|uart|i2c|spi|timer|wifi|ble|status]);
