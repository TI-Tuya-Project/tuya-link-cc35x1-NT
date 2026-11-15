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
 * @file syslog.c
 * @brief Adapt syslog and trace messages to the configured UART output path.
 *
 * Messages are formatted into a heap buffer, then passed to logger_write().
 * A nonzero UART_1 build setting selects a lazily opened UART2 instance 1;
 * otherwise output goes through Report(). This selection is compile-time,
 * not a runtime fallback when the dedicated UART is closed or fails to open.
 * openlog() and closelog() are compatibility stubs; priority/group filtering
 * is not implemented. Shared UART initialization is not locked in this module.
 */
/*******************************************************************************

    Included Header Files

********************************************************************************/
#include <stdarg.h>
#include "osi_kernel.h"
#include "errors.h"
#include "syslog.h"
#include "ti/drivers/UART2.h"

/************** Defines And Global Variables ***************/
UART2_Handle logger = NULL;
#define SYSLOG_STRING_LIMIT  256

/************** Functions Definitions ***************/
int32 logger_write(char *buf, uint32 len);


/**
 * @brief Format a message into a dynamically allocated character array.
 * @param pcFormat Valid printf-style format string.
 * @param arg Variable arguments matching pcFormat; formatting consumes this list.
 * @return NUL-terminated heap string owned by the caller, or NULL if allocation
 * fails. The caller must release a successful result with os_free().
 * @note Starts with 256 zeroed bytes (up to 255 characters plus the terminator).
 * If formatting does not fit or reports an error, doubles capacity with
 * os_realloc(), clears the buffer, and retries. The buffer is not a linked list
 * and its capacity is not capped by SYSLOG_STRING_LIMIT.
 * @warning Retries reuse the same va_list without va_copy(); portable repeated
 * formatting is not guaranteed. This comment describes the existing behavior.
 */
char* inner_report(const char *pcFormat, va_list arg)
{
    int iRet = 0;
    char        *pcTemp;
    char        *pcBuff;
    int iSize = 256;

    pcBuff = (char*)os_zalloc(iSize);
    if(pcBuff == NULL)
    {
        return NULL;
    }

    while(1)
    {
        iRet = vsnprintf(pcBuff, iSize, pcFormat, arg);

        if((iRet > -1) && (iRet < iSize))
        {
            break;
        }
        else
        {
            iSize *= 2;
            if((pcTemp = os_realloc(pcBuff, iSize)) == NULL)
            {
                os_free(pcBuff);
                return NULL;
            }
            else
            {
                memset(pcTemp, 0, iSize);
                pcBuff = pcTemp;
            }
        }
    }

    return pcBuff;
}


/**
 * @brief Send a mutable buffer through the compile-time-selected logger path.
 * @param[in,out] buf Writable buffer of at least len bytes when len is nonzero.
 * The final supplied byte is replaced with a NUL terminator before output.
 * @param len Byte count supplied to the backend, including the overwritten byte.
 * @return 0 for zero length; UART2's reported written count when UART_1 is
 * nonzero; otherwise len, without checking Report() output success.
 * @note With UART_1 enabled, the first nonempty write opens UART2 instance 1
 * at 115200 baud in blocking mode. Later calls reuse the handle. Open failure
 * loops forever; it does not fall back to Report(). Without UART_1, each
 * nonempty call emits a newline/carriage return through Report(), then buf.
 * @warning The Report branch passes buf as the format string; percent signs
 * in formatted message content can be interpreted again by Report().
 */
int32 logger_write(char *buf, uint32 len)
{
     if(len == 0)
     {
         return len;
     }
     buf[len-1] = '\0';
#if UART_1    
    if (logger == NULL)
    {
        UART2_Params params;
        UART2_Params_init(&params);
        params.baudRate = 115200;
        params.readReturnMode = UART2_ReadReturnMode_FULL;
        params.readMode = UART2_Mode_BLOCKING;
        params.writeMode = UART2_Mode_BLOCKING;
        logger = UART2_open(1, &params);
        if (logger == NULL)
        {
            while (1) {}
        }
    }

    uint32 written = 0;
    UART2_write(logger, buf, len, &written);

    return written;

#else
    Report("\n\r");
    Report(buf);
    return len;
#endif    
}



/**
 * @brief Compatibility stub; does not open or configure the logger.
 * @param ident Ignored identification string.
 * @param option Ignored logging options.
 * @param facility Ignored facility identifier.
 */
void openlog(const char *ident, int option, int facility)
{

}

/**
 * @brief Forward a printf-style message and its arguments to vsyslog().
 * @param priority Forwarded but ignored; no severity filtering is performed.
 * @param format Valid printf-style format string followed by matching arguments.
 */
void syslog(int priority, const char *format, ...)
{
    va_list list;
    va_start(list, format);
    vsyslog(priority, format, list);
    va_end(list);
}

/**
 * @brief Compatibility stub; leaves any lazily opened UART handle open.
 */
void closelog(void)
{

}

/**
 * @brief Format and output a message, then release its temporary heap buffer.
 * @param priority Ignored priority; all messages use the same output path.
 * @param format Valid printf-style format string.
 * @param ap Variable arguments matching format; consumed by inner_report().
 * @note Allocation failure silently drops the message. Passes at most 256
 * message bytes to logger_write(), then submits a separate newline buffer.
 * logger_write() replaces the last byte of each nonempty buffer with NUL,
 * so the last selected message character and the newline buffer's carriage
 * return are overwritten. Backend return values are ignored.
 */
void vsyslog(int priority, const char *format, va_list ap)
{
    char *pcBuff = inner_report(format, ap);
    if (!pcBuff)
    {
        return;
    }
    char newLine[] = "\n\r";

    logger_write(pcBuff, MIN(strlen(pcBuff),SYSLOG_STRING_LIMIT));
    logger_write(newLine, sizeof(newLine) - 1);
    os_free(pcBuff);
}


/**
 * @brief Format and output a trace message using the same policy as vsyslog().
 * @param group Ignored trace group; no group filtering is performed.
 * @param fmt Valid printf-style format string followed by matching arguments.
 * @note Frees the formatted buffer after output and silently drops messages on
 * allocation failure. Uses the same 256-byte output limit and final-byte
 * overwrite behavior documented for vsyslog().
 */
void gtrace_report(int group, char *fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    char *pcBuff = inner_report(fmt, list);
    va_end(list);

    if (!pcBuff)
    {
        return;
    }

    char newLine[] = "\n\r";

   
    logger_write(pcBuff, MIN(strlen(pcBuff),SYSLOG_STRING_LIMIT));
    logger_write(newLine, sizeof(newLine) - 1);
   
    os_free(pcBuff);
}

