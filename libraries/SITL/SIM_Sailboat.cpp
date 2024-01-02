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
#include <cassert>

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
    sail_area(1.5)
{
    Aircraft::mass = 4.0;  // kg
    motor_connected = (strcmp(frame_str, "sailboat-motor") == 0);
    lock_step_scheduled = true;
}

namespace {
   // vector of { angle in degrees, CL}
   Vector2F constexpr CL_curve[] =
   {
     {0.f, 0.f},
     {10.f, 0.5f}, // 1
     {20.f, 1.f},  // 2
     {30.f, 1.1f},  // 2.2
     {40.f, 0.95f}, // 1.
     {50.f, 0.75f}, // 0.5
     {60.f, 0.6f},   // 0.3
     {70.f, 0.4f},   // 0.2
     {80.f, 0.2f},   // 0.1
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
void Sailboat::calc_lift_and_drag(float wind_speed_m_per_s, float angle_of_attack_deg, float& lift, float& drag) const
{
    // Convert angle of attack to expected range for interpolation curves
    // ( +180 deg to - 180 deg )
    const float signed_aoa_deg = wrap_180(angle_of_attack_deg);

    const float abs_aoa_deg = fabs(signed_aoa_deg);

    float const cl = linear_interpolate(abs_aoa_deg, CL_curve, ARRAY_SIZE(CL_curve));
    float const cd = linear_interpolate(abs_aoa_deg, CD_curve, ARRAY_SIZE(CD_curve));

    // Lift equation FL = 1/2 * Cl * rho * wind_speed_m_per_s^2 * sail_area
    // Drag equation FD = 1/2 * Cd * rho * wind_speed_m_per_s^2 * sail_area
    // here we currently use quasi units for the coefficients common to both equations
    // TODO convert to actual si values
    // need rho -> air density in kg.m-3
    // actual sail area in m2
    // actual wind speed in m.s-1
    float air_density_kg_per_m3 = 1.225;
    float const common_coefficient = 1./2. * air_density_kg_per_m3 * sq(wind_speed_m_per_s) * sail_area;
    // force in direction of wind
    drag = cd * common_coefficient;
    // force normal to direction of wind
    lift = cl * common_coefficient * signum(signed_aoa_deg);

}

// return turning circle (diameter) in meters for steering proportion in the range -1 to +1
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
    float const d_m = get_turn_circle(steering); // turn circle in meters
    float const c_m = M_PI * d_m; // circumference
    float const t_s = c_m / speed;  //
    float const yaw_rate_deg_per_sec = 360.0f / t_s;
    return yaw_rate_deg_per_sec;
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

/**
  @brief return a heel angular acceleration in rad.s-2
  @param[in] heel_force is the rolling force in N
  @param[in] current_roll_angle_bf_rad is the current roll angle in radians
  @param[in] current_roll_rate_rad_per_s is current roll rate in rad/s
**/
float Sailboat::get_heel_angular_acceleration(float force_heel,
 float current_roll_angle_bf_rad, float current_roll_rate_rad_per_s)const
{
   // no  angular acceleration during gyro init
   if ( !hal.util->get_soft_armed()){
       return 0.f;
   }

   float vertical_ce = 200.f; // m

   float const keel_mass = 2.5f;  // kg
   float const keel_depth = 0.5f;  // m
   float const keel_chord = 0.1f;  // m
   float const g = 1.f;          // acceleration due to gravity m.s-2
   float const overturning_moment = force_heel * vertical_ce * cosf(current_roll_angle_bf_rad);
   float const righting_moment = -1.f * keel_mass * g * keel_depth * sinf(current_roll_angle_bf_rad);
   // damping drag as a result of drag of water on keel as it rotates
   // proportional to area and depth of keel and current rool rate
   // Force = area * 1/2 v^2 * cd * rho
   // moment = force * dist
   // kDamping = cd *rho ideally
   float const kDamping = 1.f;

   float const damping_moment =
     -1.f * sq(keel_depth) * keel_chord * current_roll_rate_rad_per_s * kDamping ;

   float const resultant = overturning_moment + righting_moment + damping_moment;
   float const kMomentOfInertia = 300.f;
   float const moment_of_inertia = keel_mass * sq(keel_depth) * kMomentOfInertia;  // mass * d^2


   return (resultant / moment_of_inertia) ;

}
/*
  mainsail angle in body frame degrees
*/
float Sailboat::get_mainsail_angle_bf(const struct sitl_input &input)const
{

    //float aoa_deg = 0.0f;
    auto const sail_type = sitl->sail_type.get();
    if ( sail_type == Sail_type::directly_actuated_wing) {
        // directly actuated wing
        return constrain_float((input.servos[DIRECT_WING_SERVO_CH]-1500)/500.0f * 90.0f, -90.0f, 90.0f);
    } else {
        assert( sail_type == Sail_type::mainsail_with_sheet);
        // mainsail with sheet
        // calculate mainsail angle from servo output 4, 0 to 90 degrees
        return constrain_float((input.servos[MAINSAIL_SERVO_CH]-1000)/1000.0f * 90.0f, 0.0f, 90.0f);
    }
    //return aoa_deg;
}

/*
  update the sailboat simulation by one time step
 */

namespace {
   float last_print_time_s = 0.f;
   float update_time_s = 0.f;
}
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
    Vector3f const wind_apparent_ef = Aircraft::velocity_ef - Aircraft::wind_ef;

#if (0)
    // earth frame apparent wind direction
    const float wind_apparent_dir_ef_old = degrees(atan2f(wind_apparent_ef.y, wind_apparent_ef.x));
    const float wind_apparent_speed_old = safe_sqrt(sq(wind_apparent_ef.x)+sq(wind_apparent_ef.y));

    float roll, pitch, yaw;
    dcm.to_euler(&roll, &pitch, &yaw);
#endif
    // Rotate the vector to body frame vector wind_vector_bf using dcm
    // to wind vector seen by boat
    Vector3f const wind_apparent_bf = dcm.mul_transpose(wind_apparent_ef);
    float const wind_apparent_dir_bf_signed = wrap_180(degrees(atan2(wind_apparent_bf.y,wind_apparent_bf.x)));
    //
    float const wind_apparent_speed_bf = safe_sqrt(sq(wind_apparent_bf.y)+sq(wind_apparent_bf.x));

#if 0
    // body frame apparent horizontal wind direction is atan2(wind_vector_bf.y,wind_vector_bf.x);
    // body frame wind speed is safe_sqrt(sq(wind_vector_bf.y) + sq(wind_vector_bf.x))
    const float wind_apparent_dir_bf_signed_old = wrap_180(wind_apparent_dir_ef_old - degrees(yaw));
#endif
    // set RPM and airspeed from wind speed, allows to test RPM and Airspeed wind vane back end in SITL
    rpm[0] = wind_apparent_speed_bf;
    airspeed_pitot = wind_apparent_speed_bf;

    float const mainsail_angle_bf = get_mainsail_angle_bf(input);

    // sail angle of attack
    float aoa_deg = 0.f ;
    if (sitl->sail_type.get() == Sail_type::directly_actuated_wing) {
        // directly actuated wing
        aoa_deg = wind_apparent_dir_bf_signed - mainsail_angle_bf;
    }else{
       // Sail_type::mainsail_with_sheet
        // Calculate angle-of-attack from wind to mainsail,
        // but cannot have negative angle of attack, sheet would go slack.
        aoa_deg =
           MAX(fabsf(wind_apparent_dir_bf_signed) - mainsail_angle_bf, 0) *
              signum(wind_apparent_dir_bf_signed);
    }

    // calculate Lift force (perpendicular to wind direction) and Drag force (parallel to wind direction)
    float lift_wf, drag_wf;
    calc_lift_and_drag(wind_apparent_speed_bf, aoa_deg, lift_wf, drag_wf);

    // rotate lift and drag from wind frame into body frame
    const float sin_rot_rad = sinf(radians(wind_apparent_dir_bf_signed));
    const float cos_rot_rad = cosf(radians(wind_apparent_dir_bf_signed));
    const float force_fwd = lift_wf * sin_rot_rad - drag_wf * cos_rot_rad;
    const float force_heel = lift_wf * cos_rot_rad + drag_wf * sin_rot_rad;

    // how much time has passed?
    float const delta_time = frame_time_us * 1.0e-6f;
    update_time_s += delta_time;

    // speed in m/s in body frame
    Vector3f const velocity_body = dcm.transposed() * velocity_ef_water;

    //create a vertical component representing a keel
    Vector3f const keel_ef{0.f,0.f,1.f};
    // rotate to body frame
    Vector3f const keel_bf = Aircraft::dcm.mul_transpose(keel_ef);

    auto const heelAngle_rad = wrap_PI(atan2(keel_bf.y, keel_bf.z));

    if ( (update_time_s - last_print_time_s) > 1.f){
       last_print_time_s = update_time_s;
       printf("roll %f deg\n",degrees(heelAngle_rad));
    }
    // speed along x axis, +ve is forward
    float const speed = velocity_body.x;
    // yaw rate in degrees/s
    float const yaw_rate = get_yaw_rate(steering, speed);

    float const roll_rate = gyro.x - get_heel_angular_acceleration(force_heel,heelAngle_rad, gyro.x) * delta_time;

    gyro = Vector3f(roll_rate,0,radians(yaw_rate)) + wave_gyro;

    // update attitude
    dcm.rotate(gyro * delta_time);
    dcm.normalize();

    // hull drag
    // waveDrag
    // skinFriction drag
    float constexpr hullDragGain = 0.5f;
    float const hull_drag = sq(speed) * Aircraft::mass * hullDragGain * signum(speed);

    // throttle force (for motor sailing)
    // gives throttle force == hull drag at 10m/s
    float throttle_force = 0.0f;
    if (motor_connected) {
        const uint16_t throttle_out = constrain_int16(input.servos[THROTTLE_SERVO_CH], 1000, 2000);
        throttle_force = (throttle_out-1500) * 0.1f;
    }

    // accel in body frame due acceleration from sail and deceleration from hull friction
    accel_body = Vector3f((throttle_force + force_fwd) - hull_drag, 0, 0);
    accel_body /= Aircraft::mass;

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
