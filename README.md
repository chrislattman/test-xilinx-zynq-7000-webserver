# Arty Z7-20 web server with FreeRTOS + lwIP

This is essentially a bare metal version of https://github.com/chrislattman/webserver/blob/master/server.c

While this repo implements its own HTTP server, a popular embedded C web server is Mongoose.

## Network setup

At boot the board requests an address over DHCP. If that times out, it falls back to the static address `192.168.1.10/24` (LED0 blinks once). Either way, the server listens on port 8080.

### Through a router/gateway

Plug the board into your router. It gets an address over DHCP and prints it on the UART (115200 8N1). Browse to `http://<board-ip>:8080/`.

### Direct Ethernet cable to a Linux PC

With nothing to answer DHCP, the board uses `192.168.1.10`. Give the PC's wired interface a static address on the same subnet. Leave out the gateway so your other connection (e.g. Wi-Fi) stays the default route:

```sh
nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 192.168.1.2/24 ipv4.gateway "" ipv4.never-default yes
nmcli con up "Wired connection 1"
```

Replace `Wired connection 1` with your wired profile's name (`nmcli con show`). Then reset the board, wait for the DHCP timeout, and browse to `http://192.168.1.10:8080/`.

The addresses on the direct link:

| Address | What it is |
| --- | --- |
| `192.168.1.10` | The board. This is the fallback address set in `set_static_ip()` in `sw/freertos_lwip_web_server/src/network.c` (also the default in Xilinx's lwIP examples). |
| `192.168.1.2` | The PC's wired interface. Any free address in `192.168.1.2`-`192.168.1.254` other than `.10` works. |
| `192.168.1.1` | The board's default gateway, also set in `set_static_ip()`. No device has this address on a direct link. That's fine: the PC is on the same `/24` subnet, so the board reaches it directly and never uses the gateway. |

The `/24` mask (`255.255.255.0`) puts every `192.168.1.x` address on one network, so they reach each other without a router. The PC needs no gateway on this link either, which keeps its default route on its other connection.

To switch the profile back to DHCP:

```sh
nmcli con mod "Wired connection 1" ipv4.method auto ipv4.addresses "" ipv4.never-default no
nmcli con up "Wired connection 1"
```
