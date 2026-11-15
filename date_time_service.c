/**
 * @file date_time_service.c
 * @brief UTC clock access, readiness tracking and calendar conversion.
 * Wraps OSI wall-clock seconds and uses Report() for display. Conversions use
 * unsigned 32-bit seconds with no timezone/DST or leap-second adjustment.
 * Shared readiness and clock changes are not locked in this module.
 */
#include "date_time_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "osi_kernel.h"
#include "task.h"




uint8_t g_dateTimeInitDone = 0;


struct tm;


/**
 * @brief Mark seconds reads ready without setting or synchronizing the clock.
 * @return Always 0.
 */
int32_t datetime_init()
{
    g_dateTimeInitDone = 1;
    return 0;
}

/**
 * @brief Clear the readiness flag without resetting the underlying clock.
 * @return Always 0.
 * @note Setters and calendar conversions do not check this flag.
 */
int32_t datetime_deinit()
{
    g_dateTimeInitDone = 0;
    return 0;
}

/* return second since epoch time :00:00 Jan 1 1970*/
/**
 * @brief Read Unix epoch seconds from OSI when initialized.
 * @return Unsigned seconds, or 0 with a diagnostic if not ready. Zero is also
 *  a valid epoch value; there is no separate error status.
 */
uint32_t datetime_secondsGet(void)
{
    struct timespec ts = {0};


    if(!g_dateTimeInitDone)
    {
        Report("\n\rdatetime was not initialize");
        return 0;
    }
    ts.tv_sec = osi_GetDateTimeS();

    return(ts.tv_sec);
}

/* set second from epoch time :00:00 Jan 1 1970*/
/**
 * @brief Set the OSI wall clock in whole seconds.
 * @param newtime Seconds since 1970-01-01 00:00:00 UTC.
 * @note Does not check readiness or report clock-setting errors.
 */
void datetime_SecondsSet(uint32_t newtime)
{
    osi_SetDateTimeS(newtime);
}


/**
 * @brief Print the C runtime wall clock as a UTC calendar timestamp.
 * @note Uses time()/gmtime(), not datetime_secondsGet(). Conversion errors are
 *  not checked; gmtime() may return shared static storage. Requires Report().
 */
void datetime_printCurTime()
{

    time_t t = time(&t);
    struct tm *currentTime = gmtime(&t);

    // Format 1: YYYY-MM-DD HH:MM:SS
    Report("\n\rCurrent UTC time: ");
    Report("%04d-%02d-%02d",currentTime->tm_year + 1900, currentTime->tm_mon + 1, currentTime->tm_mday);
    Report("T%02d:%02d:%02d\n",currentTime->tm_hour, currentTime->tm_min, currentTime->tm_sec);
}

/**
 * @brief Apply the Gregorian leap-year rule.
 * @param y Full calendar year.
 * @return true for years divisible by 4 except centuries not divisible by 400.
 */
static bool datetime_isLeapYear(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
/* convert date and time to epoch time */
/**
 * @brief Calculate seconds using the existing calendar-to-epoch formula.
 * @param year Full Gregorian year, intended to be 1970 or later.
 * @param month Month 1 through 12.
 * @param day Valid day of month starting at 1.
 * @param hour Hour 0 through 23.
 * @param minute Minute 0 through 59.
 * @param second Second 0 through 59.
 * @return Unsigned 32-bit result of the implemented day-count arithmetic.
 * @note No validation or overflow checks. Invalid month values access outside
 *  the lookup table. Year/leap-day arithmetic is not guaranteed to invert
 *  epoch_to_datetime() for all dates; no timezone adjustment is made.
 */
uint32_t datetime_to_epoch(uint32_t year, uint32_t month, uint32_t day,
        uint32_t hour, uint32_t minute, uint32_t second) {
    static const int daysBeforeMonth[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };

     // Days from 1970 to current year
     int y = year - 1;
     uint32_t days = (y - 1969) * 365
     + (y - 1969) / 4
     - (y - 1969) / 100
     + (y - 1600) / 400;

     // Days in current year
     days += daysBeforeMonth[month - 1];
     if (month > 2 && datetime_isLeapYear(year)) {
         days += 1; // Leap day
     }

     days += day - 1; // Days in current month

     return days * 86400UL + hour * 3600UL + minute * 60UL + second;
}

// Convert epoch ? date/time (UTC)
/**
 * @brief Convert unsigned Unix epoch seconds into Gregorian UTC fields.
 * @param epoch Seconds since 1970-01-01 00:00:00 UTC.
 * @param[out] year Required pointer receiving the full year.
 * @param[out] month Required pointer receiving month 1 through 12.
 * @param[out] day Required pointer receiving day 1 through 31.
 * @param[out] hour Required pointer receiving hour 0 through 23.
 * @param[out] minute Required pointer receiving minute 0 through 59.
 * @param[out] second Required pointer receiving second 0 through 59.
 * @note Uses civil_from_days arithmetic independently of clock readiness;
 *  performs no allocation, I/O or timezone conversion.
 */
void epoch_to_datetime(uint32_t epoch,
        uint32_t *year, uint32_t *month, uint32_t *day,
        uint32_t *hour, uint32_t *minute, uint32_t *second) {
    uint32_t seconds_in_day = epoch % 86400UL;
    uint32_t days = epoch / 86400UL;

    // Time of day
    *hour = seconds_in_day / 3600;
    *minute = (seconds_in_day % 3600) / 60;
    *second = seconds_in_day % 60;

    // Algorithm from Howard Hinnant's civil_from_days
    // Shift so that 1970-01-01 is day 0
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097; // [0, 146096]
    int32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; // [0, 399]
    int32_t y = yoe + era * 400;
    int32_t doy = doe - (365*yoe + yoe/4 - yoe/100); // [0, 365]
    int32_t mp = (5*doy + 2) / 153; // [0, 11]
    int32_t d = doy - (153*mp+2)/5 + 1; // [1, 31]
    int32_t m = mp + (mp < 10 ? 3 : -9); // [1, 12]
    y += (m <= 2);

    *year = y;
    *month = m;
    *day = d;
}


/**
 * @brief Print a sample conversion and inverse for 2025-08-10 14:30:15.
 * @return Always 0; output is not compared to an expected result.
 * @note Does not set the system clock.
 */
 int32_t testDateTime(void) {
    // Convert to epoch
    uint32_t epoch = datetime_to_epoch(2025, 8, 10, 14, 30, 15);
    Report("Epoch: %lu\n", (unsigned long)epoch);

    // Convert back
    uint32_t y, m, d, hh, mm, ss;
    epoch_to_datetime(epoch, &y, &m, &d, &hh, &mm, &ss);
    Report("Converted back: %04d-%02d-%02d %02d:%02d:%02d\n",
    y, m, d, hh, mm, ss);

return 0;
}


