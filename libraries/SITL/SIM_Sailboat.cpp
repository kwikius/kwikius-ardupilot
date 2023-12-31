/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
    Sailboat simulator class

    see explanation of lift and drag explained here: https://en.wikipedia.org/wiki/Forces_on_sails

    To-Do: add heel handling by calculating lateral force from wind vs gravity force from heel to arrive at roll rate or acceleration
*/

#include "SIM_Sailboat.h"
#include <AP_Math/AP_Math.h>
#include <string.h>
#include <stdio.h>

extern const AP_HAL::HAL& hal;

namespace SITL {

#define STEERING_SERVO_CH   0   // steering controlled by servo output 1
#define MAINSAIL_SERVO_CH   3   // main sail controlled by servo output 4
#define THROTTLE_SERVO_CH   2   // throttle controlled by servo output 3
#define DIRECT_WING_SERVO_CH 4

    // very roughly sort of a stability factors for waves
#define WAVE_ANGLE_GAIN 1
#define WAVE_HEAVE_GAIN 1

Sailboat::Sailboat(const char *frame_str) :
    Aircraft(frame_str),
    steering_angle_max(35),
    turning_circle(1.8),
    sail_area(1.0)
{
    motor_connected = (strcmp(frame_str, "sailboat-motor") == 0);
    lock_step_scheduled = true;
}

namespace {
   // vector of { angle in degrees, CL}
   Vector2F constexpr CL_curve[] =
   {
     {0.f, 0.f},
     {10.f, 0.5f},
     {20.f, 1.f},
     {30.f, 1.1f},
     {40.f, 0.95f},
     {50.f, 0.75f},
     {60.f, 0.6f},
     {70.f, 0.4f},
     {80.f, 0.2f},
     {90.f, 0.0f},
     // thes below should probably be less in magnitude
     {100.f, -0.2f},
     {110.f, -0.4f},
     {120.f, -0.6f},
     {130.f, -0.75f},
     {140.f, -0.95f},
     {150.f, -1.1f},
     {160.f, -1.f},
     {170.f, -0.5f},
     // should probably continue 360 degreees here..
   };

// vector of { angle in degrees, CD}
 Vector2F constexpr CD_curve[] =
   {
     {0.f, 0.1f},
     {10.f, 0.1f},
     {20.f, 0.2f},
     {30.f, 0.4f},
     {40.f, 0.8f},
     {50.f, 1.2f},
     {60.f, 1.5f},
     {70.f, 1.7f},
     {80.f, 1.9f},
     {90.f, 1.95f},
     {100.f, 1.9f},
     {110.f, 1.7f},
     {120.f, 1.5f},
     {130.f, 1.2f},
     {140.f, 0.8f},
     {150.f, 0.4f},
     {160.f, 0.2f},
     {170.f, 0.1f}
     // should probably continue 360 degreees here..
   };
}

/**
* Calculate the lift and drag
* given an apparent wind speed in m/s and angle-of-attack in degrees
* calculate Lift force (perpendicular to wind direction) and Drag force (parallel to wind direction)
**/
void Sailboat::calc_lift_and_drag(float wind_speed, float angle_of_attack_deg, float& lift, float& drag) const
{
    // Convert angle of attack to expected range for interpoltion curves
    // ( +180 deg to - 180 deg )
    angle_of_attack_deg = wrap_180(angle_of_attack_deg);
    //int const angle_off_attack_sign = is_negative(angle_of_attack_deg)? -1 : 1;
    const float abs_aoa_deg = fabs(angle_of_attack_deg);

    float const cl = linear_interpolate(abs_aoa_deg, CL_curve, ARRAY_SIZE(CL_curve));
    float const cd = linear_interpolate(abs_aoa_deg, CD_curve, ARRAY_SIZE(CD_curve));

    // Lift equation FL = 1/2 * Cl * rho * wind_speed^2 * sail_area
    // Drag equation FD = 1/2 * Cd * rho * wind_speed^2 * sail_area
    // here we currently use quasi units for the variables common to both equations
    // TODO convert to actual si values
    auto const f_max = wind_speed * wind_speed * sail_area;
    // force in direction of wind
    drag  = cd * f_max;
    // force normal to direction of wind
    lift = cl * f_max * signum(angle_of_attack_deg);
//    if (is_negative(angle_of_attack_deg)) {
//        // invert lift for negative aoa
//      lift = -cl * f_max;
//    }else{
//       lift = cl * f_max;
//    }
}

// return turning circle (diameter) in meters for steering angle proportion in the range -1 to +1
float Sailboat::get_turn_circle(float steering) const
{
    if (is_zero(steering)) {
        return 0;
    }
    return turning_circle * sinf(radians(steering_angle_max)) / sinf(radians(steering * steering_angle_max));
}

// return yaw rate in deg/sec given a steering input (in the range -1 to +1) and speed in m/s
float Sailboat::get_yaw_rate(float steering, float speed) const
{
    if (is_zero(steering) || is_zero(speed)) {
        return 0;
    }
    float d = get_turn_circle(steering);
    float c = M_PI * d;
    float t = c / speed;
    float rate = 360.0f / t;
    return rate;
}

// return lateral acceleration in m/s/s given a steering input (in the range -1 to +1) and speed in m/s
float Sailboat::get_lat_accel(float steering, float speed) const
{
    float yaw_rate = get_yaw_rate(steering, speed);
    float accel = radians(yaw_rate) * speed;
    return accel;
}

// simulate basic waves / swell
void Sailboat::update_wave(float delta_time)
{
    const float wave_heading = sitl->wave.direction;
    const float wave_speed = sitl->wave.speed;
    const float wave_length = sitl->wave.length;
    const float wave_amp = sitl->wave.amp;

    // apply rate propositional to error between boat angle and water angle
    // this gives a 'stability' effect
    float r, p, y;
    dcm.to_euler(&r, &p, &y);

    // if not armed don't do waves, to allow gyro init
    if (sitl->wave.enable == 0 || !hal.util->get_soft_armed() || is_zero(wave_amp) ) {
        wave_gyro = Vector3f(-r,-p,0.0f) * WAVE_ANGLE_GAIN;
        wave_heave = -velocity_ef.z * WAVE_HEAVE_GAIN;
        wave_phase = 0.0f;
        return;
    }

    // calculate the sailboat speed in the direction of the wave
    const float boat_speed = velocity_ef.x * sinf(radians(wave_heading)) + velocity_ef.y * cosf(radians(wave_heading));

    // update the wave phase
    const float apparent_wave_distance = (wave_speed - boat_speed) * delta_time;
    const float apparent_wave_phase_change = (apparent_wave_distance / wave_length) * M_2PI;

    wave_phase += apparent_wave_phase_change;
    wave_phase = wrap_2PI(wave_phase);

    // calculate the angles at this phase on the wave
    // use basic sine wave, dy/dx of sine = cosine
    // atan( cosine ) = wave angle
    const float wave_slope = (wave_amp * 0.5f) * (M_2PI / wave_length) * cosf(wave_phase);
    const float wave_angle = atanf(wave_slope);

    // convert wave angle to vehicle frame
    const float heading_dif = wave_heading - y;
    float angle_error_x = (sinf(heading_dif) * wave_angle) - r;
    float angle_error_y = (cosf(heading_dif) * wave_angle) - p;

    // apply gain
    wave_gyro.x = angle_error_x * WAVE_ANGLE_GAIN;
    wave_gyro.y = angle_error_y * WAVE_ANGLE_GAIN;
    wave_gyro.z = 0.0f;

    // calculate wave height (NED)
    if (sitl->wave.enable == 2) {
        wave_heave = (wave_slope - velocity_ef.z) * WAVE_HEAVE_GAIN;
    } else {
        wave_heave = 0.0f;
    }
}

/*
  update the sailboat simulation by one time step
 */
void Sailboat::update(const struct sitl_input &input)
{
    // update wind
    update_wind(input);

    // in sailboats the steering controls the rudder, the throttle controls the main sail position
    // steering input -1 to 1
    float steering = 2*((input.servos[STEERING_SERVO_CH]-1000)/1000.0f - 0.5f);

    // calculate apparent wind in earth-frame (this is the direction the wind is coming from)
    // Note than the SITL wind direction is defined as the direction the wind is travelling to
    // This is accounted for in these calculations
    Vector3f wind_apparent_ef = velocity_ef - wind_ef;
    // earth frame apparent wind direction
    const float wind_apparent_dir_ef = degrees(atan2f(wind_apparent_ef.y, wind_apparent_ef.x));
    const float wind_apparent_speed = safe_sqrt(sq(wind_apparent_ef.x)+sq(wind_apparent_ef.y));

    float roll, pitch, yaw;
    dcm.to_euler(&roll, &pitch, &yaw);

    // body frame apparent wind direction
    const float wind_apparent_dir_bf = wrap_180(wind_apparent_dir_ef - degrees(yaw));
    const int wind_apparent_dir_bf_sign = is_negative(wind_apparent_dir_bf)?-1:1;
    // set RPM and airspeed from wind speed, allows to test RPM and Airspeed wind vane back end in SITL
    rpm[0] = wind_apparent_speed;
    airspeed_pitot = wind_apparent_speed;

    float aoa_deg = 0.0f;
    if (sitl->sail_type.get() == 1) {
        // directly actuated wing
        float wing_angle_bf = constrain_float((input.servos[DIRECT_WING_SERVO_CH]-1500)/500.0f * 90.0f, -90.0f, 90.0f);

        aoa_deg = wind_apparent_dir_bf - wing_angle_bf;

    } else {
        // mainsail with sheet

        // calculate mainsail angle from servo output 4, 0 to 90 degrees
        float mainsail_angle_bf = constrain_float((input.servos[MAINSAIL_SERVO_CH]-1000)/1000.0f * 90.0f, 0.0f, 90.0f);

        // calculate angle-of-attack from wind to mainsail, cannot have negative angle of attack, sheet would go slack
        aoa_deg = MAX(fabsf(wind_apparent_dir_bf) - mainsail_angle_bf, 0) * wind_apparent_dir_bf_sign;

//        if (is_negative(wind_apparent_dir_bf)) {
//            // take into account the current tack
//            aoa_deg *= -1;
//        }

    }

    // calculate Lift force (perpendicular to wind direction) and Drag force (parallel to wind direction)
    float lift_wf, drag_wf;
    calc_lift_and_drag(wind_apparent_speed, aoa_deg, lift_wf, drag_wf);

    // rotate lift and drag from wind frame into body frame
    const float sin_rot_rad = sinf(radians(wind_apparent_dir_bf));
    const float cos_rot_rad = cosf(radians(wind_apparent_dir_bf));
    const float force_fwd = (lift_wf * sin_rot_rad) - (drag_wf * cos_rot_rad);

    const float force_heel = (lift_wf * cos_rot_rad) ; //- (drag_wf * sin_rot_rad);

    constexpr float k_pitch = 0.05;

    float heel_angle = constrain_float(force_heel * k_pitch,-45.f,45.f);

    // how much time has passed?
    float const delta_time = frame_time_us * 1.0e-6f;

    // speed in m/s in body frame
    Vector3f velocity_body = dcm.transposed() * velocity_ef_water;

    // speed along x axis, +ve is forward
    float const speed = velocity_body.x;
    int const speed_sign = is_negative(speed)?-1:1;
    // yaw rate in degrees/s
    float const yaw_rate = get_yaw_rate(steering, speed);

    gyro = Vector3f(0,0,radians(yaw_rate)) + wave_gyro;

    // update attitude
    dcm.rotate(gyro * delta_time);
    dcm.normalize();

    dcm.to_euler(&roll, &pitch, &yaw);
    roll = radians(heel_angle);
    dcm.from_euler(roll,pitch,yaw);
    dcm.normalize();

    // hull drag
    float const hull_drag = sq(speed) * 0.5f * speed_sign;
//    if (!is_positive(speed)) {
//        hull_drag *= -1.0f;
//    }

    // throttle force (for motor sailing)
    // gives throttle force == hull drag at 10m/s
    float throttle_force = 0.0f;
    if (motor_connected) {
        const uint16_t throttle_out = constrain_int16(input.servos[THROTTLE_SERVO_CH], 1000, 2000);
        throttle_force = (throttle_out-1500) * 0.1f;
    }

    // accel in body frame due acceleration from sail and deceleration from hull friction
    accel_body = Vector3f((throttle_force + force_fwd) - hull_drag, 0, 0);
    accel_body /= mass;

    // add in accel due to direction change
    accel_body.y += radians(yaw_rate) * speed;

    // now in earth frame
    // remove roll and pitch effects from waves
    float r, p, y;
    dcm.to_euler(&r, &p, &y);
    Matrix3f temp_dcm;
    temp_dcm.from_euler(0.0f, 0.0f, y);
    Vector3f accel_earth = temp_dcm * accel_body;

    // we are on the ground, so our vertical accel is zero
    accel_earth.z = 0 + wave_heave;

    // work out acceleration as seen by the accelerometers. It sees the kinematic
    // acceleration (ie. real movement), plus gravity
    accel_body = dcm.transposed() * (accel_earth + Vector3f(0, 0, -GRAVITY_MSS));

    // tide calcs
    Vector3f tide_velocity_ef;
     if (hal.util->get_soft_armed() && !is_zero(sitl->tide.speed) ) {
        tide_velocity_ef.x = -cosf(radians(sitl->tide.direction)) * sitl->tide.speed;
        tide_velocity_ef.y = -sinf(radians(sitl->tide.direction)) * sitl->tide.speed;
        tide_velocity_ef.z = 0.0f;
     }

    // new velocity vector
    velocity_ef_water += accel_earth * delta_time;
    velocity_ef = velocity_ef_water + tide_velocity_ef;

    // new position vector
    position += (velocity_ef * delta_time).todouble();

    // update lat/lon/altitude
    update_position();
    time_advance();

    // update magnetic field
    update_mag_field_bf();

    // update wave calculations
    update_wave(delta_time);

}

} // namespace SITL
