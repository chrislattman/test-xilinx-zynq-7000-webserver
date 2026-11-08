#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwipopts.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xstatus.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "network.h"

#define THREAD_STACKSIZE 1024 // in words, i.e. 4 KiB
// How long to wait for a DHCP lease before falling back to a static address
#define DHCP_TIMEOUT_MS 120000

/*
 * This is from the AXI GPIO IP axi_gpio_0 in the block design. The first
 * channel GPIO is mapped to leds_4bits and the second channel GPIO2 is mapped
 * to rgb_led.
 */
#define LED_CHANNEL 1
#define LED0_MASK   0x1 // 0x1 is the bitmask for LED0; 0x2, 0x4, 0x8 for other LEDs

#if !LWIP_SO_RCVTIMEO
#error "LWIP_SO_RCVTIMEO must be 1 (add -DLWIP_SO_RCVTIMEO=1 to the BSP and app compiler flags)"
#endif

static const unsigned short PORT_NUMBER = 8080;
static SemaphoreHandle_t mutex;
static unsigned int counter = 0;

typedef struct sock_info {
    int             client_socket;
    struct in_addr  client_address;
} sock_info;

// Note: errno here is lwIP's (the BSP sets LWIP_PROVIDE_ERRNO=1), so
// <errno.h> must not be included

/**
 * @brief Thread that handles each client connection.
 *
 * @param params pointer to a heap-allocated sock_info struct (freed here)
 */
static void client_handler(void *params)
{
    char server_message[512], content[128], content_length[32];
    char client_message[1024], *headers_begin;
    sock_info *socket_info;
    int client_socket;
    struct in_addr client_address;
    ssize_t n;

    // Each thread gets its own copy of sock_info, so main_task can't
    // overwrite it with the next connection's socket before we read it
    socket_info = (sock_info *) params;
    client_socket = socket_info->client_socket;
    client_address = socket_info->client_address;
    vPortFree(socket_info);

    // Reading the request happens here rather than in main_task so
    // that a slow or silent client only holds up its own task, not the accept
    // loop.
    // pass MSG_WAITALL in 4th argument to recv to wait for client_message to fill up entirely
    if ((n = recv(client_socket, client_message, sizeof(client_message) - 1, 0)) <= 0) {
        if (n < 0) {
            xil_printf("recv: %s\r\n", strerror(errno));
        }
        goto close_client;
    }
    client_message[n] = '\0';
    if ((headers_begin = strstr(client_message, "\r\n")) != NULL) {
        *headers_begin = '\0';
    }
    // Only accepting HTTP GET requests
    if (strncmp(client_message, "GET", 3) != 0) {
        goto close_client;
    }
    xil_printf("%s\r\n", client_message);

    // critical section
    xSemaphoreTake(mutex, portMAX_DELAY); // using a mutex
    ++counter;
    xil_printf("Handling request #%d\r\n", (int) counter);
    xSemaphoreGive(mutex);

    strcpy(server_message, "HTTP/1.1 200 OK\r\n");
    strcat(server_message, "Server: Xilinx Zynq-7000 (Arty Z7)\r\n");
    strcat(server_message, "Accept-Ranges: bytes\r\n");

    strcpy(content, "What's up? This server was written in embedded C. Your IP address is ");
    inet_ntop(AF_INET, &client_address, content + strlen(content), INET_ADDRSTRLEN);
    strcat(content, "\r\n");

    sprintf(content_length, "Content-Length: %u\r\n", (unsigned int) strlen(content));
    strcat(server_message, content_length);
    strcat(server_message, "Content-Type: text/html\r\n\r\n");
    strcat(server_message, content);

    if (send(client_socket, server_message, strlen(server_message), 0) < 0) {
        xil_printf("send: %s\r\n", strerror(errno));
    }

close_client:
    // Always close the socket, even after an error, so lwIP doesn't run out of sockets
    if (shutdown(client_socket, SHUT_WR) < 0 && errno != ENOTCONN) {
        xil_printf("shutdown: %s\r\n", strerror(errno));
    }
    if (close(client_socket) < 0) {
        xil_printf("close: %s\r\n", strerror(errno));
    }
    vTaskDelete(NULL);
}

/**
 * @brief Main server loop for the web server.
 *
 * @param params unused
 */
static void main_task(__attribute__((unused)) void *params)
{
    int server_socket, client_socket;
    struct sockaddr_in server_connection = {0}, client_connection;
    socklen_t client_connection_size;
    sock_info *socket_info;
    // Requires LWIP_SO_RCVTIMEO=1, which is set via -DLWIP_SO_RCVTIMEO=1 in the
    // BSP's proc_extra_compiler_flags and in UserConfig.cmake
    struct timeval recv_timeout = { .tv_sec = 10, .tv_usec = 0 };
    XGpio gpio_instance;

    xil_printf("\r\n\r\n-----lwIP Socket Mode HTTP server (Zynq-7000) ------\r\n");

    // AXI GPIO for the LEDs (like cyw43_arch_init() on the Pico, which also
    // sets up its LED). The XGpio functions assert on an uninitialized
    // instance, so stop if this fails.
    if (XGpio_Initialize(&gpio_instance, XPAR_XGPIO_0_BASEADDR) != XST_SUCCESS) {
        xil_printf("XGpio_Initialize failed\r\n");
        vTaskDelete(NULL);
    }
    XGpio_SetDataDirection(&gpio_instance, LED_CHANNEL, 0x0); // all outputs
    XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, 0x0);    // all LEDs off

    // Bring up lwIP, the Ethernet MAC, and the PHY
    if (network_init() != 0) {
        xil_printf("network_init failed\r\n");
        // If the link fails, blink the LED once
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, LED0_MASK);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, 0x0);
        vTaskDelete(NULL);
    }

    // Get an IP address. If DHCP times out, a static fallback address is used
    // and the server still runs, but the LED blinks once to flag it.
    if (network_connect_timeout_ms(DHCP_TIMEOUT_MS) != 0) {
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, LED0_MASK);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, 0x0);
    } else {
        // If the link succeeds, blink the LED twice and quickly
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, LED0_MASK);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, 0x0);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, LED0_MASK);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        XGpio_DiscreteWrite(&gpio_instance, LED_CHANNEL, 0x0);
    }

    if ((mutex = xSemaphoreCreateMutex()) == NULL) {
        xil_printf("xSemaphoreCreateMutex: out of memory\r\n");
        vTaskDelete(NULL);
    }

    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        xil_printf("socket: %s\r\n", strerror(errno));
        vTaskDelete(NULL);
    }

    server_connection.sin_family = AF_INET;
    server_connection.sin_port = htons(PORT_NUMBER);
    server_connection.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_socket, (struct sockaddr *) &server_connection,
            (socklen_t) sizeof(server_connection)) < 0) {
        xil_printf("bind: %s\r\n", strerror(errno));
        goto cleanup;
    }

    if (listen(server_socket, 4) < 0) {
        xil_printf("listen: %s\r\n", strerror(errno));
        goto cleanup;
    }
    xil_printf("HTTP server listening on port %d\r\n", PORT_NUMBER);

    while (1) {
        memset(&client_connection, 0, sizeof(client_connection));
        client_connection_size = (socklen_t) sizeof(client_connection);
        if ((client_socket = accept(server_socket,
                (struct sockaddr *) &client_connection,
                &client_connection_size)) < 0) {
            // Errors like ENFILE (out of lwIP sockets) only affect one
            // connection; back off briefly and keep serving
            xil_printf("accept: %s\r\n", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Bound how long an idle client can tie up its task
        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
            xil_printf("setsockopt: %s\r\n", strerror(errno));
        }

        // Allocated per connection; client_handler frees it
        if ((socket_info = pvPortMalloc(sizeof(*socket_info))) == NULL) {
            xil_printf("pvPortMalloc: out of memory\r\n");
            close(client_socket);
            continue;
        }
        *socket_info = (sock_info) {
            .client_socket = client_socket,
            .client_address = client_connection.sin_addr,
        };
        // FreeRTOS tasks delete themselves when done (like detached pthreads).
        // Any thread using lwIP should be created using sys_thread_new.
        if (sys_thread_new("http_client", client_handler, socket_info,
                THREAD_STACKSIZE, DEFAULT_THREAD_PRIO) == NULL) {
            xil_printf("sys_thread_new: out of memory\r\n");
            vPortFree(socket_info);
            close(client_socket);
            continue;
        }
    }

cleanup:
    if (close(server_socket) < 0) {
        xil_printf("close: %s\r\n", strerror(errno));
    }
    vTaskDelete(NULL);
}

int main()
{
    // Any thread using lwIP should be created using sys_thread_new
    sys_thread_new("main_thrd", main_task, NULL, THREAD_STACKSIZE, DEFAULT_THREAD_PRIO);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if there wasn't enough FreeRTOS heap
    // to create the idle or timer task. Returning from main() on bare metal
    // isn't meaningful, so stop here instead.
    while (1);
    return 0;
}
