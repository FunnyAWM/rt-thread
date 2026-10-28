/*
 * SPDX-FileCopyrightText: 2020 Amazon.com, Inc. or its affiliates
 *
 * SPDX-License-Identifier: MIT
 *
 * SPDX-FileContributor: 2016-2022 Espressif Systems (Shanghai) CO LTD
 */
/*
 * FreeRTOS Kernel V10.4.3
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 * 1 tab == 4 spaces!
 */

/*-----------------------------------------------------------------------
 * Implementation of functions defined in portable.h for the RISC-V port.
 *----------------------------------------------------------------------*/

#include "sdkconfig.h"
#include <string.h>
#include "soc/soc_caps.h"
#include "soc/periph_defs.h"
#include "soc/system_reg.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "riscv/rvruntime-frames.h"
#include "riscv/interrupt.h"
#include "esp_private/crosscore_int.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_intr_alloc.h"
#include "esp_newlib.h"
#include "esp_log.h"
#include "FreeRTOS.h"       /* This pulls in portmacro.h */
#include "task.h"
#include "portmacro.h"
#include "esp_memory_utils.h"

/* ---------------------------------------------------- Variables ------------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */

static const char *TAG = "cpu_start";

/* ---------------------------------------------- Port Implementations -------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */

// --------------------- Interrupts ------------------------

BaseType_t xPortInIsrContext(void)
{
    return (BaseType_t)rt_interrupt_get_nest();
}

BaseType_t IRAM_ATTR xPortInterruptedFromISRContext(void)
{
    /* For single core, this can be the same as xPortInIsrContext() because reading it is atomic */
    return (BaseType_t)rt_interrupt_get_nest();
}

// ----------------------- System --------------------------

uint32_t xPortGetTickRateHz(void)
{
    return (uint32_t)configTICK_RATE_HZ;
}

#define STACK_WATCH_AREA_SIZE 32
#define STACK_WATCH_POINT_NUMBER (SOC_CPU_WATCHPOINTS_NUM - 1)

void vPortSetStackWatchpoint(void *pxStackStart)
{
    uint32_t addr = (uint32_t)pxStackStart;
    addr = (addr + (STACK_WATCH_AREA_SIZE - 1)) & (~(STACK_WATCH_AREA_SIZE - 1));
    esp_cpu_set_watchpoint(STACK_WATCH_POINT_NUMBER, (char *)addr, STACK_WATCH_AREA_SIZE, ESP_CPU_WATCHPOINT_STORE);
}



/* ---------------------------------------------- Misc Implementations -------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */

// --------------------- App Start-up ----------------------

/* [refactor-todo]: See if we can include this through a header */
extern void esp_startup_start_app_common(void);

/* Boot-stage glue for the RT-Thread configuration of ESP-IDF. */
#include "esp_private/startup_internal.h"
#include "esp_timer_impl.h"
#include "hal/wdt_hal.h"

extern int __heap_start__;
extern int __heap_end__;

/*
 * The 2nd stage bootloader arms the RTC watchdog for
 * CONFIG_BOOTLOADER_WDT_TIME_MS (9000 ms) and ESP-IDF disables it again from
 * components/esp_system/startup_funcs.c:init_disable_rtc_wdt().  This BSP does
 * not build that file, so without the same init here the chip resets every
 * nine seconds.
 */
#if SOC_RTC_WDT_SUPPORTED
ESP_SYSTEM_INIT_FN(bsp_disable_rtc_wdt, SECONDARY, BIT(0), 999)
{
    wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();

    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);

    return ESP_OK;
}
#endif

/*
 * ESP-IDF v6.1 runs the ESP_SYSTEM_INIT_FN(CORE) functions -- including
 * init_flash(), which creates FreeRTOS mutexes through the wrapper's
 * rt_malloc() -- before esp_system/startup.c calls rt_system_heap_init().
 * The first allocation therefore hits an uninitialized heap and trips
 * "m != RT_NULL" in rt_smem_alloc().  Bring the heap up ahead of init_flash()
 * and make the later call from startup.c a no-op.
 */
void rt_system_heap_init(void *begin_addr, void *end_addr)
{
    static rt_bool_t heap_ready = RT_FALSE;

    if (heap_ready)
    {
        return;
    }
    heap_ready = RT_TRUE;
    rt_system_heap_init_generic(begin_addr, end_addr);
}

ESP_SYSTEM_INIT_FN(rt_heap_init, CORE, BIT(0), 120)
{
    rt_system_heap_init(&__heap_start__, &__heap_end__);
    return ESP_OK;
}

/*
 * ESP-IDF registers the newlib bring-up from components/esp_libc/src/init.c,
 * which this BSP does not build.  Without it syscall_table_ptr stays NULL and
 * any libc call that needs errno (atoi/strtol/...) faults inside the ROM
 * __getreent(), and stdio locks stay uninitialised so any printf-family call
 * trips "Uninitialized lock used".  Keep the same order IDF uses.
 */
ESP_SYSTEM_INIT_FN(bsp_libc_init, CORE, BIT(0), 121)
{
    esp_libc_init();
    return ESP_OK;
}

ESP_SYSTEM_INIT_FN(bsp_libc_time_init, CORE, BIT(0), 127)
{
    esp_libc_time_init();
    return ESP_OK;
}

ESP_SYSTEM_INIT_FN(bsp_libc_stdio_init, CORE, BIT(0), 126)
{
#if CONFIG_VFS_SUPPORT_IO
    esp_libc_init_global_stdio("/dev/console");
#else
    esp_libc_init_global_stdio();
#endif
    return ESP_OK;
}

/*
 * This BSP does not build components/esp_timer/src/esp_timer_init.c, so IDF's
 * esp_timer_init_nonos() (CORE, priority 101) never registers and the esp_timer
 * systimer HAL stays uninitialized.  init_flash() -> spi1_end() -> on_spi_released()
 * calls esp_system_get_time() when CONFIG_SPI_FLASH_YIELD_DURING_ERASE is set, and
 * that dereferences the NULL HAL context (load access fault at 0x4).
 * Do the early part of esp_timer initialization here; the ISR/task part is not
 * needed for esp_timer_get_time().
 */
ESP_SYSTEM_INIT_FN(rt_esp_timer_early_init, CORE, BIT(0), 125)
{
    esp_timer_impl_early_init();
    return ESP_OK;
}

void esp_startup_start_app(void)
{
    esp_startup_start_app_common();

    ESP_LOGI(TAG, "Starting scheduler.");
    vTaskStartScheduler();
}
