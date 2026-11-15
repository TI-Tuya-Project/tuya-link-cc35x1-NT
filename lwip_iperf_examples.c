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
 * @file lwip_iperf_examples.c
 * @brief CC35XX iperf CLI dispatch and shared TCP lifecycle helpers.
 * Owns the static session table and stable test payload; dispatches to TCP/UDP
 * backends. Raw TCP actions require core context. Stop/report callbacks complete
 * shutdown asynchronously. Session allocation is not locked; callers must manage
 * indices and keep callback arguments live.
 */

#include "lwip_iperf_examples.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "cmd_parser.h"
#include "uart_term.h"
#include "errors.h"
#include "FreeRTOSConfig.h"
#include "osi_kernel.h"
#ifdef CC35XX
#include "wlan_if.h"
#include "network_lwip.h"
#endif // CC35XX
#include "cmd_parser.h"

#ifdef CC35XX

extern int32_t iperflwip_tcp_server_start(void* args);
extern int32_t iperflwip_tcp_client_start(void* args);
extern err_t iperflwip_tcp_client_tx(session_conn_t* session_con);
extern int32_t iperflwip_udp_client_start(void* args);
extern void udp_client_stop(session_conn_t* session_con);
extern int32_t iperflwip_udp_server_start(void* args);
extern void udp_server_stop(session_conn_t* session_con);


int32_t iperflwip_stop(void* args);


#define MAX_SEND_BUFFER_SIZE (1470)
session_conn_t iperf_session[IPERF_LWIP_MAX_NUM_OF_IPERF_SESSIONS] = {0};
unsigned char send_buffer[MAX_SEND_BUFFER_SIZE];
uint8_t send_buffer_initialized = 0;


void iperflwip_tcp_stop_from_cb(void *arg);


/**
 * @brief Parse and dispatch an iperf command to its requested backend.
 * @param arg Valid argument string accepted by ParseTestIperfCmd().
 * @return -1 on parse failure, otherwise 0; backend startup status is discarded.
 * @note Initializes shared send_buffer once. Parsed options are stack-local;
 *  backends must copy them before returning.
 */
int32_t cmdTestIperfCallback(void *arg)
{
    int32_t ret = 0;
    RecvCmd_t IperfCmdParams;
    int i;

    if(!send_buffer_initialized)
    {
        for(i=0;i<MAX_SEND_BUFFER_SIZE;i++)
        {
            send_buffer[i]= (uint8_t)i;
        }
        send_buffer_initialized = 1;
    }

     /* Call the command parser */
     memset(&IperfCmdParams, 0x0, sizeof(RecvCmd_t));
     ret = ParseTestIperfCmd(arg, &IperfCmdParams);

     if (ret < 0)
     {
         Report("\n\r wrong choices");
         return (-1);
     }
     uint8_t protocol = IperfCmdParams.udpFlag? SOCK_DGRAM : SOCK_STREAM ;

     if (IperfCmdParams.server)
     {
         if(protocol == SOCK_STREAM)//TCP server
         {
             ret = iperflwip_tcp_server_start((void *)&IperfCmdParams);
         }
         else
         {
             ret = iperflwip_udp_server_start((void *)&IperfCmdParams);
         }

     }
     else
     {
         if(protocol == SOCK_STREAM)//TCP server
         {
             ret = iperflwip_tcp_client_start((void *)&IperfCmdParams);
         }
         else
         {
             ret = iperflwip_udp_client_start((void *)&IperfCmdParams);
         }
     }

     return (0);
 }


/**
 * @brief Parse a stop command and forward its process number.
 * @param arg Valid argument string accepted by ParseStopTestIperfCmd().
 * @return -1 on parse failure, otherwise iperflwip_stop() result.
 */
int32_t cmdStopTestIperfCallback(void *arg)
{
    stopCmd_t stopCmd;
    int32_t ret = 0;

    memset(&stopCmd, 0x0, sizeof(stopCmd_t));
    ret = ParseStopTestIperfCmd(arg, &stopCmd);

    if (ret < 0)
    {
        return (-1);
    }

    Report("\r\nRequest to Stop iperf process number :%d", stopCmd.processNum );
    ret = iperflwip_stop(&stopCmd.processNum);
    return ret;

}

/**
 * @brief Request shutdown of a running session or list active entries.
 * @param args Valid uint32_t process-number pointer; value must index the table.
 * @return 0 after requesting a running session stop; otherwise -1 after listing.
 * @note Queues TCP stop to lwIP or calls the UDP backend helper. The subsequent
 *  50-microsecond sleep does not confirm completion. No index validation here.
 *  Listing starts at 1 and excludes the upper bound.
 */
int32_t iperflwip_stop(void* args)
{
    int process_num = *((uint32_t *)args);
    int i;

    if( iperf_session[process_num].is_running )
    {
        if(!iperf_session[process_num].is_udp)//tcp client or server
        {
            session_conn_t *client = &iperf_session[process_num];
            tcpip_callback(iperflwip_tcp_stop_from_cb, (void *)client);
            os_sleep(0,50);//give time to the iperf_server_init to be trigger
            Report("\r\niperf tcp process number :%d stopped! ", process_num );
            return 0;
        }
        else
        {
            if(iperf_session[process_num].is_server)//udp server
            {
                session_conn_t *client = &iperf_session[process_num];
                udp_server_stop(client);
                os_sleep(0,50);//give time to the iperf_server_init to be trigger
                Report("\r\niperf udp server process number :%d stopped! ", process_num );
                return 0;
            }
            else //udp client
            {
                session_conn_t *client = &iperf_session[process_num];
                udp_client_stop(client);
                os_sleep(0,50);//give time to the iperf_server_init to be trigger
                Report("\r\niperf udp client process number :%d stopped! ", process_num );
                return 0;
            }
        }
    }
    else
    {
        if(process_num != 0)
        {
            Report("\r\nError, nothing to stop");
        }
        Report("\r\nlist of running processes:");
        for(i=1;i<IPERF_LWIP_MAX_NUM_OF_IPERF_SESSIONS;i++)
        {
            if(iperf_session[i].is_running)
            {
                Report("\r\n process num : %d is_server:%d is_udp:%d",
                        i,
                        iperf_session[i].is_server,
                        iperf_session[i].is_udp) ;
            }
        }
        return -1;
    }
}



/*!
 \brief          Prints Receive command help menu.

 \param          arg       -   Points to command line buffer.

 \return         Upon successful completion, the function shall return 0.


 */
/**
 * @brief Print iperf syntax, description and option details.
 * @param arg Unused command argument.
 * @return Always 0.
 */
int32_t printTestIperfUsage(void *arg)
{
    Report(lineBreak);
    Report(usageStr);
    Report(TestIperf);
    Report(recvTestIperfUsage2Str);
    Report(descriptionStr);
    Report(recvTestIperfDetailsStr);

    Report(recvTestIperf_s_optionDetailsStr);
    Report(recvTestIperf_c_optionDetailsStr);
    Report(recvTestIperf_u_optionDetailsStr);
    Report(recvTestIperf_p_optionDetailsStr);
    Report(recvTestIperf_i_optionDetailsStr);
    Report(recvTestIperf_t_optionDetailsStr);
    Report(recvTestIperf_b_optionDetailsStr);
    Report(recvTestIperf_B_optionDetailsStr);
    Report(recvTestIperf_l_optionDetailsStr);
    Report(lineBreak);
    return (0);
}



/**
 * @brief Print stop syntax and the process-number option.
 * @param arg Unused command argument.
 * @return Always 0.
 */
int32_t printStopTestIperfUsage(void *arg)
{
    Report(lineBreak);
    Report(usageStr);
    Report(StopTestIperf);
    Report(recvStopTestIperfUsage2Str);
    Report(descriptionStr);
    Report(recvStopTestIperfDetailsStr);

    Report(recvStopTestIperf_n_optionDetailsStr);
    Report(lineBreak);
    return (0);
}



/**
 * @brief Count idle polls and retry client queue filling.
 * @param arg Valid session callback argument.
 * @param tpcb Unused polling PCB.
 * @return Always ERR_OK; transmission results are ignored.
 * @note Core context. Requests stop at IPERF_LWIP_TCP_MAX_IDLE_SEC polls; actual
 *  elapsed time depends on the registered poll interval, despite the name.
 */
err_t lwiperf_tcp_poll(void *arg, struct tcp_pcb *tpcb)
{
    session_conn_t *session_con = (session_conn_t *)arg;
    //LWIP_ASSERT("iperflwip_client: pcb mismatch", session_con->conn_pcb_tcp == tpcb);
    LWIP_UNUSED_ARG(tpcb);
    if (++session_con->poll_count >= IPERF_LWIP_TCP_MAX_IDLE_SEC) {
        Report("\n\riperflwip: idle connection close connection  is server:%d port number:%d\n",
                session_con->lwipConfig.server,session_con->lwipConfig.destOrLocalPortNumber);
        iperflwip_tcp_stop(session_con,0);
        return ERR_OK; /* lwiperf_tcp_close frees conn */
    }

    if (!session_con->lwipConfig.server) {
        iperflwip_tcp_client_tx(session_con);
    }

    return ERR_OK;
}

/**
 * @brief Request a normal TCP stop from the TCP/IP thread.
 * @param arg Live session pointer passed to iperflwip_tcp_stop().
 */
void iperflwip_tcp_stop_from_cb(void *arg)
{
    iperflwip_tcp_stop(arg, 0);
}

/**
 * @brief Stop the timer and queue backend reporting/cleanup.
 * @param arg Valid session pointer with an installed iperf_reportFunc.
 * @param isError Nonzero marks a traffic error; zero requests normal shutdown.
 * @note Ignores inactive sessions. Sets abort/error flags but leaves is_running
 *  true until cleanup. Repeated calls may queue multiple reports. Timer and
 *  callback failures are ignored; use serialized core context.
 */
void iperflwip_tcp_stop(void *arg, uint8_t isError)
{
    session_conn_t *session_con = arg;

    if(session_con->is_running == 0)
    {
        return;
    }
    if (session_con->os_timer != 0) {

        struct itimerspec       its = {0};
        //stop the timer
        timer_settime(session_con->os_timer, 0, &its, NULL);
        timer_delete(session_con->os_timer);
        session_con->os_timer = 0;
    }

    session_con->is_req_to_abort_test = 1;
    session_con->is_stop_due_traffic_error = isError;
    tcpip_callback(session_con->iperf_reportFunc,session_con);
}

/**
 * @brief Mark a TCP session inactive and release timer/PCB references.
 * @param arg Valid session pointer; static session storage is not freed.
 * @note Core context. Detaches client callbacks and aborts on normal stop. Error
 *  paths skip abort because lwIP may have freed the PCB. Server paths close
 *  their stored PCBs. tcp_close() results are ignored; cleared references do
 *  not confirm graceful completion.
 */
void iperflwip_tcp_close(void *arg)
{
    
    session_conn_t *session_con = arg;
    uint8_t is_running = session_con->is_running;
    uint8_t isError = session_con->is_stop_due_traffic_error;

    session_con->is_running = false;

    if (session_con->os_timer != 0) {

        struct itimerspec       its = {0};
        //stop the timer
        timer_settime(session_con->os_timer, 0, &its, NULL);
        timer_delete(session_con->os_timer);
        session_con->os_timer = 0;
    }


    if ((!session_con->lwipConfig.server) && session_con->conn_pcb_tcp != NULL) {
        if(is_running)
        {
            tcp_arg(session_con->conn_pcb_tcp, NULL);
            tcp_sent(session_con->conn_pcb_tcp, NULL);
            tcp_poll(session_con->conn_pcb_tcp, NULL, 0U);
            tcp_err(session_con->conn_pcb_tcp, NULL);
            tcp_recv(session_con->conn_pcb_tcp, NULL);
            if(!isError)
            {
                tcp_abort(session_con->conn_pcb_tcp);
            }
        }
        session_con->conn_pcb_tcp = NULL;
    }
    else if (session_con->lwipConfig.server)
    {
        if(is_running)
        {
            /* no conn pcb, this is the listener pcb */
            if(session_con->conn_pcb_tcp != NULL)
            {
                if(!isError)
                {
                    tcp_close(session_con->conn_pcb_tcp);//close the listen pcb
                } 
                session_con->conn_pcb_tcp = NULL;
            }

            if(session_con->server_pcb)
            {
                tcp_close(session_con->server_pcb);//close the pcb
                session_con->server_pcb = NULL;
            }

        }
    }

}

// Error callback
/**
 * @brief Log a raw TCP error and request error-mode reporting.
 * @param arg Valid session callback argument.
 * @param err Error code from lwIP.
 * @note Core context; ignores inactive sessions. The error flag suppresses a
 *  normal abort of a PCB that lwIP may already have freed.
 */
void iperflwip_tcp_err(void *arg, err_t err)
{
    session_conn_t *session_con = (session_conn_t *)arg;
    if(session_con->is_running == 0)
    {
        //session is not running
        return;
    }
    Report("\n\riperflwip: tcp error num: %d, is server:%d port number:%d\n",
           err, session_con->lwipConfig.server,session_con->lwipConfig.destOrLocalPortNumber);
    iperflwip_tcp_stop(session_con, 1);
}

/**
 * @brief Find an exact IPv4 address match in the local netif list.
 * @param addr Valid address in lwIP representation.
 * @return 0 for a match, otherwise -1.
 * @note Does not require the matched interface to be up. Caller supplies core
 *  context/locking; this helper does not lock the list.
 */
int8_t is_ip_addr_in_net_list(const ip_addr_t *addr)
{
    struct netif *netif = netif_list;

    while (netif)
    {
        if (ip4_addr_cmp(&(netif->ip_addr), addr))
        {
            return 0;
        }
        netif = netif->next;
    }

    return -1;
}


#endif
