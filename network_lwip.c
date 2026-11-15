/*
 * Copyright (c) 2024, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file network_lwip.c
 * @brief Bridge TI WLAN Ethernet frames and STA/AP interfaces to lwIP.
 * Owns static netif objects, DHCP mode flags and one extra status callback.
 * Initialize the TCP/IP thread before adding interfaces. Public interface
 * mutation wrappers take the lwIP core lock; internal callbacks require the
 * core context/lock. Packet callbacks copy RX data and flatten chained TX data.
 * No interface storage is transferred to callers of the interface getters.
 */
/* C runtime includes */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* lwIP core includes */
#include "lwip/opt.h"

#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/api.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "lwip/pbuf.h"

/* lwIP netif includes */
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "lwip/dhcp.h"

/* applications includes */
#include "lwip/apps/netbiosns.h"
#include "lwip/apps/httpd.h"
#include "apps/httpserver/httpserver-netconn.h"
#include "apps/netio/netio.h"
#include "apps/ping/ping.h"
#include "apps/rtp/rtp.h"
#include "apps/chargen/chargen.h"
#include "apps/shell/shell.h"
#include "apps/tcpecho/tcpecho.h"
#include "apps/udpecho/udpecho.h"
#include "apps/tcpecho_raw/tcpecho_raw.h"
#include "apps/socket_examples/socket_examples.h"

#include "examples/lwiperf/lwiperf_example.h"
#include "examples/mdns/mdns_example.h"
#include "examples/snmp/snmp_example.h"
#include "examples/tftp/tftp_example.h"
#include "examples/sntp/sntp_example.h"
#include "examples/mqtt/mqtt_example.h"


#include "examples/httpd/cgi_example/cgi_example.h"
#include "examples/httpd/fs_example/fs_example.h"
#include "examples/httpd/ssi_example/ssi_example.h"

#include "default_netif.h"
#include "wlan_if.h"
#include "network_lwip.h"
#include "lwipopts.h"


#if NO_SYS
/* ... then we need information about the timer intervals: */
#include "lwip/ip4_frag.h"
#include "lwip/igmp.h"
#endif /* NO_SYS */

#include "netif/ppp/ppp_opts.h"
#if PPP_SUPPORT
/* PPP includes */
#include "lwip/sio.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/pppoe.h"
#if !NO_SYS && !LWIP_PPP_API
#error With NO_SYS==0, LWIP_PPP_API==1 is required.
#endif
#endif /* PPP_SUPPORT */

/* include the port-dependent configuration */
//#include "lwipcfg.h"

//#include "test_enet_lwip.h"

#include "uart_term.h"
#include "wlan_cmd.h"
#include "wlan_if.h"
#include "osi_kernel.h"
#include "dhcpserver.h"
#include "network_terminal.h"

#ifndef LWIP_EXAMPLE_APP_ABORT
#define LWIP_EXAMPLE_APP_ABORT() 0
#endif

/** Define this to 1 to enable a port-specific ethernet interface as default interface. */
#ifndef USE_DEFAULT_ETH_NETIF
#define USE_DEFAULT_ETH_NETIF 1
#endif

/** Define this to 1 to enable a PPP interface. */
#ifndef USE_PPP
#define USE_PPP 0
#endif

/** Define this to 1 or 2 to support 1 or 2 SLIP interfaces. */
#ifndef USE_SLIPIF
#define USE_SLIPIF 0
#endif

/** Use an ethernet adapter? Default to enabled if port-specific ethernet netif or PPPoE are used. */
#ifndef USE_ETHERNET
#define USE_ETHERNET  (USE_DEFAULT_ETH_NETIF || PPPOE_SUPPORT)
#endif

/** Use an ethernet adapter for TCP/IP? By default only if port-specific ethernet netif is used. */
#ifndef USE_ETHERNET_TCPIP
#define USE_ETHERNET_TCPIP  (USE_DEFAULT_ETH_NETIF)
#endif

#if USE_SLIPIF
#include <netif/slipif.h>
#endif /* USE_SLIPIF */

#ifndef USE_DHCP
#define USE_DHCP    LWIP_DHCP
#endif
#ifndef USE_AUTOIP
#define USE_AUTOIP  LWIP_AUTOIP
#endif

#define ETH_MAX_PAYLOAD  1514
#define VLAN_TAG_SIZE         (4U)
#define ETHHDR_SIZE     14
#define ETH_FRAME_SIZE        (ETH_MAX_PAYLOAD + VLAN_TAG_SIZE)

/* ========================================================================== */
/*                         Structure Declarations                             */
/* ========================================================================== */

/* globales variables for netifs */
#if USE_ETHERNET
#if LWIP_DHCP
/* dhcp struct for the ethernet netif */
struct dhcp netif_dhcp;
#endif /* LWIP_DHCP */
#if LWIP_AUTOIP
/* autoip struct for the ethernet netif */
struct autoip netif_autoip;
#endif /* LWIP_AUTOIP */
#endif /* USE_ETHERNET */
#if USE_PPP
/* THE PPP PCB */
ppp_pcb *ppp;
/* THE PPP interface */
struct netif ppp_netif;
/* THE PPP descriptor */
u8_t sio_idx = 0;
sio_fd_t ppp_sio;
#endif /* USE_PPP */
#if USE_SLIPIF
struct netif slipif1;
#if USE_SLIPIF > 1
struct netif slipif2;
#endif /* USE_SLIPIF > 1 */
#endif /* USE_SLIPIF */


//#define STATIC_IP
#ifdef STATIC_IP
#ifdef CC35XX
const char static_ip[4]     = {10,0,0,103};
const char static_gw[4]     = {10,0,0,1};
#endif
#ifdef CC33XX
const char static_ip[4]     = {10,123,45,12};
const char static_gw[4]     = {10,123,45,1};
#endif 
const char static_mask[4]   = {255,255,255,0};

#endif


struct netif staif = {0};
struct netif apif = { .ip_addr.addr = PP_HTONL(LWIP_MAKEU32(10, 0, 0, 3)),
                      .netmask.addr = PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0)),
                      .gw.addr      = PP_HTONL(LWIP_MAKEU32(10, 0, 0, 3)) };
extern appControlBlock app_CB;

static uint32_t isIpAcquired;
static uint8_t sta_ip_mode = IP_DHCP;
static uint8_t ap_ip_mode = IP_DHCP;
static void (*extra_status_callback)(WlanRole_e roleid, uint32_t address);


/**
 * @brief Query a station ARP entry and poll for its resolution.
 * @param ip_addr Valid pointer to an ip4_addr_t target address.
 * @return ERR_OK if resolved; ERR_CONN if STA is down or no reply arrives;
 *  otherwise the error from etharp_query().
 * @note Sends the query under the core lock, then polls at 10 ms intervals for
 *  up to 3000 ms. Sleeps in task context; do not call while holding the core lock.
 */
int update_arp(void* ip_addr)
{
    err_t result = ERR_CONN;
    ssize_t arp_find;
    struct netif *pNetIf = &staif;
    struct eth_addr* eth_ret = NULL;
    ip4_addr_t* ip4_ret = NULL;
    ip4_addr_t* ip4_addr = (ip4_addr_t*)ip_addr;

    if(netif_is_up(pNetIf))
    {
        LOCK_TCPIP_CORE();
        result = etharp_query(pNetIf, (const ip4_addr_t *) ip4_addr, NULL);
        UNLOCK_TCPIP_CORE();
        if(result == ERR_OK)
        {
            result = ERR_CONN;
            //wait here some time for reply to be received
            uint32_t timeout_ms = 3000; // 3 seconds timeout
            uint32_t elapsed_ms = 0;
            uint32_t sleep_interval_ms = 10; // Check every 10ms
            while(elapsed_ms < timeout_ms)
            {
                osi_Sleep(sleep_interval_ms);//set time to other thread to get the reply
                eth_ret = NULL;
                ip4_ret = NULL;
                arp_find = etharp_find_addr(pNetIf,(ip4_addr_t *)ip4_addr, &eth_ret,(const ip4_addr_t **)&ip4_ret);
                if(arp_find <0 )
                {
                    result = ERR_CONN;
                }
                else
                {
                    if (ip4_ret && eth_ret && ip4_addr_cmp(ip4_ret, ip4_addr) )
                    {
                       result = ERR_OK;
                       break;//found
                    }
                }
                elapsed_ms += sleep_interval_ms;
            }
        }
    }
    return result;
}

/**
 * @brief Update MAC/IP bookkeeping and notify application address waiters.
 * @param state_netif Valid lwIP interface whose status/address changed.
 * @note Runs in lwIP core context. Maps STA/AP roles (AP is reported as STA
 *  during P2P grouping). On an up interface, copies its MAC, updates the shared
 *  acquisition flag, signals the DHCP waiter for nonzero IPv4 and invokes the
 *  optional extra callback even for zero IPv4. The down branch only logs.
 */
void status_callback(struct netif *state_netif)
{
    WlanRole_e roleid = WLAN_ROLE_NONE;
    if(network_get_sta_if() == state_netif)
    {
        roleid = WLAN_ROLE_STA;
    }
    else if (network_get_ap_if() == state_netif)
    {
        if (GET_STATUS_BIT(app_CB.Status, STATUS_BIT_P2P_GROUP_STARTED))
        {
            roleid = WLAN_ROLE_STA;
        }
        else
        {
            roleid = WLAN_ROLE_AP;
        }
    }
    if (netif_is_up(state_netif))
    {
        const ip4_addr_t *temp;

        WlanMacAddress_t macAddressParams;
        memset(&macAddressParams, 0, sizeof(WlanMacAddress_t));

        macAddressParams.roleType = roleid;
      
        Wlan_Get(WLAN_GET_MACADDRESS, (void *)&macAddressParams);

        memcpy(state_netif->hwaddr, macAddressParams.pMacAddress, sizeof (macAddressParams.pMacAddress));

        state_netif->hwaddr_len = 6;
        temp = netif_ip4_addr(state_netif);
        if (temp->addr)
        {
            isIpAcquired = 1;
            Report("\r\nstatus_callback==UP, local interface IP is %s\r\n", ip4addr_ntoa(netif_ip4_addr(state_netif)));

            if (app_CB.CON_CB.dhcpIprecvSyncObj)
            {
                osi_SyncObjSignal(&app_CB.CON_CB.dhcpIprecvSyncObj);
            }
        }
        else
        {
            Report("\n\rIp address was not received!! \r\n");
            isIpAcquired = 0;
        }

        if (extra_status_callback)
        {
            extra_status_callback(roleid, temp->addr);
        }
    }
    else
    {
        Report("\n\rstatus_callback==DOWN\r\n");
    }
}


/**
 * @brief Print the first eight buffer bytes in hexadecimal for debugging.
 * @param in Source containing at least eight readable bytes.
 * @param len Ignored; does not bound the read.
 */
void printBuffer(uint8_t *in,uint32_t len)
{
    int j, total;
    total  = 0;

    Report("\n\r");

    while(total < 8)
    {
        for(j = 0; j < 8 ; j++)
        {
            Report("%x ",in[total]);
            total++;
        }
        Report("\n\r");
    }
}

/**
 * @brief Copy a received WLAN Ethernet frame into lwIP input storage.
 * @param roleId WLAN_ROLE_STA or WLAN_ROLE_AP; other roles drop the frame.
 * @param inBuf Borrowed frame bytes, valid for inLen bytes during this call.
 * @param inLen Number of bytes to copy.
 * @note Allocates a pool pbuf and submits it to tcpip_input(), transferring
 *  ownership on success and freeing on failure. Assumes the first pbuf payload
 *  has contiguous room for the entire frame; does not copy across a pbuf chain.
 */
void network_recv(WlanRole_e roleId, uint8_t *inBuf, uint32_t inLen)
{
    struct netif *pIf = NULL;
    struct pbuf *packet;    
    err_t err;


    if(roleId == WLAN_ROLE_STA)
    {
        pIf = (struct netif *)network_get_sta_if();
    }
    else if(roleId == WLAN_ROLE_AP)
    {
        pIf = (struct netif *)network_get_ap_if();
    }
    else
    {
        Report("\n\r network_recv,  no role available,packet DROP, roleId:%d",roleId);
        return;
    }

    //printBuffer(inBuf,inLen);
    packet = pbuf_alloc(PBUF_RAW , inLen, PBUF_POOL);
    if(!packet){
        //Report("\n\rRx pbuf_alloc: no storage for allocating packets, DROP");
        return;
    }
    memcpy(packet->payload,inBuf,inLen);
    packet->len = inLen;
    packet->tot_len = inLen;


    if ((err = tcpip_input(packet, pIf)) != 0) {
        //Report("\n\rRx pbuf_queue is full: err:%d, DROP", err);
        pbuf_free(packet);
        return;
    }
}

/**
 * @brief Register RX delivery and start/stop DHCP services on link changes.
 * @param state_netif Valid STA/AP interface; runs in lwIP core context.
 * @note Link-up registers network_recv. STA starts DHCP when appropriate or
 *  announces its retained address; AP starts the DHCP server and announces its
 *  address. Link-down unregisters RX and stops the role's DHCP service.
 */
void link_callback(struct netif *state_netif)
{
    WlanRole_e roleid = WLAN_ROLE_NONE;
    err_t err;
    if (network_get_sta_if() == state_netif)
    {
        roleid = WLAN_ROLE_STA;
    }
    else if (network_get_ap_if() == state_netif)
    {
        roleid = WLAN_ROLE_AP;
    }
    if (netif_is_link_up(state_netif))
    {
        Wlan_EtherPacketRecvRegisterCallback(roleid, network_recv);
        if (roleid == WLAN_ROLE_STA)
        {
        	if (!isIpAcquired)
        	{
            	Report("\n\r link_callback==UP starting DHCP");
#ifdef STATIC_IP
            	err = dhcp_start(state_netif);
            	dhcp_inform(state_netif);
            	etharp_gratuitous(state_netif);
#else
                if (sta_ip_mode == IP_DHCP)
            	{
            	    err = dhcp_start(state_netif);
#endif
            	    Report("\n\r DHCP is %d\n\n\r", err);
            	}
         	}
         	else
         	{
            	etharp_gratuitous(state_netif);
         	}
         	

        }
        else if (roleid == WLAN_ROLE_AP)
        {
            Report("\n\rlink_callback==UP starting DHCP Server\n\r");
            dhcps_start(state_netif->ip_addr.addr, state_netif);
            dhcp_inform(state_netif);
            etharp_gratuitous(state_netif);
        }

       // err = autoip_start(state_netif);
       // Report("autoip is %d\r\n", err);
    }
    else
    {
        Wlan_EtherPacketRecvRegisterCallback(roleid, NULL);
        if (roleid == WLAN_ROLE_STA)
        {
            dhcp_stop(state_netif);
            Report("DHCP stopped\r\n");
        }
        else if (roleid == WLAN_ROLE_AP)
        {
            dhcps_stop();
            Report("DHCP Server stopped\r\n");
        }

        Report("link_callback==DOWN\r\n");
    }
}



/**
 * @brief Forward an lwIP Ethernet packet to the selected WLAN role.
 * @param netif Valid output interface; staif selects STA, all others select AP.
 * @param p Borrowed non-NULL pbuf chain; ownership stays with lwIP.
 * @return ERR_MEM below the heap threshold or on temporary allocation failure;
 *  otherwise ERR_OK, even if the interface is down or WLAN sending fails.
 * @note Flattens chained payloads into allocated storage and borrows single
 *  payloads. Requires core context. The existing chain loop leaves currentPacket
 *  NULL and cleanup dereferences it; chained TX has a cleanup defect.
 */
err_t network_send(struct netif *netif, struct pbuf *p)
{
    WlanRole_e role;
    uint16_t total_len;
    struct pbuf *currentPacket = p;
    uint8_t *buff;
    uint16_t offset = 0;

    total_len = currentPacket->tot_len;

    if(osi_GetFreeHeapSize() < HEAP_THRESHOLD_FOR_TX)
    {
        return ERR_MEM;//no room for tx send
    }


    if(total_len != currentPacket->len)
    {
        buff = os_malloc(total_len);
        if(buff == NULL )
        {
            Report("\r\n allocation of TX buffer failed");
            return ERR_MEM;
        }
    //aggregate all packets
        do
        {
            os_memcpy(buff + offset,
                      currentPacket->payload,
                      currentPacket->len);
            offset += currentPacket->len;
            currentPacket = currentPacket->next;
        }
        while (currentPacket);
    }
    else
    {
        buff = currentPacket->payload;
    }


    if (netif_is_up(netif))
    {
        if (netif == &staif)
        {
            role = WLAN_ROLE_STA;
        }
        else
        {
            role = WLAN_ROLE_AP;
        }

        Wlan_EtherPacketSend(role,buff,total_len,0);
#ifdef CC33XX		
        osi_uSleep(10);
#endif // CC33XX		
    }
    if (buff != currentPacket->payload)
    {
        os_free(buff);
    }
    //done review
    return ERR_OK;
}


/**
 * @brief Mark an interface administratively and physically up.
 * @param newif Valid registered struct netif pointer.
 * @note Requires lwIP core context/lock. Sets Ethernet MTU; STATIC_IP builds
 *  first replace non-AP DHCP configuration with the compiled static address.
 */
void tcpinternal_network_set_up(void *newif)
{
    struct netif *nif = newif;
#ifdef STATIC_IP
    WlanRole_e role;
    if(nif == &staif)
    {
        role = WLAN_ROLE_STA;
    }
    else
    {
        role = WLAN_ROLE_AP;
    }

    if(WLAN_ROLE_AP != role)
    {
        struct ip4_addr ipaddr;
        struct ip4_addr netmask;
        struct ip4_addr gw;

        IP4_ADDR(&ipaddr, static_ip[0], static_ip[1], static_ip[2], static_ip[3]);
        IP4_ADDR(&netmask, static_mask[0], static_mask[1], static_mask[2], static_mask[3]);
        IP4_ADDR(&gw, static_gw[0], static_gw[1], static_gw[2], static_gw[3]);

        dhcp_release(nif);

        dhcp_stop(nif);

        netif_set_addr(nif, &ipaddr, &netmask, &gw);
    }
#endif
    netif_set_up(nif);
    nif->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;
    netif_set_link_up(nif);
}

/**
 * @brief Mark an interface administratively and physically down.
 * @param newif Valid struct netif pointer.
 * @note Requires core context/lock. Updates MTU; link callbacks stop DHCP.
 */
void tcpinternal_network_set_down(void *newif)
{
    struct netif *nif = newif;
    netif_set_down(nif);
    nif->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;
    netif_set_link_down(nif);

}

/**
 * @brief Initialize station netif callbacks, addressing support and flags.
 * @param newif Interface being added by netif_add().
 * @return Always 0.
 * @note Core-context initializer. Installs shared DHCP/AutoIP storage and signals
 *  staRoleupSyncObj when present; does not itself set link up.
 */
signed char _role_sta_up(struct netif *newif)
{
    netif_set_status_callback(newif, status_callback);
    netif_set_link_callback(newif, link_callback);
    autoip_set_struct(newif, &netif_autoip);
    dhcp_set_struct(newif, &netif_dhcp);

    newif->name[0] = 's';
    newif->name[1] = 't';

    newif->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;

    /* Populate the Driver Interface Functions. */
    newif->remove_callback      = NULL;
    newif->output               = etharp_output;
    newif->linkoutput           = network_send;
    newif->flags               |=  NETIF_FLAG_BROADCAST |
                                   NETIF_FLAG_ETHARP |
                                   NETIF_FLAG_IGMP;

    if (app_CB.CON_CB.staRoleupSyncObj != NULL)
    {
        //Signal to application that Wlan_RoleUp done
        osi_SyncObjSignal(&app_CB.CON_CB.staRoleupSyncObj);
    }
    return 0;
}

/**
 * @brief Initialize AP netif callbacks, Ethernet output and flags.
 * @param newif Interface being added by netif_add().
 * @return Always 0.
 * @note Core-context initializer; does not attach STA DHCP/AutoIP storage.
 */
signed char _role_ap_up(struct netif *newif)
{
    netif_set_status_callback(newif, status_callback);
    netif_set_link_callback(newif, link_callback);
    //autoip_set_struct(newif, &netif_autoip);
    //dhcp_set_struct(newif, &netif_dhcp);

    newif->name[0] = 'a';
    newif->name[1] = 'p';

    newif->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;

    /* Populate the Driver Interface Functions. */
    newif->remove_callback      = NULL;
    newif->output               = etharp_output;
    newif->linkoutput           = network_send;
    newif->flags               |=  NETIF_FLAG_BROADCAST |
                                   NETIF_FLAG_ETHARP |
                                   NETIF_FLAG_IGMP;

    return 0;
}

/**
 * @brief Register the station interface if it is not administratively up.
 * @param ctx Unused callback context.
 * @note Requires core context/lock. Clears staif, adds it with zero IPv4 values
 *  and makes it default. netif_add() failure is not checked. The up-state guard
 *  does not distinguish an already registered but down interface.
 */
void tcpip_network_stack_add_if_sta(void *ctx)
{
    ip4_addr_t ipaddr, netmask, gw;

    struct netif *pNetIf = &staif;
    if (!netif_is_up(pNetIf))
    {
        memset(pNetIf, 0, sizeof(struct netif));

        ip4_addr_set_zero(&gw);
        ip4_addr_set_zero(&ipaddr);
        ip4_addr_set_zero(&netmask);

        netif_add(pNetIf, &ipaddr, &netmask, &gw, NULL, _role_sta_up, tcpip_input);
        netif_set_default(pNetIf);
    }
}

/**
 * @brief Bring down and remove the station interface and clear its ARP entries.
 * @param ctx Unused callback context.
 * @note Requires core context/lock and an initialized staRoledownSyncObj, which
 *  is signaled after removal.
 */
void tcpip_network_stack_remove_if_sta(void *ctx)
{
    struct netif *pNetIf = &staif;
    tcpinternal_network_set_down(pNetIf); 
    etharp_cleanup_netif(pNetIf);
    netif_remove(pNetIf);
    osi_SyncObjSignal(&app_CB.CON_CB.staRoledownSyncObj);
}

/**
 * @brief Register the AP interface while preserving its configured IPv4 tuple.
 * @param ctx Unused callback context.
 * @note Requires core context/lock. Uses the up-state guard and does not check
 *  netif_add() success; caller must manage registration lifecycle.
 */
void tcpip_network_stack_add_if_ap(void *ctx)
{
    ip4_addr_t ipaddr, netmask, gw;

    struct netif *pNetIf = network_get_ap_if();
    if (!netif_is_up(pNetIf))
    {
        ipaddr.addr = pNetIf->ip_addr.addr;
        netmask.addr = pNetIf->netmask.addr;
        gw.addr = pNetIf->gw.addr;

        os_memset(pNetIf, 0, sizeof(struct netif));

        netif_add(pNetIf, &ipaddr, &netmask, &gw, NULL, _role_ap_up, tcpip_input);
        netif_set_addr(pNetIf, &ipaddr, &netmask, &gw);
        netif_set_ipaddr(pNetIf, &ipaddr);
    }
}

/**
 * @brief Bring down and remove the AP interface.
 * @param ctx Unused callback context.
 * @note Requires core context/lock; static apif storage remains allocated.
 */
void tcpip_network_stack_remove_if_ap(void *ctx)
{
    struct netif *pNetIf = &apif;
    tcpinternal_network_set_down(pNetIf);
    netif_remove(pNetIf);
}

/* This function initializes this lwIP test. When NO_SYS=1, this is done in
 * the main_loop context (there is no other one), when NO_SYS=0, this is done
 * in the tcpip_thread context */
/**
 * @brief Signal completion of tcpip_init() to its waiting caller.
 * @param arg Non-NULL pointer to the caller's live sys_sem_t.
 * @note Called by the TCP/IP thread; does not retain the semaphore pointer.
 */
void tcpip_network_internal_init(void * arg)
{ /* remove compiler warning */
  sys_sem_t *init_sem;
  LWIP_ASSERT("arg != NULL", arg != NULL);
  init_sem = (sys_sem_t*)arg;
  sys_sem_signal(init_sem);
}

/*************************** network API *****************************/

/**
 * @brief Return the station interface storage.
 * @return Borrowed pointer to static staif, regardless of registration/link state.
 * @note Do not free it; mutations require lwIP core synchronization.
 */
void * network_get_sta_if()
{
    return (void *)&staif;
}

/**
 * @brief Return the access-point interface storage.
 * @return Borrowed pointer to static apif, regardless of registration/link state.
 * @note Do not free it; mutations require lwIP core synchronization.
 */
void * network_get_ap_if()
{
    return (void *)&apif;
}


/**
 * @brief Bring an interface up under the lwIP core lock.
 * @param newif Valid registered struct netif pointer.
 * @note Task-context wrapper around tcpinternal_network_set_up().
 */
void network_set_up(void *newif)
{
    //tcpip_callback(tcpinternal_network_set_up, newif);
    LOCK_TCPIP_CORE();

    tcpinternal_network_set_up(newif);

    UNLOCK_TCPIP_CORE();

}


/**
 * @brief Bring an up interface down under the lwIP core lock.
 * @param newif Valid non-NULL struct netif pointer.
 * @note The initial up-state test occurs before locking; callers must serialize
 *  interface lifecycle changes.
 */
void network_set_down(void *newif)
{    /* suspect need to move to when there is wlan connect event */
    if(netif_is_up((struct netif *)newif))
    {
        //tcpip_callback(tcpinternal_network_set_down, newif);

        LOCK_TCPIP_CORE();

        tcpinternal_network_set_down(newif);

        UNLOCK_TCPIP_CORE();
    }
}

/**
 * @brief Perform station registration under the lwIP core lock.
 * @note Task-context wrapper; requires network_stack_init() and a valid interface
 *  lifecycle. Does not return registration/removal status.
 */
void network_stack_add_if_sta()
{
    LOCK_TCPIP_CORE();

    tcpip_network_stack_add_if_sta(NULL);

    UNLOCK_TCPIP_CORE();
}

/**
 * @brief Perform station removal under the lwIP core lock.
 * @note Task-context wrapper; requires network_stack_init() and a valid interface
 *  lifecycle. Does not return registration/removal status.
 */
void network_stack_remove_if_sta()
{
    //tcpip_callback(tcpip_network_stack_remove_if_sta, NULL);
    LOCK_TCPIP_CORE();

    tcpip_network_stack_remove_if_sta(NULL);

    UNLOCK_TCPIP_CORE();
}

/**
 * @brief Perform AP registration under the lwIP core lock.
 * @note Task-context wrapper; requires network_stack_init() and a valid interface
 *  lifecycle. Does not return registration/removal status.
 */
void network_stack_add_if_ap()
{
    //tcpip_callback(tcpip_network_stack_add_if_ap, NULL);
    LOCK_TCPIP_CORE();

    tcpip_network_stack_add_if_ap(NULL);

    UNLOCK_TCPIP_CORE();
}

/**
 * @brief Perform AP removal under the lwIP core lock.
 * @note Task-context wrapper; requires network_stack_init() and a valid interface
 *  lifecycle. Does not return registration/removal status.
 */
void network_stack_remove_if_ap()
{
    //tcpip_callback(tcpip_network_stack_remove_if_ap, NULL);
    LOCK_TCPIP_CORE();

    tcpip_network_stack_remove_if_ap(NULL);

    UNLOCK_TCPIP_CORE();

}

/**
 * @brief Start the lwIP TCP/IP thread and wait for initialization completion.
 * @note Task-context startup call; creates, waits on and frees a local semaphore.
 *  Semaphore creation errors are not checked. Initialize once before netif APIs.
 */
void network_stack_init()
{
    sys_sem_t init_sem;
    sys_sem_new(&init_sem,0);
    tcpip_init(tcpip_network_internal_init,&init_sem);
    sys_sem_wait(&init_sem);
    sys_sem_free(&init_sem);
}

/**
 * @brief Store the AP IP-mode flag without changing the interface or DHCP server.
 * @param mode IP_DHCP or IP_STATIC; narrowed to uint8_t without validation.
 * @note Does not take the core lock; serialize configuration calls.
 */
void network_stack_set_ap_ip_mode(uint32_t mode)
{
    ap_ip_mode = mode;
}

/**
 * @brief Store the STA IP-mode flag without starting or stopping DHCP.
 * @param mode IP_DHCP or IP_STATIC; narrowed to uint8_t without validation.
 * @note Does not take the core lock; serialize configuration calls.
 */
void network_stack_set_sta_ip_mode(uint32_t mode)
{
    sta_ip_mode = mode;
}

/**
 * @brief Stop/release STA DHCP and apply static addressing.
 * @param ip IPv4 address in lwIP/network byte order.
 * @param netmask IPv4 mask in lwIP/network byte order.
 * @param gw IPv4 gateway in lwIP/network byte order.
 * @note Takes the core lock; requires a configured interface. Records IP_STATIC after updating the tuple.
 */
void network_stack_set_static_ip_if_sta(uint32_t ip, uint32_t netmask, uint32_t gw)
{
    ip4_addr_t ip_addr = { .addr = ip };
    ip4_addr_t netmask_addr = { .addr = netmask };
    ip4_addr_t gw_addr = { .addr = gw };
    struct netif *pNetIf = network_get_sta_if();

    if (pNetIf)
    {
        LOCK_TCPIP_CORE();

        dhcp_release(pNetIf);
        dhcp_stop(pNetIf);
        netif_set_addr(pNetIf, &ip_addr, &netmask_addr, &gw_addr);

        network_stack_set_sta_ip_mode(IP_STATIC);

        UNLOCK_TCPIP_CORE();
    }
}

/**
 * @brief Clear station addressing and restart its DHCP client under the core lock.
 * @note Stops/releases DHCP, clears the address tuple, restarts DHCP and records
 *  IP_DHCP. Requires initialized/registered STA; DHCP results are not checked.
 */
void network_stack_set_dynamic_ip_if_sta()
{
    struct netif *pNetIf = network_get_sta_if();
    if (pNetIf)
    {
        LOCK_TCPIP_CORE();

        dhcp_stop(pNetIf);
        dhcp_release(pNetIf);

        netif_set_addr(pNetIf, NULL, NULL, NULL);

        dhcp_start(pNetIf);

        network_stack_set_sta_ip_mode(IP_DHCP);

        UNLOCK_TCPIP_CORE();
    }
}

/**
 * @brief Apply the AP address tuple without changing its stored mode.
 * @param ip IPv4 address in lwIP/network byte order.
 * @param netmask IPv4 mask in lwIP/network byte order.
 * @param gw IPv4 gateway in lwIP/network byte order.
 * @note Takes the core lock; requires a configured interface. Does not start/stop the DHCP server or set ap_ip_mode.
 */
void network_stack_set_static_ip_if_ap(uint32_t ip, uint32_t netmask, uint32_t gw)
{
    ip4_addr_t ip_addr = { .addr = ip };
    ip4_addr_t netmask_addr = { .addr = netmask };
    ip4_addr_t gw_addr = { .addr = gw };
    struct netif *pNetIf = network_get_ap_if();

    if (pNetIf)
    {
        LOCK_TCPIP_CORE();

        netif_set_addr(pNetIf, &ip_addr, &netmask_addr, &gw_addr);
                
        UNLOCK_TCPIP_CORE();
    }
}

/**
 * @brief Apply the AP address tuple and record IP_DHCP.
 * @param ip IPv4 address in lwIP/network byte order.
 * @param netmask IPv4 mask in lwIP/network byte order.
 * @param gw IPv4 gateway in lwIP/network byte order.
 * @note Takes the core lock; requires a configured interface. Does not start a DHCP client or the AP DHCP server.
 * @return -1 for a missing AP interface, otherwise 0.
 */
int8_t network_stack_set_dynamic_ip_if_ap(uint32_t ip, uint32_t netmask, uint32_t gw)
{
    ip4_addr_t ip_addr = { .addr = ip };
    ip4_addr_t netmask_addr = { .addr = netmask };
    ip4_addr_t gw_addr = { .addr = gw };
    
    struct netif *state_netif = network_get_ap_if();

    if (state_netif == NULL)
    {
        return -1;
    }
    
    LOCK_TCPIP_CORE();

    netif_set_addr(state_netif, &ip_addr, &netmask_addr, &gw_addr);

    network_stack_set_ap_ip_mode(IP_DHCP);

    UNLOCK_TCPIP_CORE();

    return 0;
}


/**
 * @brief Start or stop the AP DHCP server under the core lock.
 * @param enable Nonzero starts at the AP address; zero stops the server.
 * @return -1 for a missing interface, otherwise 0 without checking server status.
 */
int8_t network_stack_set_dhcp_server_if_ap(int enable)
{
    struct netif *pNetIf = network_get_ap_if();

    if (pNetIf == NULL)
    {
        return -1;
    }
    
    LOCK_TCPIP_CORE();

    if (enable)
    {
        dhcps_start(pNetIf->ip_addr.addr, pNetIf);
    }
    else
    {
        dhcps_stop();
    }

    UNLOCK_TCPIP_CORE();

    return 0;
}

/**
 * @brief Read the selected interface's IPv4 tuple and recorded DHCP mode.
 * @param role WLAN_ROLE_STA or WLAN_ROLE_AP.
 * @param[out] ip Optional address output in lwIP/network byte order.
 * @param[out] netmask Optional mask output in lwIP/network byte order.
 * @param[out] gw Optional gateway output in lwIP/network byte order.
 * @param[out] dhcp Optional output: true for IP_DHCP, false for IP_STATIC;
 *  left untouched for other stored mode values.
 * @return 0 on lookup success, -1 for an unsupported role or missing interface.
 * @note Does not acquire the core lock; outputs need not form an atomic snapshot.
 *  Mode flags describe configuration, not current DHCP lease/server state.
 */
int8_t network_stack_get_if_ip(WlanRole_e role, uint32_t *ip, uint32_t *netmask, uint32_t *gw, uint32_t *dhcp)
{
    struct netif *intf = NULL;

    if (role == WLAN_ROLE_STA)
    {
        intf = (struct netif *)network_get_sta_if();
        if (dhcp)
        {
            if (sta_ip_mode == IP_DHCP)
            {
                *dhcp = true;
            }
            else if (sta_ip_mode == IP_STATIC)
            {
                *dhcp = false;
            }
        }
    }
    else if (role == WLAN_ROLE_AP)
    {
        intf = (struct netif *)network_get_ap_if();
        if (dhcp)
        {
            if (ap_ip_mode == IP_DHCP)
            {
                *dhcp = true;
            }
            else if (ap_ip_mode == IP_STATIC)
            {
                *dhcp = false;
            }
        }
    }
    else
    {
        return -1;
    }

    if (intf == NULL)
    {
        return -1;
    }

    if (ip)
    {
        *ip = netif_ip4_addr(intf)->addr;
    }
    if (netmask)
    {
        *netmask = netif_ip4_netmask(intf)->addr;
    }
    if (gw)
    {
        *gw = netif_ip4_gw(intf)->addr;
    }

    return 0;
}

/**
 * @brief Replace the single extra interface-address notification callback.
 * @param callback Function receiving role and IPv4 address in lwIP byte order;
 *  NULL disables notifications.
 * @note Called synchronously from status_callback() in core context for up
 *  interfaces, including zero addresses. Register during startup or serialize
 *  updates; callbacks should avoid blocking the core thread.
 */
void network_stack_register_extra_status_callback(void (*callback)(WlanRole_e, uint32_t))
{
    extra_status_callback = callback;
}
