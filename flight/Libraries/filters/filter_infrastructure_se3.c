/**
 ******************************************************************************
 * @addtogroup TauLabsModules Tau Labs Modules
 * @{
 * @file       filter_infrastructure_se3.c
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013
 * @brief      Infrastructure for managing SE(3)+ filters
 *             because of the airspeed output this is slightly more than SE(3)
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "filter_infrastructure_se3.h"
#include "coordinate_conversions.h"
#include "physical_constants.h"

#include "accels.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "baroaltitude.h"
#include "flightstatus.h"
#include "gpsposition.h"
#include "gpsvelocity.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "homelocation.h"
#include "sensorsettings.h"
#include "inssettings.h"
#include "insstate.h"
#include "magnetometer.h"
#include "nedaccel.h"
#include "nedposition.h"
#include "positionactual.h"
#include "stateestimation.h"
#include "velocityactual.h"

//! Maximum time to wait for data before setting an error
#define FAILSAFE_TIMEOUT_MS 10

//! Local pointer for the working data (should be moved into the instance)
static struct filter_infrastructure_se3_data *s3_data;

static int32_t getNED(GPSPositionData * gpsPosition, float * NED);
static int32_t gpsOK(GPSPositionData * gpsPosition);

/**
 * Initialize SE(3)+ filter infrastructure
 * @param[out] data   the common part shared amongst SE(3)+ filters
 */
int32_t filter_infrastructure_se3_init(struct filter_infrastructure_se3_data **data)
{
	// Only create one instance of the common data.  This might not be what we want to
	// keep doing.  A easy (but more memory intense) way to run multiple filters would
	// be to make them all manage their own queues

	if (s3_data == NULL) {
		s3_data = (struct filter_infrastructure_se3_data *) pvPortMalloc(sizeof(struct filter_infrastructure_se3_data));
	}
	if (!s3_data)
		return -1;

	(*data) = s3_data;

	AttitudeActualInitialize();
	AttitudeSettingsInitialize();
	SensorSettingsInitialize();
	NEDPositionInitialize();
	NedAccelInitialize();
	PositionActualInitialize();
	VelocityActualInitialize();

	// Create the data queues
	s3_data->gyroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	s3_data->accelQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	s3_data->magQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	s3_data->baroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	s3_data->gpsQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	s3_data->gpsVelQueue = xQueueCreate(1, sizeof(UAVObjEvent));

	return 0;
}

//! Connect the queues used for SE(3)+ filters
int32_t filter_infrastructure_se3_start(uintptr_t id)
{
	if (GyrosHandle())
		GyrosConnectQueue(s3_data->gyroQueue);
	if (AccelsHandle())
		AccelsConnectQueue(s3_data->accelQueue);
	if (MagnetometerHandle())
		MagnetometerConnectQueue(s3_data->magQueue);
	if (BaroAltitudeHandle())
		BaroAltitudeConnectQueue(s3_data->baroQueue);
	if (GPSPositionHandle())
		GPSPositionConnectQueue(s3_data->gpsQueue);
	if (GPSVelocityHandle())
		GPSVelocityConnectQueue(s3_data->gpsVelQueue);

	return 0;
}

/**
 * process_filter_generic Compute an update of an SE(3)+ filter
 * @param[in] driver The SE(3)+ filter driver
 * @param[in] dT the update time in seconds
 * @return 0 if succesfully updated or error code
 */
int32_t filter_infrastructure_se3_process(struct filter_driver *upper_driver, uintptr_t id, float dt)
{
	// TODO: check error codes

	// Make sure we are safe to get the class specific driver
	if (!filter_interface_validate(upper_driver))
		return -1;
	struct filter_s3 *driver = &(upper_driver->sub_driver.driver_s3);

	/* 1. fetch the data from queues and pass to filter                    */
	/* if we want to start running multiple instances of this filter class */
	/* simultaneously, then this step should be done once and then all     */
	/* filters should be processed with the same data                      */

	// Potential measurements
	float *gyros = NULL;
	float *accels = NULL;
	float *mag = NULL;
	float *pos = NULL;
	float *vel = NULL;
	float *baro = NULL;
	float *airspeed = NULL;

	// Check whether the measurements were updated and fetch if so
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	MagnetometerData magData;
	BaroAltitudeData baroData;
	GPSPositionData gpsPosition;
	GPSVelocityData gpsVelocity;
	float NED[3];

	if (xQueueReceive(s3_data->gyroQueue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) == pdTRUE) {
		// Convert gyros to rad / s
		GyrosGet(&gyrosData);
		gyrosData.x *= DEG2RAD;
		gyrosData.y *= DEG2RAD;
		gyrosData.z *= DEG2RAD;
		gyros = &gyrosData.x;
	}

	if (xQueueReceive(s3_data->accelQueue, &ev, 1 / portTICK_RATE_MS) == pdTRUE) {
		AccelsGet(&accelsData);
		accels = &accelsData.x;
	}

	if (xQueueReceive(s3_data->magQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) {
		MagnetometerGet(&magData);
		mag = &magData.x;
	}

	if (xQueueReceive(s3_data->baroQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) {
		BaroAltitudeGet(&baroData);
		baro = &baroData.Altitude;
	}

	if (xQueueReceive(s3_data->gpsQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) {
		GPSPositionGet(&gpsPosition);
		if (gpsOK(&gpsPosition) == 0) {
			getNED(&gpsPosition, NED);

			NEDPositionData nedPos;
			nedPos.North = NED[0];
			nedPos.East = NED[1];
			nedPos.Down = NED[2];
			NEDPositionSet(&nedPos);

			if (getNED(&gpsPosition, NED) == 0) {
				pos = NED;				
			}
		}
	}

	if (xQueueReceive(s3_data->gpsVelQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) {
		if (gpsOK(NULL) == 0) {
			GPSVelocityGet(&gpsVelocity);
			vel = &gpsVelocity.North;
		}
	}

	/* 2. compute update */
	driver->update_filter(id, gyros, accels, mag, pos, vel, baro, airspeed, dt);

	/* 3. get the state update from the filter */
	float pos_state[3];
	float vel_state[3];
	float q_state[4];
	float gyro_bias_state[3];

	driver->get_state(id, pos_state, vel_state, q_state, gyro_bias_state, NULL);

	// Store the data in UAVOs
	PositionActualData positionActual;
	positionActual.North = pos_state[0];
	positionActual.East  = pos_state[1];
	positionActual.Down  = pos_state[2];
	PositionActualSet(&positionActual);

	VelocityActualData velocityActual;
	velocityActual.North = vel_state[0];
	velocityActual.East  = vel_state[1];
	velocityActual.Down  = vel_state[2];
	VelocityActualSet(&velocityActual);

	AttitudeActualData attitudeActual;
	attitudeActual.q1 = q_state[0];
	attitudeActual.q2 = q_state[1];
	attitudeActual.q3 = q_state[2];
	attitudeActual.q4 = q_state[3];
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);
	AttitudeActualSet(&attitudeActual);

	// Convert gyros bias to deg/s for storage
	GyrosBiasData gyrosBias;
	gyrosBias.x = gyro_bias_state[0] * RAD2DEG;
	gyrosBias.y = gyro_bias_state[1] * RAD2DEG;
	gyrosBias.z = gyro_bias_state[2] * RAD2DEG;
	GyrosBiasSet(&gyrosBias);

	return 0;
}

#define GPS_GOOD_PDOP 3.0f
#define GPS_GOOD_SAT  7
#define GPS_WARN_SAT  6
/**
 * @brief Check GPS signal is good enough to use
 *
 * This function uses a hystersis threshold, so first
 * before it becomes active a SAT >= 7 and PDOP <= 3
 * must be seen. After that it just requires 6 satellites
 * to continue.
 *
 * @param[in] Current GPS coordinates
 * @return 0 if good, -1 if not
 */
static int32_t gpsOK(GPSPositionData * gpsPosition)
{
	enum gps_status {GPS_WAIT_FOR_LOCK, GPS_LOST, GPS_DEGRADED, GPS_GOOD};
	static enum gps_status status = GPS_WAIT_FOR_LOCK;

	if (gpsPosition != NULL) {
		// Update the GPS quality state when data is here
		switch(status) {
		case GPS_WAIT_FOR_LOCK:
			if (gpsPosition->Status == GPSPOSITION_STATUS_FIX3D &&
				gpsPosition->PDOP < GPS_GOOD_PDOP &&
				gpsPosition->Satellites >= GPS_GOOD_SAT) {
				status = GPS_GOOD;
			}
			break;
		default:
			if (gpsPosition->Satellites >= GPS_GOOD_SAT) {
				status = GPS_GOOD;
			} else if (gpsPosition->Satellites >= GPS_WARN_SAT) {
				status = GPS_DEGRADED;
			} else
				status = GPS_LOST;
			break;
		}
	}

	switch(status) {
	case GPS_GOOD:
	case GPS_DEGRADED:
		return 0;
	default:
		return -1;
	}
}

/**
 * @brief Convert the GPS LLA position into NED coordinates
 * @note this method uses a taylor expansion around the home coordinates
 * to convert to NED which allows it to be done with all floating
 * calculations
 *
 * @TODO: refactor into coordinate conversions
 *
 * @param[in] Current GPS coordinates
 * @param[out] NED frame coordinates
 * @returns 0 for success, -1 for failure
 */
static int32_t getNED(GPSPositionData * gpsPosition, float * NED)
{
	HomeLocationData homeLocation;
	HomeLocationGet(&homeLocation);

	if (homeLocation.Set != HOMELOCATION_SET_TRUE)
		return -1;

	float T[3];

	// Compute matrix to convert deltaLLA to NED
	float lat, alt;
	lat = homeLocation.Latitude / 10.0e6f * DEG2RAD;
	alt = homeLocation.Altitude;

	T[0] = alt+6.378137E6f;
	T[1] = cosf(lat)*(alt+6.378137E6f);
	T[2] = -1.0f;

	float dL[3] = {(gpsPosition->Latitude - homeLocation.Latitude) / 10.0e6f * DEG2RAD,
		(gpsPosition->Longitude - homeLocation.Longitude) / 10.0e6f * DEG2RAD,
		(gpsPosition->Altitude + gpsPosition->GeoidSeparation - homeLocation.Altitude)};

	NED[0] = T[0] * dL[0];
	NED[1] = T[1] * dL[1];
	NED[2] = T[2] * dL[2];

	return 0;
}

/**
 * @}
 */
