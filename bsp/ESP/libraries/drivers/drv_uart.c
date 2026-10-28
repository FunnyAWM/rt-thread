/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-08-04     tangzz98     first version
 *
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "driver/uart.h"
#include "hal/uart_hal.h"
/* hal/uart_periph.h on IDF v6.1, soc/uart_periph.h on the rtt-dev package */
#if defined(__has_include)
#if __has_include("hal/uart_periph.h")
#include "hal/uart_periph.h"
#else
#include "soc/uart_periph.h"
#endif
#else
#include "soc/uart_periph.h"
#endif
#include "sdkconfig.h"
#ifdef RT_USING_SERIAL_V1

#ifdef CONFIG_UART_ISR_IN_IRAM
#define UART_ISR_ATTR IRAM_ATTR
#else
#define UART_ISR_ATTR
#endif

uart_hal_context_t hal[] = {
    {
        .dev = &UART0,
    },
    {
        .dev = &UART1,
    },
};

static struct rt_serial_device _serial;

static void mcu_uart_rx_intr_handler(void *param)
{
    uint32_t uart_intr_status;
    struct rt_serial_device *serial;
    uart_port_t port;
    rt_interrupt_enter();
    serial = (struct rt_serial_device *)param;
    port = (uart_port_t)serial->parent.user_data;

    uart_intr_status = uart_hal_get_intsts_mask(&hal[port]);
    if (uart_intr_status != 0)
    {
        if (uart_intr_status & UART_INTR_RXFIFO_FULL)
        {
            rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_IND);
        }
        uart_hal_clr_intsts_mask(&hal[port], uart_intr_status);
    }
    rt_interrupt_leave();
}

static rt_err_t mcu_uart_configure(struct rt_serial_device *serial, struct serial_configure *cfg)
{
    return RT_EOK;
}

static rt_err_t mcu_uart_control(struct rt_serial_device *serial, int cmd, void *arg)
{
    uart_port_t port = (uart_port_t)serial->parent.user_data;

    switch (cmd)
    {
    case RT_DEVICE_CTRL_SET_INT:
        /* The serial framework calls this after serial_rx is allocated; the
         * peripheral interrupt must not be enabled any earlier or the RX ISR
         * would run against an unallocated rx_fifo. */
        if (((rt_uint32_t)(uintptr_t)arg) & RT_DEVICE_FLAG_INT_RX)
        {
            uart_intr_config_t uart_intr = {
                .intr_enable_mask = UART_INTR_RXFIFO_FULL,
                .rxfifo_full_thresh = 1,
            };
            ESP_ERROR_CHECK(uart_intr_config(port, &uart_intr));
        }
        break;
    case RT_DEVICE_CTRL_CLR_INT:
        uart_hal_disable_intr_mask(&hal[port], UART_INTR_RXFIFO_FULL);
        break;
    default:
        break;
    }

    return RT_EOK;
}

static int mcu_uart_putc(struct rt_serial_device *serial, char c)
{
    uart_port_t port = (uart_port_t)serial->parent.user_data;
    uint32_t write_size = 0;
    do
    {
        uart_hal_write_txfifo(&hal[port], (const uint8_t *)&c, 1, &write_size);
    } while (write_size == 0);

    return 1;
}

static int mcu_uart_getc(struct rt_serial_device *serial)
{
    uart_port_t port = (uart_port_t)serial->parent.user_data;
    uint8_t c;
    int len = uart_hal_get_rxfifo_len(&hal[port]);
    if (len == 0)
    {
        return -1;
    }
    else
    {
        len = 1;
        uart_hal_read_rxfifo(&hal[port], &c, &len);
        return (int)c;
    }
}

static const struct rt_uart_ops _uart_ops = {
    mcu_uart_configure,
    mcu_uart_control,
    mcu_uart_putc,
    mcu_uart_getc,
    RT_NULL,
};

int rt_hw_uart_init(void)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE_115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#ifdef SOC_ESP32_C6
        .source_clk = UART_SCLK_DEFAULT,
#else
        .source_clk = UART_SCLK_APB,
#endif
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_param_config(RT_BSP_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(RT_BSP_UART_PORT, RT_BSP_UART_TX_PIN, RT_BSP_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Bind the ISR only; the RX interrupt itself is enabled from
     * mcu_uart_control(RT_DEVICE_CTRL_SET_INT) once the device is opened. */
    ESP_ERROR_CHECK(esp_intr_alloc(uart_periph_signal[RT_BSP_UART_PORT].irq, intr_alloc_flags, mcu_uart_rx_intr_handler, (void *)&_serial, NULL));
    /* The ROM/2nd-stage bootloader leaves UART int_ena bits set, so the RX
     * interrupt would fire before the serial framework allocates rx_fifo. */
    uart_hal_disable_intr_mask(&hal[RT_BSP_UART_PORT], UART_INTR_RXFIFO_FULL);
    uart_hal_clr_intsts_mask(&hal[RT_BSP_UART_PORT], UART_INTR_RXFIFO_FULL);
    _serial.ops = &_uart_ops;
    _serial.config = config;

    return rt_hw_serial_register(&_serial, "uart", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX, (void *)RT_BSP_UART_PORT);
}
INIT_BOARD_EXPORT(rt_hw_uart_init);

#endif /* RT_USING_SERIAL_V1 */
