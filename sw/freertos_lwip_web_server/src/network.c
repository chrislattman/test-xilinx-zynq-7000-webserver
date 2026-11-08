/*
 * Ethernet bring-up for the Zynq-7000 (Arty Z7): lwIP, the GEM0 MAC, the
 * Realtek PHY, and DHCP. See network.h.
 *
 * Derived from the Xilinx freertos_lwip_echo_server template's main.c:
 *
 * Copyright (C) 2016 - 2022 Xilinx, Inc.
 * Copyright (C) 2022 - 2023 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#include "network.h"

#include "xparameters.h"
#include "netif/xadapter.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "lwip/init.h"
#include "FreeRTOS.h"
#include "task.h"

#if LWIP_DHCP == 1
#include "lwip/dhcp.h"
#endif

// server.c only speaks IPv4 (AF_INET); the template's IPv6 path was dropped
#if LWIP_IPV6 == 1
#error "IPv6 is not supported; disable lwip220_ipv6_enable in the BSP"
#endif

#define THREAD_STACKSIZE 1024 // in words, i.e. 4 KiB

static struct netif server_netif;

/**
 * @brief Prints an IPv4 address with a label.
 */
static void print_ip(const char *msg, const ip_addr_t *ip)
{
    xil_printf("%s%d.%d.%d.%d\r\n", msg, ip4_addr1(ip), ip4_addr2(ip),
            ip4_addr3(ip), ip4_addr4(ip));
}

/**
 * @brief Prints the interface's IP address, netmask, and gateway.
 */
static void print_ip_settings(const struct netif *netif)
{
    print_ip("Board IP: ", &netif->ip_addr);
    print_ip("Netmask : ", &netif->netmask);
    print_ip("Gateway : ", &netif->gw);
}

/**
 * @brief Configures the static fallback address.
 */
static void set_static_ip(struct netif *netif)
{
    IP4_ADDR(&netif->ip_addr, 192, 168, 1, 10);
    IP4_ADDR(&netif->netmask, 255, 255, 255, 0);
    IP4_ADDR(&netif->gw, 192, 168, 1, 1);
}

#if LWIP_DHCP == 1
/**
 * @brief Drives lwIP's DHCP fine and coarse timers, as the Xilinx template
 * does (the BSP builds lwIP with NO_SYS_NO_TIMERS=1).
 *
 * @param params unused
 */
static void dhcp_timer_thread(__attribute__((unused)) void *params)
{
    int mscnt = 0;

    while (1) {
        vTaskDelay(DHCP_FINE_TIMER_MSECS / portTICK_RATE_MS);
        dhcp_fine_tmr();
        mscnt += DHCP_FINE_TIMER_MSECS;
        if (mscnt >= DHCP_COARSE_TIMER_SECS * 1000) {
            dhcp_coarse_tmr();
            mscnt = 0;
        }
    }
}
#endif

int network_init(void)
{
    // the MAC address of the board; this should be unique per board
    unsigned char mac_ethernet_address[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };
    ip_addr_t ipaddr, netmask, gw;

    // initialize lwIP before calling sys_thread_new
    lwip_init();

    ipaddr.addr = 0;
    netmask.addr = 0;
    gw.addr = 0;

    // Add the network interface to the netif_list. This also brings up the
    // GEM MAC and the PHY (autonegotiation; see xemacpsif_physpeed.c).
    if (!xemac_add(&server_netif, &ipaddr, &netmask, &gw, mac_ethernet_address,
            PLATFORM_EMAC_BASEADDR)) {
        xil_printf("xemac_add: error adding network interface\r\n");
        return -1;
    }
    netif_set_default(&server_netif);
    netif_set_up(&server_netif);

    // start the packet receive thread - required for lwIP operation.
    // Any thread using lwIP should be created using sys_thread_new.
    if (sys_thread_new("xemacif_input_thread", (void (*)(void *)) xemacif_input_thread,
            &server_netif, THREAD_STACKSIZE, DEFAULT_THREAD_PRIO) == NULL) {
        xil_printf("sys_thread_new: out of memory\r\n");
        return -1;
    }

    return 0;
}

int network_connect_timeout_ms(uint32_t timeout_ms)
{
#if LWIP_DHCP == 1
    uint32_t mscnt = 0;

    if (sys_thread_new("dhcp_timer_thread", dhcp_timer_thread, NULL,
            THREAD_STACKSIZE, DEFAULT_THREAD_PRIO) == NULL) {
        xil_printf("sys_thread_new: out of memory\r\n");
        goto static_ip;
    }
    dhcp_start(&server_netif);

    while (server_netif.ip_addr.addr == 0) {
        if (mscnt >= timeout_ms) {
            xil_printf("ERROR: DHCP request timed out\r\n");
            goto static_ip;
        }
        vTaskDelay(DHCP_FINE_TIMER_MSECS / portTICK_RATE_MS);
        mscnt += DHCP_FINE_TIMER_MSECS;
    }
    xil_printf("DHCP request success\r\n");
    print_ip_settings(&server_netif);
    return 0;

static_ip:
    // Stop DHCP so it doesn't later overwrite the static address
    dhcp_stop(&server_netif);
    xil_printf("Configuring default IP of 192.168.1.10\r\n");
    set_static_ip(&server_netif);
    print_ip_settings(&server_netif);
    return -1;
#else
    // DHCP is disabled in the BSP, so the static address is the configuration
    (void) timeout_ms;
    set_static_ip(&server_netif);
    print_ip_settings(&server_netif);
    return 0;
#endif
}
