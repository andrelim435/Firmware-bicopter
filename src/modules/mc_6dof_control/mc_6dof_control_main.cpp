/****************************************************************************
 *
 *   Copyright (c) 2013-2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_6dof_control_main.cpp
 * Multicopter 6dof position & attitude controller
 *
 * @author Lorenz Meier		<lorenz@px4.io>
 * @author Anton Babushkin	<anton.babushkin@me.com>
 * @author Sander Smeets	<sander@droneslab.com>
 * @author Matthias Grob	<maetugr@gmail.com>
 * @author Beat Küng		<beat-kueng@gmx.net>
 * @author Andre Lim		<andrelim435@hotmail.com>
 *
 */

#include "mc_6dof_control.hpp"

#include <conversion/rotation.h>
#include <drivers/drv_hrt.h>
#include <lib/ecl/geo/geo.h>
#include <circuit_breaker/circuit_breaker.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>

#define TPA_RATE_LOWER_LIMIT 0.05f

#define AXIS_INDEX_ROLL 0
#define AXIS_INDEX_PITCH 1
#define AXIS_INDEX_YAW 2
#define AXIS_COUNT 3

using namespace matrix;


int Multicopter6dofControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This implements the multicopter attitude and rate controller. It takes attitude
setpoints (`vehicle_attitude_setpoint`) or rate setpoints (in acro mode
via `manual_control_setpoint` topic) as inputs and outputs actuator control messages.

The controller has two loops: a P loop for angular error and a PID loop for angular rate error.

Publication documenting the implemented Quaternion Attitude Control:
Nonlinear Quadrocopter Attitude Control (2013)
by Dario Brescianini, Markus Hehn and Raffaello D'Andrea
Institute for Dynamic Systems and Control (IDSC), ETH Zurich

https://www.research-collection.ethz.ch/bitstream/handle/20.500.11850/154099/eth-7387-01.pdf

### Implementation
To reduce control latency, the module directly polls on the gyro topic published by the IMU driver.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("mc_6dof_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

Multicopter6dofControl::Multicopter6dofControl() :
	ModuleParams(nullptr),
	_loop_perf(perf_alloc(PC_ELAPSED, "mc_6dof_control"))
{
	for (uint8_t i = 0; i < MAX_GYRO_COUNT; i++) {
		_sensor_gyro_sub[i] = -1;
	}

	_vehicle_status.is_rotary_wing = true;

	/* initialize quaternions in messages to be valid */
	_v_att.q[0] = 1.f;
	_v_att_sp.q_d[0] = 1.f;

	_rates_prev.zero();
	_rates_prev_filtered.zero();
	_rates_sp.zero();
	_rates_int.zero();
	_thrust_sp = 0.0f;
	_att_control_0.zero();
	_att_control_1.zero();
	_att_control_thrust = 0.0f;
	_p_control_att_0.zero();
	_p_control_att_1.zero();
	_virtual_control_0.zero();
	_virtual_control_1.zero();

	/* initialize thermal corrections as we might not immediately get a topic update (only non-zero values) */
	for (unsigned i = 0; i < 3; i++) {
		// used scale factors to unity
		_sensor_correction.gyro_scale_0[i] = 1.0f;
		_sensor_correction.gyro_scale_1[i] = 1.0f;
		_sensor_correction.gyro_scale_2[i] = 1.0f;
	}

	parameters_updated();
}

void
Multicopter6dofControl::parameters_updated()
{
	// Store some of the parameters in a more convenient way & precompute often-used values
	_attitude_control.setProportionalGain(Vector3f(_param_mc_roll_p.get(), _param_mc_pitch_p.get(), _param_mc_yaw_p.get()));

	// rate gains
	_rate_p = Vector3f(_param_mc_rollrate_p.get(), _param_mc_pitchrate_p.get(), _param_mc_yawrate_p.get());
	_rate_i = Vector3f(_param_mc_rollrate_i.get(), _param_mc_pitchrate_i.get(), _param_mc_yawrate_i.get());
	_rate_int_lim = Vector3f(_param_mc_rr_int_lim.get(), _param_mc_pr_int_lim.get(), _param_mc_yr_int_lim.get());
	_rate_d = Vector3f(_param_mc_rollrate_d.get(), _param_mc_pitchrate_d.get(), _param_mc_yawrate_d.get());
	_rate_ff = Vector3f(_param_mc_rollrate_ff.get(), _param_mc_pitchrate_ff.get(), _param_mc_yawrate_ff.get());

	if (fabsf(_lp_filters_d.get_cutoff_freq() - _param_mc_dterm_cutoff.get()) > 0.01f) {
		_lp_filters_d.set_cutoff_frequency(_loop_update_rate_hz, _param_mc_dterm_cutoff.get());
		_lp_filters_d.reset(_rates_prev);
	}

	// angular rate limits
	using math::radians;
	_attitude_control.setRateLimit(Vector3f(radians(_param_mc_rollrate_max.get()), radians(_param_mc_pitchrate_max.get()), radians(_param_mc_yawrate_max.get())));

	// manual rate control acro mode rate limits
	_acro_rate_max = Vector3f(radians(_param_mc_acro_r_max.get()), radians(_param_mc_acro_p_max.get()), radians(_param_mc_acro_y_max.get()));

	_man_tilt_max = math::radians(_param_mpc_man_tilt_max.get());

	_actuators_0_circuit_breaker_enabled = circuit_breaker_enabled("CBRK_RATE_CTRL", CBRK_RATE_CTRL_KEY);

	/* get transformation matrix from sensor/board to body frame */
	_board_rotation = get_rot_matrix((enum Rotation)_param_sens_board_rot.get());

	/* fine tune the rotation */
	Dcmf board_rotation_offset(Eulerf(
			M_DEG_TO_RAD_F * _param_sens_board_x_off.get(),
			M_DEG_TO_RAD_F * _param_sens_board_y_off.get(),
			M_DEG_TO_RAD_F * _param_sens_board_z_off.get()));
	_board_rotation = board_rotation_offset * _board_rotation;
}

void
Multicopter6dofControl::parameter_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		updateParams();
		parameters_updated();
	}
}

void
Multicopter6dofControl::vehicle_control_mode_poll()
{
	bool updated;

	/* Check if vehicle control mode has changed */
	orb_check(_v_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _v_control_mode_sub, &_v_control_mode);
	}
}

bool
Multicopter6dofControl::vehicle_manual_poll()
{
	bool updated;

	/* get pilots inputs */
	orb_check(_manual_control_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(manual_control_setpoint), _manual_control_sp_sub, &_manual_control_sp);
		return true;
	}
	return false;
}

void
Multicopter6dofControl::vehicle_attitude_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_att_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_attitude_setpoint), _v_att_sp_sub, &_v_att_sp);
	}
}

void
Multicopter6dofControl::partial_controls_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_partial_controls_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(partial_controls), _partial_controls_sub, &_partial_controls);
	}
}

bool
Multicopter6dofControl::vehicle_rates_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_rates_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_rates_setpoint), _v_rates_sp_sub, &_v_rates_sp);
		return true;
	}
	return false;
}

void
Multicopter6dofControl::vehicle_status_poll()
{
	/* check if there is new status information */
	bool vehicle_status_updated;
	orb_check(_vehicle_status_sub, &vehicle_status_updated);

	if (vehicle_status_updated) {
		orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &_vehicle_status);

		/* set correct uORB ID, depending on if vehicle is VTOL or not */
		if (_actuators_id == nullptr) {
			if (_vehicle_status.is_vtol) {
				_actuators_id = ORB_ID(actuator_controls_virtual_mc);
				_attitude_sp_id = ORB_ID(mc_virtual_attitude_setpoint);

				int32_t vt_type = -1;
				if (param_get(param_find("VT_TYPE"), &vt_type) == PX4_OK) {
					_is_tailsitter = (vt_type == vtol_type::TAILSITTER);
				}

			} else {
				_actuators_id = ORB_ID(actuator_controls_0);
				_attitude_sp_id = ORB_ID(vehicle_attitude_setpoint);
			}
		}
	}
}

void
Multicopter6dofControl::vehicle_motor_limits_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_motor_limits_sub, &updated);

	if (updated) {
		multirotor_motor_limits_s motor_limits = {};
		orb_copy(ORB_ID(multirotor_motor_limits), _motor_limits_sub, &motor_limits);

		_saturation_status.value = motor_limits.saturation_status;
	}
}

void
Multicopter6dofControl::battery_status_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_battery_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(battery_status), _battery_status_sub, &_battery_status);
	}
}

bool
Multicopter6dofControl::vehicle_attitude_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_v_att_sub, &updated);

	if (updated) {
		uint8_t prev_quat_reset_counter = _v_att.quat_reset_counter;

		orb_copy(ORB_ID(vehicle_attitude), _v_att_sub, &_v_att);

		// Check for a heading reset
		if (prev_quat_reset_counter != _v_att.quat_reset_counter) {
			// we only extract the heading change from the delta quaternion
			_man_yaw_sp += Eulerf(Quatf(_v_att.delta_q_reset)).psi();
		}
		return true;
	}
	return false;
}

void
Multicopter6dofControl::sensor_correction_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_correction_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_correction), _sensor_correction_sub, &_sensor_correction);
	}

	/* update the latest gyro selection */
	if (_sensor_correction.selected_gyro_instance < _gyro_count) {
		_selected_gyro = _sensor_correction.selected_gyro_instance;
	}
}

void
Multicopter6dofControl::sensor_bias_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_bias_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_bias), _sensor_bias_sub, &_sensor_bias);
	}

}

void
Multicopter6dofControl::vehicle_land_detected_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_vehicle_land_detected_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_land_detected), _vehicle_land_detected_sub, &_vehicle_land_detected);
	}

}

void
Multicopter6dofControl::landing_gear_state_poll()
{
	bool updated;
	orb_check(_landing_gear_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(landing_gear), _landing_gear_sub, &_landing_gear);
	}
}

float
Multicopter6dofControl::throttle_curve(float throttle_stick_input)
{
	// throttle_stick_input is in range [0, 1]
	switch (_param_mpc_thr_curve.get()) {
	case 1: // no rescaling to hover throttle
		return _param_mpc_manthr_min.get() + throttle_stick_input * (_param_mpc_thr_max.get() - _param_mpc_manthr_min.get());

	default: // 0 or other: rescale to hover throttle at 0.5 stick
		if (throttle_stick_input < 0.5f) {
			return (_param_mpc_thr_hover.get() - _param_mpc_manthr_min.get()) / 0.5f * throttle_stick_input + _param_mpc_manthr_min.get();

		} else {
			return (_param_mpc_thr_max.get() - _param_mpc_thr_hover.get()) / 0.5f * (throttle_stick_input - 1.0f) + _param_mpc_thr_max.get();
		}
	}
}

float
Multicopter6dofControl::get_landing_gear_state()
{
	// Only switch the landing gear up if we are not landed and if
	// the user switched from gear down to gear up.
	// If the user had the switch in the gear up position and took off ignore it
	// until he toggles the switch to avoid retracting the gear immediately on takeoff.
	if (_vehicle_land_detected.landed) {
		_gear_state_initialized = false;
	}
	float landing_gear = landing_gear_s::GEAR_DOWN; // default to down
	if (_manual_control_sp.gear_switch == manual_control_setpoint_s::SWITCH_POS_ON && _gear_state_initialized) {
		landing_gear = landing_gear_s::GEAR_UP;

	} else if (_manual_control_sp.gear_switch == manual_control_setpoint_s::SWITCH_POS_OFF) {
		// Switching the gear off does put it into a safe defined state
		_gear_state_initialized = true;
	}

	return landing_gear;
}

void
Multicopter6dofControl::generate_attitude_setpoint(float dt, bool reset_yaw_sp)
{
	vehicle_attitude_setpoint_s attitude_setpoint{};
	const float yaw = Eulerf(Quatf(_v_att.q)).psi();

	/* reset yaw setpoint to current position if needed */
	if (reset_yaw_sp) {
		_man_yaw_sp = yaw;

	} else if (_manual_control_sp.z > 0.05f || _param_mc_airmode.get() == (int32_t)Mixer::Airmode::roll_pitch_yaw) {

		const float yaw_rate = math::radians(_param_mpc_man_y_max.get());
		attitude_setpoint.yaw_sp_move_rate = _manual_control_sp.r * yaw_rate;
		_man_yaw_sp = wrap_pi(_man_yaw_sp + attitude_setpoint.yaw_sp_move_rate * dt);
	}

	/*
	 * Input mapping for roll & pitch setpoints
	 * ----------------------------------------
	 * We control the following 2 angles:
	 * - tilt angle, given by sqrt(x*x + y*y)
	 * - the direction of the maximum tilt in the XY-plane, which also defines the direction of the motion
	 *
	 * This allows a simple limitation of the tilt angle, the vehicle flies towards the direction that the stick
	 * points to, and changes of the stick input are linear.
	 */
	const float x = _manual_control_sp.x * _man_tilt_max;
	const float y = _manual_control_sp.y * _man_tilt_max;

	// we want to fly towards the direction of (x, y), so we use a perpendicular axis angle vector in the XY-plane
	Vector2f v = Vector2f(y, -x);
	float v_norm = v.norm(); // the norm of v defines the tilt angle

	if (v_norm > _man_tilt_max) { // limit to the configured maximum tilt angle
		v *= _man_tilt_max / v_norm;
	}

	Quatf q_sp_rpy = AxisAnglef(v(0), v(1), 0.f);
	Eulerf euler_sp = q_sp_rpy;
	attitude_setpoint.roll_body = euler_sp(0);
	attitude_setpoint.pitch_body = euler_sp(1);
	// The axis angle can change the yaw as well (noticeable at higher tilt angles).
	// This is the formula by how much the yaw changes:
	//   let a := tilt angle, b := atan(y/x) (direction of maximum tilt)
	//   yaw = atan(-2 * sin(b) * cos(b) * sin^2(a/2) / (1 - 2 * cos^2(b) * sin^2(a/2))).
	attitude_setpoint.yaw_body = _man_yaw_sp + euler_sp(2);

	/* modify roll/pitch only if we're a VTOL */
	if (_vehicle_status.is_vtol) {
		// Construct attitude setpoint rotation matrix. Modify the setpoints for roll
		// and pitch such that they reflect the user's intention even if a large yaw error
		// (yaw_sp - yaw) is present. In the presence of a yaw error constructing a rotation matrix
		// from the pure euler angle setpoints will lead to unexpected attitude behaviour from
		// the user's view as the euler angle sequence uses the  yaw setpoint and not the current
		// heading of the vehicle.
		// However there's also a coupling effect that causes oscillations for fast roll/pitch changes
		// at higher tilt angles, so we want to avoid using this on multicopters.
		// The effect of that can be seen with:
		// - roll/pitch into one direction, keep it fixed (at high angle)
		// - apply a fast yaw rotation
		// - look at the roll and pitch angles: they should stay pretty much the same as when not yawing

		// calculate our current yaw error
		float yaw_error = wrap_pi(attitude_setpoint.yaw_body - yaw);

		// compute the vector obtained by rotating a z unit vector by the rotation
		// given by the roll and pitch commands of the user
		Vector3f zB = {0.0f, 0.0f, 1.0f};
		Dcmf R_sp_roll_pitch = Eulerf(attitude_setpoint.roll_body, attitude_setpoint.pitch_body, 0.0f);
		Vector3f z_roll_pitch_sp = R_sp_roll_pitch * zB;

		// transform the vector into a new frame which is rotated around the z axis
		// by the current yaw error. this vector defines the desired tilt when we look
		// into the direction of the desired heading
		Dcmf R_yaw_correction = Eulerf(0.0f, 0.0f, -yaw_error);
		z_roll_pitch_sp = R_yaw_correction * z_roll_pitch_sp;

		// use the formula z_roll_pitch_sp = R_tilt * [0;0;1]
		// R_tilt is computed from_euler; only true if cos(roll) not equal zero
		// -> valid if roll is not +-pi/2;
		attitude_setpoint.roll_body = -asinf(z_roll_pitch_sp(1));
		attitude_setpoint.pitch_body = atan2f(z_roll_pitch_sp(0), z_roll_pitch_sp(2));
	}

	/* copy quaternion setpoint to attitude setpoint topic */
	Quatf q_sp = Eulerf(attitude_setpoint.roll_body, attitude_setpoint.pitch_body, attitude_setpoint.yaw_body);
	q_sp.copyTo(attitude_setpoint.q_d);
	attitude_setpoint.q_d_valid = true;

	attitude_setpoint.thrust_body[2] = -throttle_curve(_manual_control_sp.z);
	attitude_setpoint.timestamp = hrt_absolute_time();
	if (_attitude_sp_id != nullptr) {
		orb_publish_auto(_attitude_sp_id, &_vehicle_attitude_setpoint_pub, &attitude_setpoint, nullptr, ORB_PRIO_DEFAULT);
	}

	_landing_gear.landing_gear = get_landing_gear_state();
	_landing_gear.timestamp = hrt_absolute_time();
	orb_publish_auto(ORB_ID(landing_gear), &_landing_gear_pub, &_landing_gear, nullptr, ORB_PRIO_DEFAULT);
}

/**
 * Attitude controller.
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_p_control_att_0/1' vectors
 */
void
Multicopter6dofControl::control_attitude()
{
	vehicle_attitude_setpoint_poll();

	// reinitialize the setpoint while not armed to make sure no value from the last mode or flight is still kept
	if (!_v_control_mode.flag_armed) {
		Quatf().copyTo(_v_att_sp.q_d);
		Vector3f().copyTo(_v_att_sp.thrust_body);
	}

	// physical thrust axis is the negative of body z axis
	_thrust_sp = -_v_att_sp.thrust_body[2];

	Quatf q = Quatf(_v_att.q);
	// Should always be zero for now. Use this when adding full 6dof control to offboard mode
	Quatf qd = Quatf(_v_att_sp.q_d);

	// ensure input quaternions are exactly normalized because acosf(1.00001) == NaN
	q.normalize();
	qd.normalize();

	// quaternion attitude control law, qe is rotation from q to qd
	const Quatf qe = q.inversed() * qd;

	// using sin(alpha/2) scaled rotation axis as attitude error (see quaternion definition by axis angle)
	// also taking care of the antipodal unit quaternion ambiguity
	// const Vector3f eq = 1.f * math::signNoZero(qe(0)) * qe.imag();
	Vector3f eq = Eulerf(qe);

	// /*	Set eq to only desired (no current att) */
	// eq = Eulerf(qd);
	// eq(2) = 0.f;

	// /*	Remove certain channels for testing */
	// eq = -Eulerf(q);
	// eq(0) = 0.f;
	// eq(1) = 0.f;
	eq(2) = 0.f;

	// /*	Quaternion to Euler formula from wikipedia */
	// eq(0) = atan2f( 2*(qe(0)*qe(1) + qe(2)*qe(3)), 1 - 2*(qe(1)*qe(1) + qe(2)*qe(2)) );
	// eq(1) = asinf( 2*(qe(0)*qe(2) + qe(3)*qe(1)) );
	// eq(2) = atan2f( 2*(qe(0)*qe(3) + qe(1)*qe(2)), 1 - 2*(qe(2)*qe(2) + qe(3)*qe(3)) );


	// Calculate partial LQR output
	// Rotor 1: phi,theta,psi
	_p_control_att_0(0) = _param_mpc_lqr_k410.get() * eq(0) + _param_mpc_lqr_k111.get() * eq(1) + _param_mpc_lqr_k112.get() * eq(2);
	_p_control_att_0(1) = _param_mpc_lqr_k510.get() * eq(0) + _param_mpc_lqr_k211.get() * eq(1) + _param_mpc_lqr_k212.get() * eq(2);
	_p_control_att_0(2) = _param_mpc_lqr_k610.get() * eq(0) + _param_mpc_lqr_k311.get() * eq(1) + _param_mpc_lqr_k312.get() * eq(2);
	// Rotor 2: phi,theta,psi
	_p_control_att_1(0) = _param_mpc_lqr_k110.get() * eq(0) + _param_mpc_lqr_k411.get() * eq(1) + _param_mpc_lqr_k412.get() * eq(2);
	_p_control_att_1(1) = _param_mpc_lqr_k210.get() * eq(0) + _param_mpc_lqr_k511.get() * eq(1) + _param_mpc_lqr_k512.get() * eq(2);
	_p_control_att_1(2) = _param_mpc_lqr_k310.get() * eq(0) + _param_mpc_lqr_k611.get() * eq(1) + _param_mpc_lqr_k612.get() * eq(2);

	/* Add gravity vector */
	// const Vector3f gravity_body_frame = q.conjugate_inversed(Vector3f(0,0,0.37));
	// _p_control_att_0 += gravity_body_frame;
	// _p_control_att_1 += gravity_body_frame;
	_p_control_att_0 += Vector3f(0,0,0.37);
	_p_control_att_1 += Vector3f(0,0,0.37);

	// // TESTING
	// Quatf qd = Quatf(_v_att_sp.q_d);
	// Eulerf ed = Eulerf(qd.inversed());

	// _thrust_sp = _thrust_sp * _param_mpc_max_thrust.get();

	// // Rotor 1: phi,theta,psi
	// _p_control_att_0(0) = _thrust_sp * sin(ed.phi());
	// _p_control_att_0(1) = _thrust_sp * sin(ed.theta());
	// _p_control_att_0(2) += _thrust_sp * _param_mpc_max_thrust.get();
	// // Rotor 2: phi,theta,psi
	// _p_control_att_1(0) = _thrust_sp * sin(ed.phi());
	// _p_control_att_1(1) = _thrust_sp * sin(ed.theta());
	// _p_control_att_1(2) += _thrust_sp * _param_mpc_max_thrust.get();
}

/*
 * Throttle PID attenuation
 * Function visualization available here https://www.desmos.com/calculator/gn4mfoddje
 * Input: 'tpa_breakpoint', 'tpa_rate', '_thrust_sp'
 * Output: 'pidAttenuationPerAxis' vector
 */
Vector3f
Multicopter6dofControl::pid_attenuations(float tpa_breakpoint, float tpa_rate)
{
	/* throttle pid attenuation factor */
	float tpa = 1.0f - tpa_rate * (fabsf(_thrust_sp) - tpa_breakpoint) / (1.0f - tpa_breakpoint);
	tpa = fmaxf(TPA_RATE_LOWER_LIMIT, fminf(1.0f, tpa));

	Vector3f pidAttenuationPerAxis;
	pidAttenuationPerAxis(AXIS_INDEX_ROLL) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_PITCH) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_YAW) = 1.0;

	return pidAttenuationPerAxis;
}

/*
 * Attitude rates controller.
 * Input: '_rates_sp' vector, '_thrust_sp'
 * Output: '_att_control' vector
 */
void
Multicopter6dofControl::control_attitude_rates(float dt)
{
	// /* reset integral if disarmed */
	// if (!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) {
	// 	_rates_int.zero();
	// }

	// get the raw gyro data and correct for thermal errors
	Vector3f rates;

	if (_selected_gyro == 0) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_0[0]) * _sensor_correction.gyro_scale_0[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_0[1]) * _sensor_correction.gyro_scale_0[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_0[2]) * _sensor_correction.gyro_scale_0[2];

	} else if (_selected_gyro == 1) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_1[0]) * _sensor_correction.gyro_scale_1[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_1[1]) * _sensor_correction.gyro_scale_1[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_1[2]) * _sensor_correction.gyro_scale_1[2];

	} else if (_selected_gyro == 2) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_2[0]) * _sensor_correction.gyro_scale_2[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_2[1]) * _sensor_correction.gyro_scale_2[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_2[2]) * _sensor_correction.gyro_scale_2[2];

	} else {
		rates(0) = _sensor_gyro.x;
		rates(1) = _sensor_gyro.y;
		rates(2) = _sensor_gyro.z;
	}

	// rotate corrected measurements from sensor to body frame
	rates = _board_rotation * rates;

	// correct for in-run bias errors
	rates(0) -= _sensor_bias.gyro_x_bias;
	rates(1) -= _sensor_bias.gyro_y_bias;
	rates(2) -= _sensor_bias.gyro_z_bias;

	/* apply low-pass filtering to the rates for D-term */
	Vector3f rates_filtered(_lp_filters_d.apply(rates));

	/* angular rates error */
	// Should I accept rate sp for offboard control?
	Vector3f rates_err = rates/5;
	// rates_err(0) = 0.f;
	// rates_err(1) = 0.f;
	rates_err(1) *= 1/2;
	rates_err(2) = 0.f;
	// rates_err(2) *= -2;

	// Calculate final LQR output (rate) and combine with all previous partial controls (pos/vel/att)
	// Rotor 1
	_virtual_control_0(0) = _param_mpc_lqr_k17.get() * rates_err(0) + _param_mpc_lqr_k18.get() * rates_err(1) +
				_param_mpc_lqr_k19.get() * rates_err(2)
				+ _p_control_att_0(0) + _partial_controls.control[0];
	_virtual_control_0(1) = _param_mpc_lqr_k27.get() * rates_err(0) + _param_mpc_lqr_k28.get() * rates_err(1) +
				_param_mpc_lqr_k29.get() * rates_err(2)
				+ _p_control_att_0(1) + _partial_controls.control[1];
	_virtual_control_0(2) = _param_mpc_lqr_k37.get() * rates_err(0) + _param_mpc_lqr_k38.get() * rates_err(1) +
				_param_mpc_lqr_k39.get() * rates_err(2)
				+ _p_control_att_0(2) + _partial_controls.control[2];

	// Rotor 2
	_virtual_control_1(0) = _param_mpc_lqr_k47.get() * rates_err(0) + _param_mpc_lqr_k48.get() * rates_err(1) +
				_param_mpc_lqr_k49.get() * rates_err(2)
				+ _p_control_att_1(0) + _partial_controls.control[3];
	_virtual_control_1(1) = _param_mpc_lqr_k57.get() * rates_err(0) + _param_mpc_lqr_k58.get() * rates_err(1) +
				_param_mpc_lqr_k59.get() * rates_err(2)
				+ _p_control_att_1(1) + _partial_controls.control[4];
	_virtual_control_1(2) = _param_mpc_lqr_k67.get() * rates_err(0) + _param_mpc_lqr_k68.get() * rates_err(1) +
				_param_mpc_lqr_k69.get() * rates_err(2)
				+ _p_control_att_1(2) + _partial_controls.control[5];

	// // TESTING
	// _virtual_control_0(0) = _p_control_att_0(0) + _partial_controls.control[0];
	// _virtual_control_0(1) = _p_control_att_0(1) + _partial_controls.control[1];
	// _virtual_control_0(2) = _p_control_att_0(2) + _partial_controls.control[2];
	// _virtual_control_1(0) = _p_control_att_1(0) + _partial_controls.control[3];
	// _virtual_control_1(1) = _p_control_att_1(1) + _partial_controls.control[4];
	// _virtual_control_1(2) = _p_control_att_1(2) + _partial_controls.control[5];

	/* Check for negative thrust
	 * Correct by setting negative thrust to 0.1N and adding the difference to the other rotor
	 */
	if (_virtual_control_0(2) < 0.f) {
		_virtual_control_1(2) += 0.1f - _virtual_control_0(2);
		_virtual_control_0(2) = 0.1f;
	} else if (_virtual_control_1(2) < 0.f) {
		_virtual_control_0(2) += 0.1f - _virtual_control_1(2);
		_virtual_control_1(2) = 0.1f;
	}

	// Convert virtual (Fx/y/z) control input to actual (alpha/beta/T) input
	convert_virtual_input();

}

/*
 * Convert virtual (Fx/y/z) input to actual (alpha/beta/T) input
 * Input: 'virtual_control' vectors
 * Output: '_att_control' vector
 */
void
Multicopter6dofControl::convert_virtual_input()
{
	// Extract euler from rotation matrix
	_att_control_0(1) = -atan2f(_virtual_control_0(1), _virtual_control_0(2)) / 0.75f;
	_att_control_0(0) = atan2f(_virtual_control_0(0), _virtual_control_0(2)/cosf(_att_control_0(1))) / 0.75f;
	_att_control_0(2) = _virtual_control_0.norm() / _param_mpc_max_thrust.get();

	_att_control_1(1) = -atan2f(_virtual_control_1(1), _virtual_control_1(2)) / 0.75f;
	_att_control_1(0) = atan2f(_virtual_control_1(0), _virtual_control_1(2)/cosf(_att_control_1(1))) / 0.75f;
	_att_control_1(2) = _virtual_control_1.norm() / _param_mpc_max_thrust.get();

	/* For now do all control calculations in SI units (N,m,etc) then convert to normalised (-1 .. 1) range in the final step
	*  Consider doing all calculations normalised?
	*/

	// Calculate thrust (channel 3) for arming/disarming safety logic
	_att_control_thrust = (_att_control_0(2)+_att_control_1(2)) / 2;
}

void
Multicopter6dofControl::publish_rates_setpoint()
{
	_v_rates_sp.roll = _rates_sp(0);
	_v_rates_sp.pitch = _rates_sp(1);
	_v_rates_sp.yaw = _rates_sp(2);
	_v_rates_sp.thrust_body[0] = 0.0f;
	_v_rates_sp.thrust_body[1] = 0.0f;
	_v_rates_sp.thrust_body[2] = -_thrust_sp;
	_v_rates_sp.timestamp = hrt_absolute_time();
	orb_publish_auto(ORB_ID(vehicle_rates_setpoint), &_v_rates_sp_pub, &_v_rates_sp, nullptr, ORB_PRIO_DEFAULT);
}

void
Multicopter6dofControl::publish_rate_controller_status()
{
	rate_ctrl_status_s rate_ctrl_status = {};
	rate_ctrl_status.timestamp = hrt_absolute_time();
	rate_ctrl_status.rollspeed = _rates_prev(0);
	rate_ctrl_status.pitchspeed = _rates_prev(1);
	rate_ctrl_status.yawspeed = _rates_prev(2);
	rate_ctrl_status.rollspeed_integ = _rates_int(0);
	rate_ctrl_status.pitchspeed_integ = _rates_int(1);
	rate_ctrl_status.yawspeed_integ = _rates_int(2);
	orb_publish_auto(ORB_ID(rate_ctrl_status), &_controller_status_pub, &rate_ctrl_status, nullptr, ORB_PRIO_DEFAULT);
}

void
Multicopter6dofControl::publish_actuator_controls()
{
	_actuators.control[0] = (PX4_ISFINITE(_att_control_0(0))) ? _att_control_0(0) : 0.0f;
	_actuators.control[1] = (PX4_ISFINITE(_att_control_0(1))) ? _att_control_0(1) : 0.0f;
	_actuators.control[2] = (PX4_ISFINITE(_att_control_0(2))) ? _att_control_0(2) : 0.0f;
	_actuators.control[5] = (PX4_ISFINITE(_att_control_1(0))) ? _att_control_1(0) : 0.0f;
	_actuators.control[6] = (PX4_ISFINITE(_att_control_1(1))) ? _att_control_1(1) : 0.0f;
	_actuators.control[7] = (PX4_ISFINITE(_att_control_1(2))) ? _att_control_1(2) : 0.0f;
	_actuators.control[3] = (PX4_ISFINITE(_thrust_sp)) ? _thrust_sp : 0.0f;
	_actuators.timestamp = hrt_absolute_time();
	_actuators.timestamp_sample = _sensor_gyro.timestamp;

	/* scale effort by battery status */
	if (_param_mc_bat_scale_en.get() && _battery_status.scale > 0.0f) {
		for (int i = 0; i < 4; i++) {
			_actuators.control[i] *= _battery_status.scale;
		}
	}

	if (!_actuators_0_circuit_breaker_enabled) {
		orb_publish_auto(_actuators_id, &_actuators_0_pub, &_actuators, nullptr, ORB_PRIO_DEFAULT);
	}
}

void
Multicopter6dofControl::run()
{

	/*
	 * do subscriptions
	 */
	_v_att_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	_v_att_sp_sub = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	_partial_controls_sub = orb_subscribe(ORB_ID(partial_controls));
	_v_rates_sp_sub = orb_subscribe(ORB_ID(vehicle_rates_setpoint));
	_v_control_mode_sub = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_manual_control_sp_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	_vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	_motor_limits_sub = orb_subscribe(ORB_ID(multirotor_motor_limits));
	_battery_status_sub = orb_subscribe(ORB_ID(battery_status));

	_gyro_count = math::constrain(orb_group_count(ORB_ID(sensor_gyro)), 1, MAX_GYRO_COUNT);

	for (unsigned s = 0; s < _gyro_count; s++) {
		_sensor_gyro_sub[s] = orb_subscribe_multi(ORB_ID(sensor_gyro), s);
	}

	_sensor_correction_sub = orb_subscribe(ORB_ID(sensor_correction));
	_sensor_bias_sub = orb_subscribe(ORB_ID(sensor_bias));
	_vehicle_land_detected_sub = orb_subscribe(ORB_ID(vehicle_land_detected));
	_landing_gear_sub = orb_subscribe(ORB_ID(landing_gear));

	/* wakeup source: gyro data from sensor selected by the sensor app */
	px4_pollfd_struct_t poll_fds = {};
	poll_fds.events = POLLIN;

	const hrt_abstime task_start = hrt_absolute_time();
	hrt_abstime last_run = task_start;
	float dt_accumulator = 0.f;
	int loop_counter = 0;

	bool reset_yaw_sp = true;
	float attitude_dt = 0.f;

	while (!should_exit()) {

		// check if the selected gyro has updated first
		sensor_correction_poll();
		poll_fds.fd = _sensor_gyro_sub[_selected_gyro];

		/* wait for up to 100ms for data */
		int pret = px4_poll(&poll_fds, 1, 100);

		/* timed out - periodic check for should_exit() */
		if (pret == 0) {
			continue;
		}

		/* this is undesirable but not much we can do - might want to flag unhappy status */
		if (pret < 0) {
			PX4_ERR("poll error %d, %d", pret, errno);
			/* sleep a bit before next try */
			px4_usleep(100000);
			continue;
		}

		perf_begin(_loop_perf);

		/* run controller on gyro changes */
		if (poll_fds.revents & POLLIN) {
			const hrt_abstime now = hrt_absolute_time();

			// Guard against too small (< 0.2ms) and too large (> 20ms) dt's.
			const float dt = math::constrain(((now - last_run) / 1e6f), 0.0002f, 0.02f);
			last_run = now;

			/* copy gyro data */
			orb_copy(ORB_ID(sensor_gyro), _sensor_gyro_sub[_selected_gyro], &_sensor_gyro);

			/* run the rate controller immediately after a gyro update */
			if (_v_control_mode.flag_control_rates_enabled) {
				control_attitude_rates(dt);

				publish_actuator_controls();
				publish_rate_controller_status();
			}

			/* check for updates in other topics */
			vehicle_control_mode_poll();
			vehicle_status_poll();
			vehicle_motor_limits_poll();
			battery_status_poll();
			sensor_bias_poll();
			vehicle_land_detected_poll();
			landing_gear_state_poll();
			const bool manual_control_updated = vehicle_manual_poll();
			const bool attitude_updated = vehicle_attitude_poll();
			attitude_dt += dt;

			/* Check if we are in rattitude mode and the pilot is above the threshold on pitch
			 * or roll (yaw can rotate 360 in normal att control). If both are true don't
			 * even bother running the attitude controllers */
			if (_v_control_mode.flag_control_rattitude_enabled) {
				_v_control_mode.flag_control_attitude_enabled =
						fabsf(_manual_control_sp.y) <= _param_mc_ratt_th.get() &&
						fabsf(_manual_control_sp.x) <= _param_mc_ratt_th.get();
			}

			bool attitude_setpoint_generated = false;

			const bool is_hovering = _vehicle_status.is_rotary_wing && !_vehicle_status.in_transition_mode;

			// vehicle is a tailsitter in transition mode
			const bool is_tailsitter_transition = _vehicle_status.in_transition_mode && _is_tailsitter;

			bool run_att_ctrl = _v_control_mode.flag_control_attitude_enabled && (is_hovering || is_tailsitter_transition);


			if (run_att_ctrl) {
				if (attitude_updated) {
					// Generate the attitude setpoint from stick inputs if we are in Manual/Stabilized mode
					if (_v_control_mode.flag_control_manual_enabled &&
							!_v_control_mode.flag_control_altitude_enabled &&
							!_v_control_mode.flag_control_velocity_enabled &&
							!_v_control_mode.flag_control_position_enabled) {
						generate_attitude_setpoint(attitude_dt, reset_yaw_sp);
						attitude_setpoint_generated = true;
					}

					control_attitude();
					publish_rates_setpoint();
				}

			} else {
				/* attitude controller disabled, poll rates setpoint topic */
				if (_v_control_mode.flag_control_manual_enabled && is_hovering) {
					if (manual_control_updated) {
						/* manual rates control - ACRO mode */
						Vector3f man_rate_sp(
								math::superexpo(_manual_control_sp.y, _param_mc_acro_expo.get(), _param_mc_acro_supexpo.get()),
								math::superexpo(-_manual_control_sp.x, _param_mc_acro_expo.get(), _param_mc_acro_supexpo.get()),
								math::superexpo(_manual_control_sp.r, _param_mc_acro_expo_y.get(), _param_mc_acro_supexpoy.get()));
						_rates_sp = man_rate_sp.emult(_acro_rate_max);
						_thrust_sp = _manual_control_sp.z;
						publish_rates_setpoint();
					}

				} else {
					/* attitude controller disabled, poll rates setpoint topic */
					if (vehicle_rates_setpoint_poll()) {
						_rates_sp(0) = _v_rates_sp.roll;
						_rates_sp(1) = _v_rates_sp.pitch;
						_rates_sp(2) = _v_rates_sp.yaw;
						_thrust_sp = -_v_rates_sp.thrust_body[2];
					}
				}
			}

			if (_v_control_mode.flag_control_termination_enabled) {
				if (!_vehicle_status.is_vtol) {
					_rates_sp.zero();
					_rates_int.zero();
					_thrust_sp = 0.0f;
					_att_control_0.zero();
					_att_control_1.zero();
					_att_control_thrust = 0.0f;
					_virtual_control_0.zero();
					_virtual_control_1.zero();
					publish_actuator_controls();
				}
			}

			if (attitude_updated) {
				// reset yaw setpoint during transitions, tailsitter.cpp generates
				// attitude setpoint for the transition
				reset_yaw_sp = (!attitude_setpoint_generated && !_v_control_mode.flag_control_rattitude_enabled) ||
						_vehicle_land_detected.landed ||
						(_vehicle_status.is_vtol && _vehicle_status.in_transition_mode);

				attitude_dt = 0.f;
			}

			/* calculate loop update rate while disarmed or at least a few times (updating the filter is expensive) */
			if (!_v_control_mode.flag_armed || (now - task_start) < 3300000) {
				dt_accumulator += dt;
				++loop_counter;

				if (dt_accumulator > 1.f) {
					const float loop_update_rate = (float)loop_counter / dt_accumulator;
					_loop_update_rate_hz = _loop_update_rate_hz * 0.5f + loop_update_rate * 0.5f;
					dt_accumulator = 0;
					loop_counter = 0;
					_lp_filters_d.set_cutoff_frequency(_loop_update_rate_hz, _param_mc_dterm_cutoff.get());
				}
			}

			parameter_update_poll();
		}

		perf_end(_loop_perf);
	}

	orb_unsubscribe(_v_att_sub);
	orb_unsubscribe(_v_att_sp_sub);
	orb_unsubscribe(_partial_controls_sub);
	orb_unsubscribe(_v_rates_sp_sub);
	orb_unsubscribe(_v_control_mode_sub);
	orb_unsubscribe(_params_sub);
	orb_unsubscribe(_manual_control_sp_sub);
	orb_unsubscribe(_vehicle_status_sub);
	orb_unsubscribe(_motor_limits_sub);
	orb_unsubscribe(_battery_status_sub);

	for (unsigned s = 0; s < _gyro_count; s++) {
		orb_unsubscribe(_sensor_gyro_sub[s]);
	}

	orb_unsubscribe(_sensor_correction_sub);
	orb_unsubscribe(_sensor_bias_sub);
	orb_unsubscribe(_vehicle_land_detected_sub);
	orb_unsubscribe(_landing_gear_sub);
}

int Multicopter6dofControl::task_spawn(int argc, char *argv[])
{
	_task_id = px4_task_spawn_cmd("mc_6dof_control",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_ATTITUDE_CONTROL,
					   1700,
					   (px4_main_t)&run_trampoline,
					   (char *const *)argv);

	if (_task_id < 0) {
		_task_id = -1;
		return -errno;
	}

	return 0;
}

Multicopter6dofControl *Multicopter6dofControl::instantiate(int argc, char *argv[])
{
	return new Multicopter6dofControl();
}

int Multicopter6dofControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

extern "C" __EXPORT int mc_6dof_control_main(int argc, char *argv[]);
int mc_6dof_control_main(int argc, char *argv[])
{
	return Multicopter6dofControl::main(argc, argv);
}