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
 * @file lwip_iperf_tcp_client.c
 * @brief CC35XX raw-lwIP TCP throughput client and timed reporting.
 * Queues setup and reports to the TCP/IP thread; connection/ACK/poll callbacks
 * fill the send queue. Byte totals count queued bytes, not acknowledged delivery.
 * The shared send_buffer is borrowed without copying and must be initialized
 * and remain stable for the session lifetime.
 */

#include <string.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmd_parser.h"
#include "uart_term.h"
#include "errors.h"
#include "FreeRTOSConfig.h"
#include "osi_kernel.h"
#include "network_lwip.h"

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpbase.h"
#include "lwip_iperf_examples.h"


#ifdef CC35XX


#define IPERF_LWIP_CLIENT_DURATION_MS 10000 // Test duration (10 seconds)

#define IPERF_LWIP_MAX_FORMAT_RATE_LENGTH  20

#define SEND_BUFFER_SIZE 1460


extern session_conn_t iperf_session[];

// Forward declarations
static err_t iperflwip_tcp_session_conected(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t lwiperf_tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void iperflwip_client_tcp_init(void *param);
err_t iperflwip_tcp_client_tx(session_conn_t* session_con);

extern void format_bps(double bps, char *output, size_t size);
extern void iperflwip_tcp_stop(void *arg, uint8_t isError);
extern void iperflwip_tcp_err(void *arg, err_t err);
extern err_t lwiperf_tcp_poll(void *arg, struct tcp_pcb *tpcb);
static void iperflwip_client_tcp_os_timer_callback(union sigval sv);

extern unsigned char send_buffer[];


/**
 * @brief Create/bind a raw IPv4 PCB and request connection.
 * @param param Live reserved session with copied lwipConfig.
 * @note Core callback. Resets counters, binds any address/ephemeral port, disables
 *  Nagle and installs ACK/error/poll handlers. Setup failures clear is_running
 *  and close a created PCB; no completion status goes to the start caller.
 */
static void iperflwip_client_tcp_init(void *param)
{
    err_t err;

    session_conn_t* session_con = param;
    session_con->actualTestdurationMs = 0;
    session_con->actualNumOfDurations = 0;
    session_con->conn_pcb_tcp = NULL;
    session_con->os_timer = 0;
    session_con->total_bytes = 0;
    session_con->bytes_per_period = 0;
    session_con->poll_count = 0;
    session_con->previous_time = osi_GetTimeMS();
    session_con->start_time = osi_GetTimeMS();

    session_con->dest_ip.addr = htonl((unsigned int )session_con->lwipConfig.ipAddr.ipv4);

    session_con->conn_pcb_tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (session_con->conn_pcb_tcp == NULL) {
        Report("\n\riperflwip_client: ERROR ! Failed to create pcb\n");
        session_con->is_running = false;
        return;
    }

    err = tcp_bind(session_con->conn_pcb_tcp, IP4_ADDR_ANY4, 0);
    if(err != ERR_OK){
        Report("\n\riperflwip_client: ERROR ! tcp_bind, port is in use\n");
        tcp_close(session_con->conn_pcb_tcp);
        session_con->is_running = false;
        return;
    }

    tcp_nagle_disable(session_con->conn_pcb_tcp);
    tcp_arg(session_con->conn_pcb_tcp, session_con);
    tcp_err(session_con->conn_pcb_tcp, iperflwip_tcp_err);
    tcp_sent(session_con->conn_pcb_tcp, lwiperf_tcp_client_sent);
    tcp_poll(session_con->conn_pcb_tcp, lwiperf_tcp_poll, 100U);

    tcp_nagle_disable(session_con->conn_pcb_tcp);

    tcp_setprio(session_con->conn_pcb_tcp, TCP_PRIO_MAX);       


    err = tcp_connect(session_con->conn_pcb_tcp, &session_con->dest_ip, session_con->lwipConfig.destOrLocalPortNumber, iperflwip_tcp_session_conected);
    if (err != ERR_OK) {
        Report("\n\riperflwip_client: ERROR ! TCP_connection failed: %d\n", err);
        tcp_close(session_con->conn_pcb_tcp);
        session_con->is_running = false;
    }
}

// Connection established
/**
 * @brief Start sending and timed reporting after TCP connection.
 * @param arg Valid session callback argument.
 * @param tpcb Connected PCB expected to match the stored pointer.
 * @param err Connection status from lwIP.
 * @return err after closing on failure, otherwise ERR_OK.
 * @note Core context. PCB mismatch only logs. Chooses interval from period or
 *  timeout; the large-timeout fallback multiplies IPERF_LWIP_CLIENT_DURATION_MS
 *  by 1000. Timer errors do not prevent sending; an unarmed timer cannot end
 *  the test.
 */
err_t iperflwip_tcp_session_conected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    session_conn_t* session_con = arg;
    struct sigevent         event;
    char ip[INET_ADDRSTRLEN] = {0};
    uint32_t ipAddress = 0;


    if(session_con->conn_pcb_tcp != tpcb)
    {
        Report("\n\riperflwip_client: Connect error\n");
    }

    if (err != ERR_OK) {
        Report("\n\riperflwip_client: Connect error\n");
        tcp_close(session_con->conn_pcb_tcp);
        return err;
    }

    ipAddress = htonl((unsigned int )session_con->lwipConfig.ipAddr.ipv4);
    inet_ntop(AF_INET, &ipAddress, ip, INET_ADDRSTRLEN);
    Report("\n\riperflwip_client: Connected to %s port %d\n\r", ip,
           session_con->conn_pcb_tcp->local_port);


    session_con->total_bytes = 0;
    session_con->is_running = true;

    if ( session_con->lwipConfig.period)
    {
        session_con->actualTestdurationMs = session_con->lwipConfig.period * 1000;
    }
    else if(session_con->lwipConfig.timeout >= (uint32_t)99999)
    {
        session_con->actualTestdurationMs = IPERF_LWIP_CLIENT_DURATION_MS * 1000;
    }
    else
    {
        session_con->actualTestdurationMs = session_con->lwipConfig.timeout*1000;//sec to ms
    }

    event.sigev_notify = SIGEV_THREAD;
    event.sigev_value.sival_ptr = session_con;
    event.sigev_notify_function = iperflwip_client_tcp_os_timer_callback;
    event.sigev_notify_attributes = NULL;


    // Start test timer
    timer_create(CLOCK_REALTIME, &event, &session_con->os_timer);


    if (session_con->os_timer != 0) {
        struct itimerspec       its = {0};
        its.it_value.tv_sec = (session_con->actualTestdurationMs / 1000);
        its.it_value.tv_nsec = (session_con->actualTestdurationMs % 1000)*1000000; // expiration
        timer_settime(session_con->os_timer, 0, &its, NULL);
    }
    else {
        Report("\n\riperflwip_client: ERROR ! fail to create timer");
    }

    session_con->previous_time = osi_GetTimeMS();
    session_con->start_time = osi_GetTimeMS();
    iperflwip_tcp_client_tx(session_con);

    return ERR_OK;
}

//function is been called when tcp-ack is received
/**
 * @brief Reset idle count and refill the TCP queue after an ACK.
 * @param arg Valid session pointer.
 * @param tpcb Expected session PCB; mismatch triggers LWIP_ASSERT.
 * @param len Acknowledged byte count, unused.
 * @return Transmit helper result (currently always ERR_OK).
 * @note Executes in TCP/IP context.
 */
static err_t lwiperf_tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    session_conn_t* session_con = arg;


    LWIP_ASSERT("iperflwip_client: invalid conn", session_con->conn_pcb_tcp == tpcb);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

    session_con->poll_count = 0;


    return iperflwip_tcp_client_tx(session_con);
}


// Send function: chunked sending as fast as possible
/**
 * @brief Queue test payload while TCP send-buffer capacity remains.
 * @param session_con Live session with a non-NULL connected PCB.
 * @return Always ERR_OK, including tcp_write() failure.
 * @note Core context. Chunk length defaults/caps to 1460 after narrowing the
 *  configured size to u16_t. Successful writes update queued-byte counters.
 *  Without TCP_WRITE_FLAG_COPY, shared send_buffer must stay unchanged until
 *  ACKed. No explicit tcp_output() is issued; later ACK/poll callbacks retry.
 */
err_t iperflwip_tcp_client_tx(session_conn_t* session_con) {
    err_t err = ERR_OK;
    struct tcp_pcb * tpcb = session_con->conn_pcb_tcp;

    u16_t packetLength;
    if (session_con->lwipConfig.packetLength > 0) 
    {
        packetLength = (u16_t)session_con->lwipConfig.packetLength;

        if (packetLength > SEND_BUFFER_SIZE)
        {
            packetLength = SEND_BUFFER_SIZE;
        }
    }
    else
    {
        // Default length
        packetLength = SEND_BUFFER_SIZE;
    }

    while (err == ERR_OK && tcp_sndbuf(tpcb) > 0) {
        u16_t len = tcp_sndbuf(tpcb);

        if (len > packetLength)
        {
            // Limit len to packetLength set
            len = packetLength;
        }


        err = tcp_write(tpcb, send_buffer, len, 0);
        if (err == ERR_OK) {
            session_con->packet_count++;
            session_con->total_bytes += len;
            session_con->bytes_per_period += len;
            // You can add optional logic here to track bytes sent
        } else if (err == ERR_MEM) {
            // Can't send more now, will try again in sent callback
        }
    }
    //tcp_output(tpcb);  // Flush the data
    return ERR_OK;
}


// Timer expired -> test done
/**
 * @brief Print interval throughput, rearm the timer or close a finished test.
 * @param arg Live session pointer, invoked through tcpip_callback().
 * @note Rates use queued bytes and elapsed OSI milliseconds. Continues according
 *  to timeout policy unless aborted; otherwise prints totals and closes.
 *  Inactive/missing-PCB paths only log completion; timer rearm errors are ignored.
 */
static void iperflwip_report(void* arg)
{
    char ratestr[IPERF_LWIP_MAX_FORMAT_RATE_LENGTH];
    session_conn_t* session_con = (session_conn_t*)arg;
    double secondsFromStart, durationInSecond;
    uint32_t current_time;
    double bps = 0.0;

    if (session_con->is_running && session_con->conn_pcb_tcp != NULL) {

        session_con->actualNumOfDurations++;

        if(session_con->lwipConfig.period)
        {
            current_time = osi_GetTimeMS();
            uint32_t delta_ms = current_time - session_con->previous_time;
            durationInSecond = (double)delta_ms / 1000.0;
            if(durationInSecond){
                bps = ((double)session_con->bytes_per_period * 8.0) / durationInSecond;
            }

            format_bps(bps,ratestr, sizeof(ratestr));
            Report("\n\r[%d] %s",session_con->process_num,ratestr);

        }

        session_con->previous_time = osi_GetTimeMS();
        session_con->bytes_per_period = 0;

        if (!session_con->is_req_to_abort_test && ((session_con->lwipConfig.timeout >= 99999) ||
                (session_con->lwipConfig.timeout*1000 > (session_con->actualTestdurationMs* session_con->actualNumOfDurations))))
        {
            //trigger the timer again
            struct itimerspec       its = {0};
            its.it_value.tv_sec = (session_con->actualTestdurationMs / 1000);
            its.it_value.tv_nsec = (session_con->actualTestdurationMs % 1000)*1000000; // expiration
            timer_settime(session_con->os_timer, 0, &its, NULL);
        }
        else
        {
            uint32_t  curr_time = osi_GetTimeMS();
            secondsFromStart = ((double)(curr_time - session_con->start_time));
            Report("\n\riperflwip: [%d] TCP client Test finished",session_con->process_num);

            if(secondsFromStart > 0)
            {
                secondsFromStart=secondsFromStart/1000.0;
                bps = ((session_con->total_bytes * 8.0)/secondsFromStart);
                format_bps(bps,ratestr, sizeof(ratestr));
            }
            else
            {
                bps = 0;
                snprintf(ratestr, sizeof(ratestr), "0 bps");
            }
            Report("\n\riperf TCP client :  %lu total bytes duration :%lu sec", (unsigned long )session_con->total_bytes,(unsigned long )secondsFromStart);
            Report("\t %s \n", ratestr);
            iperflwip_tcp_close(session_con);
        }
    }
    else
    {
        Report("\n\riperflwip: TCP client Test finished\n");
    }

}


/**
 * @brief Queue a report from a POSIX timer notification thread.
 * @param sv sival_ptr holds a session live until the report finishes.
 * @note Does not operate on raw TCP state directly; queue errors are ignored.
 */
static void iperflwip_client_tcp_os_timer_callback(union sigval sv)
{
    tcpip_callback(iperflwip_report, sv.sival_ptr);
}

/**
 * @brief Reserve a shared session and queue client setup.
 * @param args Valid RecvCmd_t pointer copied before return.
 * @return 0 after queuing, -1 if no candidate slot was found.
 * @note Requires initialized lwIP and send_buffer; serialize allocation. Queue
 *  success is unchecked and the 50-microsecond sleep is not a connection barrier.
 *  The search includes index IPERF_LWIP_MAX_NUM_OF_IPERF_SESSIONS even though
 *  the array has that many elements; a full table can cause an out-of-bounds
 *  access. Options are not validated here.
 */
int32_t iperflwip_tcp_client_start(void* args)
{
    int i;
    uint8_t found = FALSE;

    //find available iperf TCP server
    for(i=1; i< IPERF_LWIP_MAX_NUM_OF_IPERF_SESSIONS+1; i++)
    {
        if(!iperf_session[i].is_running)
        {
            found = TRUE;//found not running process
            os_memset(&iperf_session[i], 0, sizeof(iperf_session[i]));
            iperf_session[i].is_server = 0;
            iperf_session[i].is_udp = 0;
            iperf_session[i].process_num = i;
            iperf_session[i].iperf_reportFunc = iperflwip_report;
            iperf_session[i].is_req_to_abort_test = 0;
            iperf_session[i].is_stop_due_traffic_error = 0;
            iperf_session[i].packet_count = 0;
            iperf_session[i].is_running = true;
            break;
        }
    }

    if(found)
    {
        Report("\r\nTo stop the TCP client process , iperf_stop -n %d ", i);
        os_memcpy((void *)&iperf_session[i].lwipConfig,args, sizeof(RecvCmd_t));
        tcpip_callback(iperflwip_client_tcp_init,(void *) (void *)&iperf_session[i]);
        os_sleep(0,50);//give time to the iperflwip_server_tcp_init to be trigger
        return 0;
    }
    else
    {
        Report("\n\riperflwip_client: Num of sessions exceeded, max num = %d ",IPERF_LWIP_MAX_NUM_OF_IPERF_SESSIONS);
        return -1;
    }

}


#endif
