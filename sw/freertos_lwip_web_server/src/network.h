/*
 * Ethernet bring-up for the Zynq-7000 (Arty Z7): lwIP, the GEM0 MAC, the
 * Realtek PHY, and DHCP. Plays the role that pico/cyw43_arch.h plays in
 * ../test-bare-metal-pico-webserver, so server.c only deals with sockets.
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>

/**
 * @brief Initializes lwIP and the Ethernet interface (MAC + PHY), and starts
 * the lwIP packet receive thread.
 *
 * Must be called from a FreeRTOS task created with sys_thread_new(), before
 * any other lwIP call. Analogous to cyw43_arch_init() on the Pico.
 *
 * @return 0 on success or -1 on error
 */
int network_init(void);

/**
 * @brief Gets an IPv4 address via DHCP, waiting up to timeout_ms. If DHCP
 * doesn't answer in time, a static fallback address (192.168.1.10/24, gateway
 * 192.168.1.1) is configured instead. Prints the resulting IP settings.
 *
 * Must be called after network_init(). Analogous to
 * cyw43_arch_wifi_connect_timeout_ms() on the Pico.
 *
 * @param timeout_ms how long to wait for a DHCP lease
 * @return 0 if DHCP succeeded, or -1 if it timed out and the static fallback
 * address is in use (the network is usable either way)
 */
int network_connect_timeout_ms(uint32_t timeout_ms);

#endif
