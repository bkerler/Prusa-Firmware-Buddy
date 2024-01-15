#include "../gcode.h"
#include "inet.h"
#include "wui_api.h"
#include "netdev.h"
#include "netif_settings.h"

/** \addtogroup G-Codes
 * @{
 */

/**
 * M46: Reports the assigned IP address to serial port
 *
 * ## Parameters
 *
 * - `M` - Also print out MAC address to serial port
 */

void GcodeSuite::M46() {
    lan_t ethconfig = {};
    netdev_get_ip_addresses(netdev_get_active_id(), &ethconfig);
    uint8_t *ipp = (uint8_t *)&ethconfig.addr_ip4.u_addr.ip4;
    char ip4_string[17] = { 0 };
    snprintf(ip4_string, sizeof(ip4_string), "%u.%u.%u.%u\n", ipp[0], ipp[1], ipp[2], ipp[3]);
    serialprintPGM(ip4_string);
    uint32_t ip6[4];
    char ip6_string[40] = { 0 };
    memcpy(ip6, ethconfig.addr_ip6.u_addr.ip6.addr, sizeof(ip6));
    inet6_ntoa_r(ethconfig.addr_ip6, ip6_string, sizeof(ip6_string));
    serialprintPGM(ip6_string);
    SERIAL_CHAR('\n');
    if (parser.seen('M')) {
        mac_address_t mac;
        char mac_buffer[19] = { 0 };
        get_MAC_address(&mac, netdev_get_active_id());
        snprintf(mac_buffer, sizeof(mac_buffer), "%s\n", mac);
        serialprintPGM(mac_buffer);
    }
}

/** @}*/
