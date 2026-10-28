#ifndef SERIAL_HANDLER_H
#define SERIAL_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

typedef struct {
    uint8_t *data;
    int length;
} uart_data_t;

void serial_init(void);
// Accoda i dati: la scrittura fisica sulla UART viene eseguita dal task TX.
void serial_send_data(const uint8_t *data, int length);
void serial_send_string(const char *str);
/* True when bridge-queued UART data is fully drained (including active write). */
bool serial_uart_tx_idle(void);
/* Called while the MPG lease mutex is held; normal TCP TX is refused while
 * claimed. The watchdog/MPG_STOP path releases ownership after jog cancel. */
bool serial_uart_mpg_claim(void);
void serial_uart_mpg_release(void);
bool serial_uart_mpg_owned(void);
void serial_task_start(void);
QueueHandle_t serial_get_queue(void);
bool serial_get_data(uart_data_t **data, TickType_t wait_time);
void serial_free_data(uart_data_t *data);

#endif
