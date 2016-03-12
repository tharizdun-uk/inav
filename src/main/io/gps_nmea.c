/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <math.h>

#include "platform.h"
#include "build_config.h"
#include "debug.h"

#include "common/maths.h"
#include "common/axis.h"
#include "common/utils.h"

#include "drivers/system.h"
#include "drivers/serial.h"
#include "drivers/serial_uart.h"

#include "io/serial.h"
#include "io/gps.h"
#include "io/gps_private.h"

#include "flight/gps_conversion.h"

#include "config/config.h"
#include "config/runtime_config.h"

#if defined(GPS) && defined(GPS_PROTO_NMEA)

/* This is a light implementation of a GPS frame decoding
   This should work with most of modern GPS devices configured to output 5 frames.
   It assumes there are some NMEA GGA frames to decode on the serial bus
   Now verifies checksum correctly before applying data

   Here we use only the following data :
     - latitude
     - longitude
     - GPS fix is/is not ok
     - GPS num sat (4 is enough to be +/- reliable)
     // added by Mis
     - GPS altitude (for OSD displaying)
     - GPS speed (for OSD displaying)
*/

#define NO_FRAME   0
#define FRAME_GGA  1
#define FRAME_RMC  2

static uint32_t grab_fields(char *src, uint8_t mult)
{                               // convert string to uint32
    uint32_t i;
    uint32_t tmp = 0;
    for (i = 0; src[i] != 0; i++) {
        if (src[i] == '.') {
            i++;
            if (mult == 0)
                break;
            else
                src[i + mult] = 0;
        }
        tmp *= 10;
        if (src[i] >= '0' && src[i] <= '9')
            tmp += src[i] - '0';
        if (i >= 15)
            return 0; // out of bounds
    }
    return tmp;
}

typedef struct gpsDataNmea_s {
    bool fix;
    int32_t latitude;
    int32_t longitude;
    uint8_t numSat;
    uint16_t altitude;
    uint16_t speed;
    uint16_t ground_course;
    uint16_t hdop;
} gpsDataNmea_t;

static bool gpsNewFrameNMEA(char c)
{
    static gpsDataNmea_t gps_Msg;

    uint8_t frameOK = 0;
    static uint8_t param = 0, offset = 0, parity = 0;
    static char string[15];
    static uint8_t checksum_param, gps_frame = NO_FRAME;

    switch (c) {
        case '$':
            param = 0;
            offset = 0;
            parity = 0;
            break;
        case ',':
        case '*':
            string[offset] = 0;
            if (param == 0) {       //frame identification
                gps_frame = NO_FRAME;
                if (string[0] == 'G' && string[1] == 'P' && string[2] == 'G' && string[3] == 'G' && string[4] == 'A')
                    gps_frame = FRAME_GGA;
                if (string[0] == 'G' && string[1] == 'P' && string[2] == 'R' && string[3] == 'M' && string[4] == 'C')
                    gps_frame = FRAME_RMC;
            }

            switch (gps_frame) {
                case FRAME_GGA:        //************* GPGGA FRAME parsing
                    switch(param) {
            //          case 1:             // Time information
            //              break;
                        case 2:
                            gps_Msg.latitude = GPS_coord_to_degrees(string);
                            break;
                        case 3:
                            if (string[0] == 'S')
                                gps_Msg.latitude *= -1;
                            break;
                        case 4:
                            gps_Msg.longitude = GPS_coord_to_degrees(string);
                            break;
                        case 5:
                            if (string[0] == 'W')
                                gps_Msg.longitude *= -1;
                            break;
                        case 6:
                            if (string[0] > '0') {
                                gps_Msg.fix = true;
                            } else {
                                gps_Msg.fix = false;
                            }
                            break;
                        case 7:
                            gps_Msg.numSat = grab_fields(string, 0);
                            break;
                        case 8:
                            gps_Msg.hdop = grab_fields(string, 1) * 10;         // hdop (assume GPS is reporting it in meters)
                            break;
                        case 9:
                            gps_Msg.altitude = grab_fields(string, 1) * 10;     // altitude in cm
                            break;
                    }
                    break;
                case FRAME_RMC:        //************* GPRMC FRAME parsing
                    switch(param) {
                        case 7:
                            gps_Msg.speed = ((grab_fields(string, 1) * 5144L) / 1000L);    // speed in cm/s added by Mis
                            break;
                        case 8:
                            gps_Msg.ground_course = (grab_fields(string, 1));      // ground course deg * 10
                            break;
                    }
                    break;
            }

            param++;
            offset = 0;
            if (c == '*')
                checksum_param = 1;
            else
                parity ^= c;
            break;
        case '\r':
        case '\n':
            if (checksum_param) {   //parity checksum
                uint8_t checksum = 16 * ((string[0] >= 'A') ? string[0] - 'A' + 10 : string[0] - '0') + ((string[1] >= 'A') ? string[1] - 'A' + 10 : string[1] - '0');
                if (checksum == parity) {
                    gpsStats.packetCount++;
                    switch (gps_frame) {
                    case FRAME_GGA:
                        frameOK = 1;
                        gpsSol.numSat = gps_Msg.numSat;
                        if (gps_Msg.fix) {
                            gpsSol.flags.fix3D = 1;
                            gpsSol.llh.lat = gps_Msg.latitude;
                            gpsSol.llh.lon = gps_Msg.longitude;
                            gpsSol.llh.alt = gps_Msg.altitude;

                            // EPH/EPV are unreliable for NMEA as they are not real accuracy
                            gpsSol.eph = gpsConstrainEPE(gps_Msg.hdop);
                            gpsSol.epv = gpsConstrainEPE(gps_Msg.hdop);
                            gpsSol.flags.validEPE = 0;
                        }
                        else {
                            gpsSol.flags.fix3D = 0;
                        }

                        // NMEA does not report VELNED
                        gpsSol.flags.validVelNE = 0;
                        gpsSol.flags.validVelD = 0;
                        break;
                    case FRAME_RMC:
                        gpsSol.groundSpeed = gps_Msg.speed;
                        gpsSol.groundCourse = gps_Msg.ground_course;
                        break;
                    } // end switch
                }
                else {
                    gpsStats.errors++;
                }
            }
            checksum_param = 0;
            break;
        default:
            if (offset < 15)
                string[offset++] = c;
            if (!checksum_param)
                parity ^= c;
    }
    return frameOK;
}

static bool gpsReceiveData(void)
{
    bool hasNewData = false;

    if (gpsState.gpsPort) {
        while (serialRxBytesWaiting(gpsState.gpsPort)) {
            uint8_t newChar = serialRead(gpsState.gpsPort);
            if (gpsNewFrameNMEA(newChar)) {
                gpsSol.flags.gpsHeartbeat = !gpsSol.flags.gpsHeartbeat;
                gpsSol.flags.validVelNE = 0;
                gpsSol.flags.validVelD = 0;
                hasNewData = true;
            }
        }
    }

    return hasNewData;
}

static bool gpsInitialize(void)
{
    gpsSetState(GPS_CHANGE_BAUD);
    return false;
}

static bool gpsChangeBaud(void)
{
    gpsFinalizeChangeBaud();
    return false;
}

bool gpsHandleNMEA(void)
{
    // Receive data
    bool hasNewData = gpsReceiveData();

    // Process state
    switch(gpsState.state) {
    default:
        return false;

    case GPS_INITIALIZING:
        return gpsInitialize();

    case GPS_CHANGE_BAUD:
        return gpsChangeBaud();


    case GPS_CHECK_VERSION:
    case GPS_CONFIGURE:
        // No autoconfig, switch straight to receiving data 
        gpsSetState(GPS_RECEIVING_DATA);
        return false;

    case GPS_RECEIVING_DATA:
        return hasNewData;
    }
}

#endif
