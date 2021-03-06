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

// Inertial Measurement Unit (IMU)

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "build_config.h"
#include "platform.h"
#include "debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/system.h"
#include "drivers/sensor.h"
#include "drivers/accgyro.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/boardalignment.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/hil.h"
#include "flight/navigation_rewrite.h"
#include "flight/navigation_rewrite_private.h"

#include "config/runtime_config.h"
#include "config/config.h"

#ifdef HIL

bool hilActive = false;
hilIncomingStateData_t hilToFC;
hilOutgoingStateData_t hilToSIM;

void hilUpdateControlState(void)
{
    // FIXME: There must be a cleaner way to to this
    // If HIL active, store PID outout into hilState and disable motor control
    if (FLIGHT_MODE(PASSTHRU_MODE) || !STATE(FIXED_WING)) {
        hilToSIM.pidCommand[ROLL] = rcCommand[ROLL];
        hilToSIM.pidCommand[PITCH] = rcCommand[PITCH];
        hilToSIM.pidCommand[YAW] = rcCommand[YAW];
    } else {
        hilToSIM.pidCommand[ROLL] = axisPID[ROLL];
        hilToSIM.pidCommand[PITCH] = axisPID[PITCH];
        hilToSIM.pidCommand[YAW] = axisPID[YAW];
    }

    hilToSIM.pidCommand[THROTTLE] = rcCommand[THROTTLE];
}

#endif
