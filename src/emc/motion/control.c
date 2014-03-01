/********************************************************************
 * Description: control.c
 *   emcmotController() is the main loop running at the servo cycle
 *   rate. All state logic and trajectory calcs are called from here.
 *
 *   Derived from a work by Fred Proctor & Will Shackleford
 *
 * Author:
 * License: GPL Version 2
 * Created on:
 * System: Linux
 *
 * Copyright (c) 2004 All rights reserved.
 ********************************************************************/
#include <stdint.h>

#include "posemath.h"
#include "rtapi.h"
#include "hal.h"
#include "emcmotglb.h"
#include "motion.h"
#include "mot_priv.h"
#include "rtapi_math.h"
#include "tp.h"
#include "tc.h"
#include "simple_tp.h"
#include "motion_debug.h"
#include "config.h"
#include "assert.h"
#include <sync_cmd.h>

// Mark strings for translation, but defer translation to userspace
#define _(s) (s)
#define PI                      3.141592653589

static int num_dio = DEFAULT_DIO;       /* default number of motion synched DIO */

/* kinematics flags */
KINEMATICS_FORWARD_FLAGS fflags = 0;
KINEMATICS_INVERSE_FLAGS iflags = 0;

/* 1/servo cycle time */
double servo_freq;

// to disable DP(): #define TRACE 0
#define TRACE 0
#include "dptrace.h"
#if (TRACE!=0)
static FILE* dptrace = 0;
static uint32_t _dt = 0;
#endif

#define CSS_TRACE 0
#if (CSS_TRACE!=0)
#if (TRACE==0)
static FILE* csstrace = 0;
static uint32_t _dt = 0;
#else
#undef CSS_TRACE // disable CSS_TRACE when TRACE is enabled
#endif
#endif

/* debugging function - prints a cartesean pose (multplies the floating
   point numbers by 1 million since kernel printf doesn't handle floats
 */

void print_pose ( EmcPose *pos )
{
    rtapi_print(" (%d, %d, %d):(%d, %d, %d),(%d, %d, %d) ",
            (int)(pos->tran.x*1000000.0),
            (int)(pos->tran.y*1000000.0),
            (int)(pos->tran.z*1000000.0),
            (int)(pos->a*1000000.0),
            (int)(pos->b*1000000.0),
            (int)(pos->c*1000000.0),
            (int)(pos->u*1000000.0),
            (int)(pos->v*1000000.0),
            (int)(pos->w*1000000.0));
}

/*! \todo FIXME - debugging - uncomment the following line to log changes in
   JOINT_FLAG and MOTION_FLAG */
// #define WATCH_FLAGS 1

/* debugging function - it watches a particular variable and
   prints a message when the value changes.  Right now there are
   calls to this scattered throughout this and other files.
   To disable them, comment out the following define:
 */
// #define ENABLE_CHECK_STUFF

#ifdef ENABLE_CHECK_STUFF
void check_stuff(const char *location)
{
    static char *target, old = 0xFF;
    /*! \todo Another #if 0 */
#if 0
    /* kludge to look at emcmotDebug->enabling and emcmotStatus->motionFlag
   at the same time - we simply use a high bit of the flags to
   hold "enabling" */
    short tmp;
    if ( emcmotDebug->enabling )
        tmp = 0x1000;
    else
        tmp = 0x0;
    tmp |= emcmotStatus->motionFlag;
    target = &tmp;
    /* end of kluge */
#endif

    target = (emcmot_hal_data->enable);
    if ( old != *target ) {
        rtapi_print ( "%d: watch value %02X (%s)\n", emcmotStatus->heartbeat, *target, location );
        old = *target;
    }
}
#else /* make it disappear */
void check_stuff(const char *location)
{
    /* do nothing (I wonder if gcc is smart
   enough to optimize the calls away?) */
}
#endif /* ENABLE_CHECK_STUFF */

/***********************************************************************
 *                  LOCAL VARIABLE DECLARATIONS                         *
 ************************************************************************/

/* the (nominal) period the last time the motion handler was invoked */
static unsigned long last_period = 0;

/* servo cycle time */
static double servo_period;

/***********************************************************************
 *                      LOCAL FUNCTION PROTOTYPES                       *
 ************************************************************************/

/* the following functions are called (in this order) by the main
   controller function.  They are an attempt to break the huge
   function (originally 1600 lines) into something a little easier
   to understand.
 */

/* 'process_inputs()' is responsible for reading hardware input
   signals (from the HAL) and doing basic processing on them.  In
   the case of position feedback, that means removing backlash or
   screw error comp and calculating the following error.  For 
   switches, it means debouncing them and setting flags in the
   emcmotStatus structure.
 */
static void process_inputs(void);

/* 'do forward kins()' takes the position feedback in joint coords
   and applies the forward kinematics to it to generate feedback
   in Cartesean coordinates.  It has code to handle machines that
   don't have forward kins, and other special cases, such as when
   the joints have not been homed.
 */
static void do_forward_kins(void);

/* probe inputs need to be handled after forward kins are run, since
   cartesian feedback position is latched when the probe fires, and it
   should be based on the feedback read in on this servo cycle.
 */
static void process_probe_inputs(void);

/* 'check_for_faults()' is responsible for detecting fault conditions
   such as limit switches, amp faults, following error, etc.  It only
   checks active axes.  It is also responsible for generating an error
   message.  (Later, once I understand the cmd/status/error interface
   better, it will probably generate error codes that can be passed
   up the architecture toward the GUI - printing error messages
   directly seems a little messy)
 */
static void check_for_faults(void);

/* 'set_operating_mode()' handles transitions between the operating
   modes, which are free, coordinated, and teleop.  This stuff needs
   to be better documented.  It is basically a state machine, with
   a current state, a desired state, and rules determining when the
   state can change.  It should be rewritten as such, but for now
   it consists of code copied exactly from emc1.
 */
static void set_operating_mode(void);

/* 'handle_jogwheels()' reads jogwheels, decides if they should be
   enabled, and if so, changes the free mode planner's target position
   when the jogwheel(s) turn.
 */
static void handle_jogwheels(void);

/* 'do_homing_sequence()' looks at emcmotStatus->homingSequenceState 
   to decide what, if anything, needs to be done related to multi-joint
   homing.

   no prototype here, implemented in homing.c, proto in mot_priv.h
 */

/* 'do_homing()' looks at the home_state field of each joint struct
    to decide what, if anything, needs to be done related to homing
    the joint.  Homing is implemented as a state machine, the exact
    sequence of states depends on the machine configuration.  It
    can be as simple as immediately setting the current position to
    zero, or a it can be a multi-step process (find switch, set
    approximate zero, back off switch, find index, set final zero,
    rapid to home position), or anywhere in between.

   no prototype here, implemented in homing.c, proto in mot_priv.h
 */

/* 'get_pos_cmds()' generates the position setpoints.  This includes
   calling the trajectory planner and interpolating its outputs.
 */
static void get_pos_cmds(long period);
static void get_spindle_cmds(double servo_period);

/* 'compute_screw_comp()' is responsible for calculating backlash and
   lead screw error compensation.  (Leadscrew error compensation is
   a more sophisticated version that includes backlash comp.)  It uses
   the velocity in emcmotStatus->joint_vel_cmd to determine which way
   each joint is moving, and the position in emcmotStatus->joint_pos_cmd
   to determine where the joint is at.  That information is used to
   create the compensation value that is added to the joint_pos_cmd
   to create motor_pos_cmd, and is subtracted from motor_pos_fb to
   get joint_pos_fb.  (This function does not add or subtract the
   compensation value, it only computes it.)  The basic compensation
   value is in backlash_corr, however has makes step changes when
   the direction reverses.  backlash_filt is a ramped version, and
   that is the one that is later added/subtracted from the position.
 */
static void compute_screw_comp(void);

/* 'output_to_hal()' writes the handles the final stages of the
   control function.  It applies screw comp and writes the
   final motor position to the HAL (which routes it to the PID
   loop).  It also drives other HAL outputs, and it writes a
   number of internal variables to HAL parameters so they can
   be observed with halscope and halmeter.
 */
static void output_to_hal(void);

/* 'update_status()' copies assorted status information to shared
   memory (the emcmotStatus structure) so that it is available to
   higher level code.
 */
static void update_status(void);

static void handle_special_cmd(void);

/***********************************************************************
 *                        PUBLIC FUNCTION CODE                          *
 ************************************************************************/

/*
  emcmotController() runs the trajectory and interpolation calculations
  each control cycle

  This function gets called at regular intervals - therefore it does NOT
  have a loop within it!

  Inactive axes are still calculated, but the PIDs are inhibited and
  the amp enable/disable are inhibited
 */
void emcmotController(void *arg, long period)
{
    // - overrun detection -
    // maintain some records of how long it's been between calls.  The
    // first time we see a delay that's much longer than the records show
    // is normal, report an error.  This might detect bogus realtime 
    // performance caused by ACPI, onboard video, etc.  It can be reliably
    // triggered by maximizing glxgears on my nvidia system, which also
    // causes the rtai latency test to show overruns.

    // check below if you set this under 5
#define CYCLE_HISTORY 5


    static long long int last = 0;
#ifndef RTAPI_SIM
    static int index = 0;
    static long int cycles[CYCLE_HISTORY];
    static int priming = 1;
#endif

    long long int now = rtapi_get_clocks();
    long int this_run = (long int)(now - last);
    emcmot_hal_data->last_period = this_run;
#ifdef HAVE_CPU_KHZ
    emcmot_hal_data->last_period_ns = this_run * 1e6 / cpu_khz;
#endif

#if (TRACE!=0)
    if(!dptrace) {
        dptrace = fopen("control.log", "w");
        /* prepare header for gnuplot */
        DPS ("#%10s%10s%10s%10s%10s%17s%17s%17s%17s\n",
                "dt", "x.pos_cmd", "y.pos_cmd", "z.pos_cmd", "a.pos_cmd",
                "joint[0]", "joint[1]", "joint[2]", "joint[3]");
    }
    _dt+=1;
#endif

#if (CSS_TRACE!=0)
    if (!csstrace) {
        csstrace = fopen("css.log", "w");
        _dt = 0;
    }
#endif
#ifndef RTAPI_SIM
    if(!priming) {
        // we have CYCLE_HISTORY samples, so check for this call being 
        // anomolously late
        int i;

        for(i=0; i<CYCLE_HISTORY; i++) {
            if (this_run > 1.2 * cycles[i]) {
                emcmot_hal_data->overruns++;
                // print message on first overrun only
                if(emcmot_hal_data->overruns == 1) {
                    int saved_level = rtapi_get_msg_level();
                    rtapi_set_msg_level(RTAPI_MSG_ALL);
                    reportError(_("Unexpected realtime delay: check dmesg for details."));
                    rtapi_print_msg(RTAPI_MSG_WARN,
                            _("\nIn recent history there were\n"
                                    "%ld, %ld, %ld, %ld, and %ld\n"
                                    "elapsed clocks between calls to the motion controller.\n"),
                                    cycles[0], cycles[1], cycles[2], cycles[3], cycles[4]);
                    rtapi_print_msg(RTAPI_MSG_WARN,
                            _("This time, there were %ld which is so anomalously\n"
                                    "large that it probably signifies a problem with your\n"
                                    "realtime configuration.  For the rest of this run of\n"
                                    "EMC, this message will be suppressed.\n\n"),
                                    this_run);
                    rtapi_set_msg_level(saved_level);
                }

                break;
            }
        }
    }
    if(last) {
        cycles[index++] = this_run;
    }
    if(index == CYCLE_HISTORY) {
        // wrap around to the start of the array
        index = 0;
        // we now have CYCLE_HISTORY good samples, so start checking times
        priming = 0;
    }
#endif
    // we need this for next time
    last = now;

    // end of overrun detection

    /* calculate servo period as a double - period is in integer nsec */
    servo_period = period * 0.000000001;

    if(period != last_period) {
        emcmotSetCycleTime(period);
        last_period = period;
    }

    /* calculate servo frequency for calcs like vel = Dpos / period */
    /* it's faster to do vel = Dpos * freq */
    servo_freq = 1.0 / servo_period;
    /* increment head count to indicate work in progress */
    emcmotStatus->head++;
    /* here begins the core of the controller */

    check_stuff ( "before process_inputs()" );
    process_inputs();
    check_stuff ( "after process_inputs()" );
    do_forward_kins();
    check_stuff ( "after do_forward_kins()" );
    process_probe_inputs();
    handle_special_cmd();
    check_stuff ( "after process_probe_inputs()" );
    check_for_faults();
    check_stuff ( "after check_for_faults()" );
    set_operating_mode();
    check_stuff ( "after set_operating_mode()" );
    handle_jogwheels();
    check_stuff ( "after handle_jogwheels()" );
    do_homing_sequence();
    check_stuff ( "after do_homing_sequence()" );
    do_homing();
    check_stuff ( "after do_homing()" );
    if (*(emcmot_hal_data->usb_busy) == 0) {
        get_spindle_cmds (servo_period);
        get_pos_cmds (period);
    }
    check_stuff ( "after get_pos_cmds()" );
    compute_screw_comp();
    check_stuff ( "after compute_screw_comp()" );
    output_to_hal();
    check_stuff ( "after output_to_hal()" );
    update_status();
    check_stuff ( "after update_status()" );
    /* here ends the core of the controller */
    emcmotStatus->heartbeat++;
    /* set tail to head, to indicate work complete */
    emcmotStatus->tail = emcmotStatus->head;
    /* clear init flag */
    first_pass = 0;

    /* end of controller function */
}

/***********************************************************************
 *                         LOCAL FUNCTION CODE                          *
 ************************************************************************/

/* The protoypes and documentation for these functions are located
   at the top of the file in the section called "local function
   prototypes"
 */

static void process_inputs(void)
{
    int joint_num;
    double abs_ferror, tmp, scale;
    joint_hal_t *joint_data;
    emcmot_joint_t *joint;
    unsigned char enables;

    /* read usb status */
    emcmotStatus->usb_status = *emcmot_hal_data->usb_status;

    /* read spindle angle (for threading, etc) */
    emcmotStatus->spindleSpeedIn = *emcmot_hal_data->spindle_speed_in;
    emcmotStatus->spindle.at_speed = *emcmot_hal_data->spindle_is_atspeed;
    emcmotStatus->spindle.update_pos_req = *emcmot_hal_data->spindle_update_pos_req;
    emcmotStatus->spindle.curr_pos_cmd = *emcmot_hal_data->spindle_curr_pos_cmd;
    emcmotStatus->spindle.curr_vel_rps = *emcmot_hal_data->spindle_curr_vel_rps;
    emcmotStatus->spindle.on = *emcmot_hal_data->spindle_on;
    emcmotStatus->spindle.dynamic_speed_mode = *emcmot_hal_data->spindle_dynamic_speed_mode;
    emcmotStatus->spindle.const_speed_radius = *emcmot_hal_data->spindle_const_speed_radius;


    /* compute net feed and spindle scale factors */
    if ( emcmotStatus->motion_state == EMCMOT_MOTION_COORD ) {
        /* use the enables that were queued with the current move */
        enables = emcmotStatus->enables_queued;
    } else {
        /* use the enables that are in effect right now */
        enables = emcmotStatus->enables_new;
    }
    /* feed scaling first:  feed_scale, adaptive_feed, and feed_hold */
    scale = 1.0;
    if ( enables & FS_ENABLED ) {
        scale *= emcmotStatus->feed_scale;
    }
    if ( enables & AF_ENABLED ) {
        /* read and clamp (0.0 to 1.0) adaptive feed HAL pin */
        tmp = *emcmot_hal_data->adaptive_feed;
        if ( tmp > 1.0 ) {
            tmp = 1.0;
        } else if ( tmp < 0.0 ) {
            tmp = 0.0;
        }
        scale *= tmp;
    }
    if ( enables & FH_ENABLED ) {
        /* read feed hold HAL pin */
        if ( *emcmot_hal_data->feed_hold ) {
            scale = 0;
        }
    }
    /* save the resulting combined scale factor */
    emcmotStatus->net_feed_scale = scale;

    /* now do spindle scaling: only one item to consider */
    scale = 1.0;
    if ( enables & SS_ENABLED ) {
        scale *= emcmotStatus->spindle_scale;
    }
    /* save the resulting combined scale factor */
    emcmotStatus->net_spindle_scale = scale;

    /* read and process per-joint inputs */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint HAL data */
        joint_data = &(emcmot_hal_data->joint[joint_num]);
        /* point to joint data */
        joint = &joints[joint_num];
        if (!GET_JOINT_ACTIVE_FLAG(joint)) {
            /* if joint is not active, skip it */
            continue;
        }
        /* copy data from HAL to joint structure */
        joint->index_enable = *(joint_data->index_enable);
        joint->motor_pos_fb = *(joint_data->motor_pos_fb);  // absolute motor position
        joint->index_pos = *(joint_data->index_pos_pin);  // absolute switch position
        joint->pos_fb = joint->motor_pos_fb -
                (joint->backlash_filt + joint->motor_offset);
        joint->risc_pos_cmd = *(joint_data->risc_pos_cmd);
        joint->blender_offset = *(joint_data->blender_offset);

        /* calculate following error */
        joint->ferror = joint->pos_cmd - joint->pos_fb;
        abs_ferror = fabs(joint->ferror);
        /* update maximum ferror if needed */
        if (abs_ferror > joint->ferror_high_mark) {
            joint->ferror_high_mark = abs_ferror;
        }

        /* calculate following error limit */
        // TODO: port ferror limit to risc
        // (curr_vel / max_vel) * max_ferror = current_ferror_limit
        // if current_ferror_limit > min_ferror => issue ferror
        if (joint->vel_limit > 0.0) {
            joint->ferror_limit =
                    joint->max_ferror * fabs(joint->vel_cmd) / joint->vel_limit;
        } else {
            joint->ferror_limit = 0;
        }
        if (joint->ferror_limit < joint->min_ferror) {
            joint->ferror_limit = joint->min_ferror;
        }
        /* update following error flag */

#ifndef MOTION_OVER_USB
        if (abs_ferror > joint->ferror_limit) {
            SET_JOINT_FERROR_FLAG(joint, 1);
        } else {
            SET_JOINT_FERROR_FLAG(joint, 0);
        }
#else
        SET_JOINT_FERROR_FLAG(joint, *(joint_data->usb_ferror_flag));
#endif
        /* read limit switches */
        if ((joint->home_flags & HOME_IGNORE_LIMITS) &&
                joint->home_state != HOME_IDLE) {
            // do nothing
        } else {
            if (*(joint_data->pos_lim_sw)) {
                SET_JOINT_PHL_FLAG(joint, 1);
            } else {
                SET_JOINT_PHL_FLAG(joint, 0);
            }
            if (*(joint_data->neg_lim_sw)) {
                SET_JOINT_NHL_FLAG(joint, 1);
            } else {
                SET_JOINT_NHL_FLAG(joint, 0);
            }
            joint->on_pos_limit = GET_JOINT_PHL_FLAG(joint);
            joint->on_neg_limit = GET_JOINT_NHL_FLAG(joint);
        }

        /* read amp fault input */
        if (*(joint_data->amp_fault)) {
            SET_JOINT_FAULT_FLAG(joint, 1);
        } else {
            SET_JOINT_FAULT_FLAG(joint, 0);
        }

        /* read home switch input */
        if (*(joint_data->home_sw)) {
            SET_JOINT_HOME_SWITCH_FLAG(joint, 1);
        } else {
            SET_JOINT_HOME_SWITCH_FLAG(joint, 0);
        }
        joint->home_sw_id = *(joint_data->home_sw_id);
        joint->probed_pos = *(joint_data->probed_pos);
        /* end of read and process joint inputs loop */
    }

    // a fault was signalled during a spindle-orient in progress
    // signal error, and cancel the orient
    if (*(emcmot_hal_data->spindle_orient)) {
        if (*(emcmot_hal_data->spindle_orient_fault)) {
            emcmotStatus->spindle.orient_state = EMCMOT_ORIENT_FAULTED;
            *(emcmot_hal_data->spindle_orient) = 0;
            emcmotStatus->spindle.orient_fault = *(emcmot_hal_data->spindle_orient_fault);
            reportError(_("fault %d during orient in progress"), emcmotStatus->spindle.orient_fault);
            emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_COMMAND;
            tpAbort(&emcmotDebug->coord_tp);
            SET_MOTION_ERROR_FLAG(1);
        } else if (*(emcmot_hal_data->spindle_is_oriented)) {
            *(emcmot_hal_data->spindle_orient) = 0;
            *(emcmot_hal_data->spindle_locked) = 1;
            emcmotStatus->spindle.locked = 1;
            emcmotStatus->spindle.brake = 1;
            emcmotStatus->spindle.orient_state = EMCMOT_ORIENT_COMPLETE;
            rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_ORIENT complete, spindle locked");
        }
    }
}

static void do_forward_kins(void)
{
    /* there are four possibilities for kinType:

   IDENTITY: Both forward and inverse kins are available, and they
   can used without an initial guess, even if one or more joints
   are not homed.  In this case, we apply the forward kins to the
   joint->pos_fb to produce carte_pos_fb, and if all axes are homed
   we set carte_pos_fb_ok to 1 to indicate that the feedback data
   is good.

   BOTH: Both forward and inverse kins are available, but the forward
   kins need an initial guess, and/or the kins require all joints to
   be homed before they work properly.  Here we must tread carefully.
   IF all the joints have been homed, we apply the forward kins to
   the joint->pos_fb to produce carte_pos_fb, and set carte_pos_fb_ok
   to indicate that the feedback is good.  We use the previous value
   of carte_pos_fb as the initial guess.  If all joints have not been
   homed, we don't call the kinematics, instead we set carte_pos_fb to
   the cartesean coordinates of home, as stored in the global worldHome,
   and we set carte_fb_ok to 0 to indicate that the feedback is invalid.
\todo  FIXME - maybe setting to home isn't the right thing to do.  We need
   it to be set to home eventually, (right before the first attemt to
   run the kins), but that doesn't mean we should say we're at home
   when we're not.

   INVERSE_ONLY: Only inverse kinematics are available, forward
   kinematics cannot be used.  So we have to fake it, the question is
   simply "what way of faking it is best".  In free mode, or if all
   axes have not been homed, the feedback position is unknown.  If
   we are in teleop or coord mode, or if we are in free mode and all
   axes are homed, and haven't been moved since they were homed, then
   we set carte_pos_fb to carte_pos_cmd, and set carte_pos_fb_ok to 1.
   If we are in free mode, and any joint is not homed, or any joint has
   moved since it was homed, we leave cart_pos_fb alone, and set
   carte_pos_fb_ok to 0.

   FORWARD_ONLY: Only forward kinematics are available, inverse kins
   cannot be used.  This exists for completeness only, since EMC won't
   work without inverse kinematics.

     */

    /*! \todo FIXME FIXME FIXME - need to put a rate divider in here, run it
   at the traj rate */

    double joint_pos[EMCMOT_MAX_JOINTS] = {0,};
    int joint_num, result;
    emcmot_joint_t *joint;

    /* copy joint position feedback to local array */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint struct */
        joint = &joints[joint_num];
        /* copy feedback */
        joint_pos[joint_num] = joint->pos_fb;
    }
    switch (emcmotConfig->kinType) {
    // switch (emcmotConfig->kinType)

    case KINEMATICS_IDENTITY:
        kinematicsForward(joint_pos, &emcmotStatus->carte_pos_fb, &fflags,
                &iflags);
        if (checkAllHomed()) {
            emcmotStatus->carte_pos_fb_ok = 1;
        } else {
            emcmotStatus->carte_pos_fb_ok = 0;
        }
        break;

    case KINEMATICS_BOTH:
        if (checkAllHomed()) {
            /* is previous value suitable for use as initial guess? */
            if (!emcmotStatus->carte_pos_fb_ok) {
                /* no, use home position as initial guess */
                emcmotStatus->carte_pos_fb = emcmotStatus->world_home;
            }
            /* calculate Cartesean position feedback from joint pos fb */
            result =
                    kinematicsForward(joint_pos, &emcmotStatus->carte_pos_fb,
                            &fflags, &iflags);
            /* check to make sure kinematics converged */
            if (result < 0) {
                /* error during kinematics calculations */
                emcmotStatus->carte_pos_fb_ok = 0;
            } else {
                /* it worked! */
                emcmotStatus->carte_pos_fb_ok = 1;
            }
        } else {
            emcmotStatus->carte_pos_fb_ok = 0;
        }
        break;

    case KINEMATICS_INVERSE_ONLY:

        if ((GET_MOTION_COORD_FLAG()) || (GET_MOTION_TELEOP_FLAG())) {
            /* use Cartesean position command as feedback value */
            emcmotStatus->carte_pos_fb = emcmotStatus->carte_pos_cmd;
            emcmotStatus->carte_pos_fb_ok = 1;
        } else {
            emcmotStatus->carte_pos_fb_ok = 0;
        }
        break;

    default:
        emcmotStatus->carte_pos_fb_ok = 0;
        break;
    }
}

#define PROBE_CMD_TYPE 0x0001
static void process_probe_inputs(void)
{
    unsigned char probe_type = emcmotStatus->probe_type;

    // interp_convert.cc: probe_type = g_code - G_38_2;
    // G38.2: probe_type = 0, stop on contact, signal error if failure
    // G38.3: probe_type = 1, stop on contact
    // G38.4: probe_type = 2, stop on loss of contact, signal error if failure
    // G38.5: probe_type = 3, stop on loss of contact

    // don't error
    char probe_suppress = probe_type & 1;  // suppressed: G38.2, G38.4

    // trigger when the probe clears, instead of the usual case of triggering when it trips
    char probe_whenclears = (probe_type & 2);

    /* read probe input */
    emcmotStatus->probeVal = !!*(emcmot_hal_data->probe_input);
    switch ( emcmotStatus->usb_status & 0x0000000F) // probe status mask
    {
    case USB_STATUS_PROBING:
        DP("probe: USB_STATUS_PROBING begin\n");
        if (GET_MOTION_INPOS_FLAG() && tpQueueDepth(&emcmotDebug->coord_tp) == 0)
        {
            emcmotStatus->probeTripped = 0;
            // ack risc to stop probing
            emcmotStatus->probe_cmd = USB_CMD_STATUS_ACK;
            emcmotStatus->usb_cmd = PROBE_CMD_TYPE;
            emcmotStatus->usb_cmd_param[0] = emcmotStatus->probe_cmd;
        }
        break;

    case USB_STATUS_PROBE_HIT:
        DP("probe: USB_STATUS_PROBE_HIT begin\n");
        emcmotStatus->probeTripped = 1;
        /* tell USB that we've got the status */
        DP("sending USB_CMD_STATUS_ACK\n");
        emcmotStatus->probe_cmd = USB_CMD_STATUS_ACK;
        emcmotStatus->usb_cmd = PROBE_CMD_TYPE;
        emcmotStatus->usb_cmd_param[0] = emcmotStatus->probe_cmd;
        break;

    case USB_STATUS_READY: // PROBE STATUS Clean
        // deal with PROBE related status only
        if ((emcmotStatus->probe_cmd == USB_CMD_STATUS_ACK) && emcmotStatus->probing)
        {
            if (emcmotStatus->probeTripped == 1)
            {
                int32_t joint_num;
                emcmot_joint_t *joint;
                /* update probed pos */
                double joint_pos[EMCMOT_MAX_JOINTS] = {0,};
                for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++)
                {
                    joint = &joints[joint_num];
                    joint_pos[joint_num] =  joint->probed_pos - (joint->backlash_filt + joint->motor_offset + joint->blender_offset);
                }
                kinematicsForward(joint_pos, &emcmotStatus->probedPos, &fflags, &iflags);
            } else
            {
                /* remember the current position */
                emcmotStatus->probedPos = emcmotStatus->carte_pos_cmd;
                if (probe_suppress == 0)
                {
                    if(probe_whenclears) 
                    {
                        reportError(_("G38.4 move finished without breaking contact."));
                        SET_MOTION_ERROR_FLAG(1);
                    } else 
                    {
                        reportError(_("G38.2 move finished without making contact."));
                        SET_MOTION_ERROR_FLAG(1);
                    }
                }
            }
            DP("emcmotStatus->depth(%d) paused(%d) probe_cmd(%d)\n", emcmotStatus->depth, emcmotStatus->paused, emcmotStatus->probe_cmd);
            tpAbort(&emcmotDebug->coord_tp);
            emcmotStatus->probing = 0;
        }

        if ((*emcmot_hal_data->update_pos_req == 0) &&
            (emcmotStatus->probe_cmd == USB_CMD_STATUS_ACK))
        {
            DP("to issue USB_CMD_NOOP\n");
            emcmotStatus->probe_cmd = USB_CMD_NOOP;
        }

        break;
    default:
        break;
    }
}

static int update_current_pos = 0;
static void handle_special_cmd(void)
{
    if (*emcmot_hal_data->req_cmd_sync == 1) {
        DP("req_cmd_sync(%d)\n", *emcmot_hal_data->req_cmd_sync);
        emcmotStatus->sync_pos_cmd = 1;
        update_current_pos = 1;
        printf("ERROR: handle_special_cmd(): req_cmd_sync(1)\n");
        assert(0);
    } else {
        emcmotStatus->sync_pos_cmd = 0;
    }

    /* must not be homing */
    if (emcmotStatus->homing_active) {
        // let usb_homing.c control the "update_pos_req" and "rcmd_seq_num_ack"
        return;
    }

    if (emcmotStatus->depth == 0)
    {   // not at EMCMOT_MOTION_COORD mode
        emcmotStatus->update_pos_ack = *emcmot_hal_data->update_pos_req;
    }
    else
    {   // block update_pos_ack to RISC when
        // ((emcmotStatus->motion_state == EMCMOT_MOTION_COORD)
        //   and there are pending TP commands)
        emcmotStatus->update_pos_ack = 0;
    }

    emcmotStatus->update_current_pos_flag = 0;  // prevent emcTaskPlanSynch() at emcTask.cc
    if (emcmotStatus->update_pos_ack != 0)
    {
        int joint_num;
        emcmot_joint_t *joint;
        double positions[EMCMOT_MAX_JOINTS];

        for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
            /* point to joint struct */
            joint = &joints[joint_num];
            /* copy risc_pos_cmd feedback */
            joint->pos_cmd = joint->risc_pos_cmd - joint->backlash_filt - joint->motor_offset - joint->blender_offset;
            joint->coarse_pos = joint->pos_cmd;
            joint->free_tp.curr_pos = joint->pos_cmd;
            joint->free_tp.pos_cmd = joint->pos_cmd;
            /* to reset cubic parameters */
            joint->cubic.needNextPoint=1;
            joint->cubic.filled=0;
            cubicAddPoint(&(joint->cubic), joint->coarse_pos);
            /* copy coarse command */
            positions[joint_num] = joint->coarse_pos;
//            rtapi_print (
//                    _("handle_special_cmd: j[%d] risc_pos_cmd(%f) pos_cmd(%f) pos_fb(%f) curr_pos(%f) motor_offset(%f)\n"),
//                    joint_num,
//                    joint->risc_pos_cmd,
//                    joint->pos_cmd,
//                    joint->pos_fb,
//                    joint->free_tp.curr_pos,
//                    joint->motor_offset);
        }

        /* if less than a full complement of joints, zero out the rest */
        while ( joint_num < EMCMOT_MAX_JOINTS ) {
            positions[joint_num] = 0.0;
            joint_num++;
        }

        /* update carte_pos_cmd for RISC-JOGGING */
        kinematicsForward(positions, &emcmotStatus->carte_pos_cmd, &fflags, &iflags);
        /* preset traj planner to current position */
        tpSetPos(&emcmotDebug->coord_tp, emcmotStatus->carte_pos_cmd); // for EMCMOT_MOTION_COORD mode
        emcmotStatus->update_current_pos_flag = 1; // force emcTaskPlanSynch() at emcTask.cc
    }

    // if spindle position get updated by spindle.comp
    if (emcmotStatus->spindle.update_pos_req != 0)
    {
        emcmot_joint_t *joint;

        /* point to joint struct */
        joint = &joints[emcmot_hal_data->spindle_joint_id];
        /* copy risc_pos_cmd feedback */
        joint->pos_cmd = emcmotStatus->spindle.curr_pos_cmd
                         - joint->backlash_filt
                         - joint->motor_offset
                         - joint->blender_offset;
        joint->coarse_pos = joint->pos_cmd;
        joint->free_tp.curr_pos = joint->pos_cmd;
        /* to reset cubic parameters */
        joint->cubic.needNextPoint=1;
        joint->cubic.filled=0;
        cubicAddPoint(&(joint->cubic), joint->coarse_pos);
        /* update carte_pos_cmd for RISC-JOGGING */
        emcmotStatus->carte_pos_cmd.s = joint->pos_cmd;
        emcmotDebug->coord_tp.currentPos.s = joint->pos_cmd;
    }
}

static void check_for_faults(void)
{
    int joint_num;
    emcmot_joint_t *joint;
    int neg_limit_override, pos_limit_override;

    /* check for various global fault conditions */
    /* only check enable input if running */
    if ( GET_MOTION_ENABLE_FLAG() != 0 ) {
        if ( *(emcmot_hal_data->enable) == 0 ) {
            reportError(_("motion stopped by enable input"));
            emcmotDebug->enabling = 0;
        }
    }

    if (( *(emcmot_hal_data->enable) == 0 ) ||
            (emcmotDebug->enabling == 0))
    {
        int n;
        /* turn-off all motion-synch-dout[] */
        for (n = 0; n < num_dio; n++)
        {
            *(emcmot_hal_data->synch_do[n]) = 0;
        }
    }

    /* check for various joint fault conditions */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint data */
        joint = &joints[joint_num];
        /* only check active, enabled axes */
        if ( GET_JOINT_ACTIVE_FLAG(joint) && GET_JOINT_ENABLE_FLAG(joint) ) {
            /* are any limits for this joint overridden? */
            neg_limit_override = emcmotStatus->overrideLimitMask & ( 1 << (joint_num*2));
            pos_limit_override = emcmotStatus->overrideLimitMask & ( 2 << (joint_num*2));
            /* check for hard limits */
            if ((GET_JOINT_PHL_FLAG(joint) && ! pos_limit_override ) ||
                    (GET_JOINT_NHL_FLAG(joint) && ! neg_limit_override )) {
                /* joint is on limit switch, should we trip? */
                if (GET_JOINT_HOMING_FLAG(joint)) {
                    /* no, ignore limits */
                } else {
                    /* trip on limits */
                    if (!GET_JOINT_ERROR_FLAG(joint)) {
                        /* report the error just this once */
                        reportError(_("joint %d on limit switch error"),
                                joint_num);
                        // override limit automatically.
                        emcmotStatus->overrideLimitMask |= ( 2 << (joint_num*2));
                        emcmotStatus->overrideLimitMask |= ( 1 << (joint_num*2));
                    }
                    SET_JOINT_ERROR_FLAG(joint, 1);
                    SET_MOTION_ERROR_FLAG(1);
                    // emcmotDebug->enabling = 0;
                }
            } else {
                // clean override mask after leave hard limits.
                if ((!GET_JOINT_PHL_FLAG(joint)) && (!GET_JOINT_NHL_FLAG(joint))) {
                    emcmotStatus->overrideLimitMask &= ~( 2 << (joint_num*2));
                    emcmotStatus->overrideLimitMask &= ~( 1 << (joint_num*2));
                }
            }

            // We don't want it to stop motion when PHL or NHL are toggled.
            // if (GET_JOINT_PHL_FLAG(joint) ||
            //     GET_JOINT_NHL_FLAG(joint)) {
            //     /* joint is on limit switch, should we trip? */
            //     SET_MOTION_ERROR_FLAG(1);
            // }

            /* check for amp fault */
            if (GET_JOINT_FAULT_FLAG(joint)) {
                /* joint is faulted, trip */
                if (!GET_JOINT_ERROR_FLAG(joint)) {
                    /* report the error just this once */
                    reportError(_("joint %d amplifier fault"), joint_num);
                }
                SET_JOINT_ERROR_FLAG(joint, 1);
                emcmotDebug->enabling = 0;
            }
            /* check for excessive following error */
            if (GET_JOINT_FERROR_FLAG(joint)) {
                if (!GET_JOINT_ERROR_FLAG(joint)) {
                    /* report the error just this once */
                    reportError(_("joint %d following error"), joint_num);
                }
                SET_JOINT_ERROR_FLAG(joint, 1);
                emcmotDebug->enabling = 0;

            }
            /* end of if JOINT_ACTIVE_FLAG(joint) */
        }
        /* end of check for joint faults loop */
    }
}

static void set_operating_mode(void)
{
    int joint_num, axis_num;
    emcmot_joint_t *joint;
    emcmot_axis_t *axis;
    double positions[EMCMOT_MAX_JOINTS];

    /* check for disabling */
    if (!emcmotDebug->enabling && GET_MOTION_ENABLE_FLAG()) {
        /* clear out the motion emcmotDebug->coord_tp and interpolators */
        tpClear(&emcmotDebug->coord_tp);
        for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
            /* point to joint data */
            joint = &joints[joint_num];
            /* disable free mode planner */
            joint->free_tp.enable = 0;
            joint->free_tp.curr_vel = 0.0;
            joint->free_tp.curr_acc = 0.0;
            /* drain coord mode interpolators */
            cubicDrain(&(joint->cubic));
            if (GET_JOINT_ACTIVE_FLAG(joint)) {
                SET_JOINT_ENABLE_FLAG(joint, 0);
                SET_JOINT_HOMING_FLAG(joint, 0);
                joint->home_state = HOME_IDLE;
            }
            /* don't clear the joint error flag, since that may signify why
               we just went into disabled state */
        }

        for (axis_num = 0; axis_num < EMCMOT_MAX_AXIS; axis_num++) {
            /* point to axis data */
            axis = &axes[axis_num];
            /* disable teleop mode planner */
            axis->teleop_tp.enable = 0;
            axis->teleop_tp.curr_vel = 0.0;
        }

        SET_MOTION_ENABLE_FLAG(0);
        /* don't clear the motion error flag, since that may signify why we
           just went into disabled state */
    }

    /* check for emcmotDebug->enabling */
    if (emcmotDebug->enabling && !GET_MOTION_ENABLE_FLAG()) {
        tpSetPos(&emcmotDebug->coord_tp, emcmotStatus->carte_pos_cmd);
        for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
            /* point to joint data */
            joint = &joints[joint_num];

            joint->free_tp.curr_pos = joint->pos_cmd;
            if (GET_JOINT_ACTIVE_FLAG(joint)) {
                SET_JOINT_ENABLE_FLAG(joint, 1);
                SET_JOINT_HOMING_FLAG(joint, 0);
                joint->home_state = HOME_IDLE;
            }
            /* clear any outstanding joint errors when going into enabled
               state */
            SET_JOINT_ERROR_FLAG(joint, 0);
        }
        SET_MOTION_ENABLE_FLAG(1);
        /* clear any outstanding motion errors when going into enabled state */
        SET_MOTION_ERROR_FLAG(0);
    }

    /* check for entering teleop mode */
    if (emcmotDebug->teleoperating && !GET_MOTION_TELEOP_FLAG()) {
        if (GET_MOTION_INPOS_FLAG()) {

            /* update coordinated emcmotDebug->coord_tp position */
            tpSetPos(&emcmotDebug->coord_tp, emcmotStatus->carte_pos_cmd);
            /* drain the cubics so they'll synch up */
            for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                /* point to joint data */
                joint = &joints[joint_num];
                cubicDrain(&(joint->cubic));
                positions[joint_num] = joint->coarse_pos;
            }
            /* Initialize things to do when starting teleop mode. */
            SET_MOTION_TELEOP_FLAG(1);
            SET_MOTION_ERROR_FLAG(0);

            kinematicsForward(positions, &emcmotStatus->carte_pos_cmd, &fflags, &iflags);

            (&axes[0])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.tran.x;
            (&axes[1])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.tran.y;
            (&axes[2])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.tran.z;
            (&axes[3])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.a;
            (&axes[4])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.b;
            (&axes[5])->teleop_tp.curr_pos = emcmotStatus->carte_pos_cmd.c;
        } else {
            /* not in position-- don't honor mode change */
            emcmotDebug->teleoperating = 0;
        }
    } else {
        if (GET_MOTION_INPOS_FLAG()) {
            if (!emcmotDebug->teleoperating && GET_MOTION_TELEOP_FLAG()) {
                SET_MOTION_TELEOP_FLAG(0);
                if (!emcmotDebug->coordinating) {
                    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                        /* point to joint data */
                        joint = &joints[joint_num];
                        /* update free planner positions */
                        joint->free_tp.curr_pos = joint->pos_cmd;
                    }
                }
            }
        }

        /* check for entering coordinated mode */
        if (emcmotDebug->coordinating && !GET_MOTION_COORD_FLAG()) {
            if (GET_MOTION_INPOS_FLAG()) {
                /* preset traj planner to current position */
                tpSetPos(&emcmotDebug->coord_tp, emcmotStatus->carte_pos_cmd);
                /* drain the cubics so they'll synch up */
                for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                    /* point to joint data */
                    joint = &joints[joint_num];
                    cubicDrain(&(joint->cubic));
                }
                /* clear the override limits flags */
                emcmotDebug->overriding = 0;
                emcmotStatus->overrideLimitMask = 0;
                SET_MOTION_COORD_FLAG(1);
                SET_MOTION_TELEOP_FLAG(0);
                SET_MOTION_ERROR_FLAG(0);
            } else {
                /* not in position-- don't honor mode change */
                emcmotDebug->coordinating = 0;
            }
        }

        /* check entering free space mode */
        if (!emcmotDebug->coordinating && GET_MOTION_COORD_FLAG()) {
            if (GET_MOTION_INPOS_FLAG()) {
                for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                    /* point to joint data */
                    joint = &joints[joint_num];
                    /* set joint planner curr_pos to current location */
                    joint->free_tp.curr_pos = joint->pos_cmd;
                    /* but it can stay disabled until a move is required */
                    joint->free_tp.enable = 0;
                }
                SET_MOTION_COORD_FLAG(0);
                SET_MOTION_TELEOP_FLAG(0);
                SET_MOTION_ERROR_FLAG(0);
            } else {
                /* not in position-- don't honor mode change */
                emcmotDebug->coordinating = 1;
            }
        }
    }
    /*! \todo FIXME - this code is temporary - eventually this function will be
       cleaned up and simplified, and 'motion_state' will become the master
       for this info, instead of having to gather it from several flags */
    if (!GET_MOTION_ENABLE_FLAG()) {
        emcmotStatus->motion_state = EMCMOT_MOTION_DISABLED;
    } else if (GET_MOTION_TELEOP_FLAG()) {
        emcmotStatus->motion_state = EMCMOT_MOTION_TELEOP;
    } else if (GET_MOTION_COORD_FLAG()) {
        emcmotStatus->motion_state = EMCMOT_MOTION_COORD;
    } else {
        emcmotStatus->motion_state = EMCMOT_MOTION_FREE;
    }
}

static void handle_jogwheels(void)
{
    int joint_num;
    emcmot_joint_t *joint;
    joint_hal_t *joint_data;
    int new_jog_counts, delta;
    double distance, pos;

    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint data */
        joint_data = &(emcmot_hal_data->joint[joint_num]);
        joint = &joints[joint_num];

        joint->wheel_jog_active = 0;
        if (!GET_JOINT_ACTIVE_FLAG(joint)) {
            /* if joint is not active, skip it */
            continue;
        }

        /* get counts from jogwheel */
        new_jog_counts = *(joint_data->jog_counts);
        delta = new_jog_counts - joint->old_jog_counts;
        /* save value for next time */
        joint->old_jog_counts = new_jog_counts;

        /* initialization complete */
        if ( first_pass ) {
            continue;
        }

        /* must be in free mode and enabled */
        if (GET_MOTION_COORD_FLAG()) {
            continue;
        }

        if (!GET_MOTION_ENABLE_FLAG()) {
            continue;
        }

        /* the jogwheel input for this joint must be enabled */
        if ( *(joint_data->jog_enable) == 0 ) {
            continue;
        }

        /* must not be homing */
        if (emcmotStatus->homing_active) {
            continue;
        }

        /* must not be doing a keyboard jog */
        if (joint->kb_jog_active) {
            continue;
        }

        if (emcmotStatus->net_feed_scale < 0.0001 ) {
            /* don't jog if feedhold is on or if feed override is zero */
            break;
        }

        /* MPG scale switch at x1 mode -- accurate positioning mode */
        if (*(emcmot_hal_data->mpg_scale_x1) == 1 ||
                *(emcmot_hal_data->mpg_scale_x10) == 1 ||
                *(emcmot_hal_data->mpg_scale_x100) == 1)
        {
            /* calculate distance to jog */
            distance = (double) delta * (*(joint_data->jog_scale));
            if (*(emcmot_hal_data->mpg_scale_x10) == 1){
                distance = (double) delta * (*(joint_data->jog_scale) *10);
            }
            else if (*(emcmot_hal_data->mpg_scale_x100) == 1){
                distance = (double) delta * (*(joint_data->jog_scale) *100);
            }
            /* check for joint already on hard limit */
            if (distance > 0.0 && GET_JOINT_PHL_FLAG(joint)) {
                continue;
            }
            if (distance < 0.0 && GET_JOINT_NHL_FLAG(joint)) {
                continue;
            }
            /* calc target position for jog */
            pos = joint->free_tp.pos_cmd + distance;
            /* don't jog past limits */
            refresh_jog_limits(joint);
            if (pos > joint->max_jog_limit) {
                continue;
            }
            if (pos < joint->min_jog_limit) {
                continue;
            }
            /* set target position */
            joint->free_tp.pos_cmd = pos;
            /* and let it go */
            joint->free_tp.enable = 1;
        } // end: MPG scale switch at x1 (accurate positioning mode)

        /* lock out other jog sources */
        joint->wheel_jog_active = 1;

        SET_JOINT_ERROR_FLAG(joint, 0);
//        /* clear joint homed flag(s) if we don't have forward kins.
//           Otherwise, a transition into coordinated mode will incorrectly
//           assume the homed position. Do all if they've all been moved
//           since homing, otherwise just do this one */
//        clearHomes(joint_num);
    }
}


static double calc_accel(double acc_limit, double acc_desired)
{
    double accel_mag;

    if (emcmotStatus->acc > acc_limit)
    {
        accel_mag = acc_limit;
    } else
    {
        accel_mag = emcmotStatus->acc;
    }

    if (fabs(acc_desired) > accel_mag)
    {
        accel_mag = accel_mag * acc_desired / fabs(acc_desired);
    } else
    {
        accel_mag = acc_desired;
    }

    return (accel_mag);
}

static double calc_vel(double vel_limit, double cur_vel, double cur_accel)
{
    cur_vel += cur_accel * servo_period;
    if (cur_vel > vel_limit)
    {
        cur_vel = vel_limit;
    } else if (cur_vel < -vel_limit)
    {
        cur_vel = -vel_limit;
    }
    return (cur_vel);
}

static void get_spindle_cmds (double cycle_time)
{
    if(emcmotStatus->spindle.css_factor && emcmotStatus->spindle.on)
    {
        // for G96
        double denom = emcmotStatus->spindle.xoffset - emcmotStatus->carte_pos_cmd.tran.x;
        double speed;       // speed for major spindle (spindle-s)
        double maxpositive;

        // css_factor: unit(mm or inch)/min
        if(emcmotStatus->spindle.dynamic_speed_mode == 0)
        {
            if(denom != 0)
            {
                speed = emcmotStatus->spindle.css_factor / (denom * 60.0); // rps
            }
            else
            {
                speed = emcmotStatus->spindle.speed_rps;
            }
        }
        else
        {
            // dynamic_spindle_mode == 1
            // adjust spindle speed dynamically to maintain constant tangential velocity
            // the spindle speed is constant if (r <= const_speed_radius)
            double csr;
            csr = emcmotStatus->spindle.const_speed_radius;

            if ((fabs(denom) >= csr) && (csr > 0))
            {
                speed = (emcmotStatus->spindle.speed_rps * csr) / denom; // rps
            }
            else
            {
                speed = emcmotStatus->spindle.speed_rps;
            }
        }
        speed = fabs(speed);
        maxpositive = fabs(emcmotStatus->spindle.speed_rps);
        if(speed > maxpositive) speed = maxpositive;
        emcmotStatus->spindle.speed_req_rps = speed * emcmotStatus->spindle.direction;
        emcmotStatus->spindle.css_error =
                        (emcmotStatus->spindle.css_factor / 60.0
                         - denom * fabs(emcmotStatus->spindle.curr_vel_rps))
                        * emcmotStatus->spindle.direction; // (unit/(2*PI*sec)
        DP ("css_req(%f)(unit/sec)\n", denom * emcmotStatus->spindle.speed_req_rps * 2 * M_PI);
        DP ("css_cur(%f)\n", denom * emcmotStatus->spindle.curr_vel_rps * 2 * M_PI);
        DP ("css_error(%f)(unit/(2*PI*sec))\n", emcmotStatus->spindle.css_error);
        DP ("speed(%f) denom(%f) s.curr_vel(%f) css_factor(%f)\n",
                             speed, denom, emcmotStatus->spindle.curr_vel_rps, emcmotStatus->spindle.css_factor);
        DP ("spindle.direction(%d)\n",
                             emcmotStatus->spindle.direction);
//        DP ("synched-joint-vel(%f)(unit/sec)\n",emcmotStatus->spindle.curr_vel_rps * tp->uu_per_rev);
#if (CSS_TRACE!=0)
        /* prepare data for gnuplot */
        fprintf (csstrace, "%11d%15.9f%15.9f%19.9f%19.9f%19.9f\n"
                , _dt
                , denom
                , emcmotStatus->progress
                , emcmotStatus->spindle.css_factor / 60.0 * 2 * M_PI
                , denom * emcmotStatus->spindle.curr_vel_rps * 2 * M_PI
                , (emcmotStatus->spindle.css_factor / 60.0 * 2 * M_PI - denom * emcmotStatus->spindle.curr_vel_rps * 2 * M_PI));
        _dt += 1;
#endif
    }
    else
    {
        // G97, G33 w/ G97 or G33.1
        emcmotStatus->spindle.speed_req_rps = emcmotStatus->spindle.speed_rps;
        emcmotStatus->spindle.css_error = 0;
    }
}

static void get_pos_cmds(long period)
{
    int joint_num, result;
    emcmot_joint_t *joint;
    //obsolete: emcmot_axis_t *axis;
    double positions[EMCMOT_MAX_JOINTS]/*, tmp_pos[EMCMOT_MAX_JOINTS], tmp_vel[EMCMOT_MAX_JOINTS]*/;
    double old_pos_cmd;
    int onlimit = 0;
    int joint_limit[EMCMOT_MAX_JOINTS][2];
    int num_joints;

    num_joints = emcmotConfig->numJoints;

    /* copy joint position feedback to local array */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint struct */
        joint = &joints[joint_num];
        /* copy coarse command */
        positions[joint_num] = joint->coarse_pos;
    }
    /* if less than a full complement of joints, zero out the rest */
    while ( joint_num < EMCMOT_MAX_JOINTS ) {
        positions[joint_num] = 0.0;
        joint_num++;
    }

    /* RUN MOTION CALCULATIONS: */

    /* run traj planner code depending on the state */
    switch ( emcmotStatus->motion_state) {
    case EMCMOT_MOTION_FREE:
        rtapi_print_msg(RTAPI_MSG_DBG,"EMCMOT_MOTION_FREE ++++\n");
        /* in free mode, each joint is planned independently */
        /* initial value for flag, if needed it will be cleared below */
        SET_MOTION_INPOS_FLAG(1);
        for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
            /* point to joint struct */
            joint = &joints[joint_num];
            if (!GET_JOINT_ACTIVE_FLAG(joint)) {
                /* if joint is not active, skip it */
                continue;
            }

            // TODO: distinguish LINEAR and ANGULAR vel/acc/jerk
            if(joint->acc_limit > emcmotStatus->acc)
                joint->acc_limit = emcmotStatus->acc;
            if(joint->jerk_limit > emcmotStatus->jerk)
                joint->jerk_limit = emcmotStatus->jerk;
            /* compute joint velocity limit */
            if ( joint->home_state == HOME_IDLE ) {
                /* velocity limit = joint limit * global scale factor */
                /* the global factor is used for feedrate override */

                // joints_axes3: vel_lim = joint->vel_limit * emcmotStatus->net_feed_scale;
                /**
                 * 2010-05-14 ysli:
                 * the joint->free_tp.max_vel was set at command.c::EMCMOT_JOG_*,
                 * which was set by the "Jog Speed" of AXIS_GUI
                 **/
                joint->free_tp.max_vel = fabs(emcmotCommand->vel) * emcmotStatus->net_feed_scale;
                /* must not be greater than the joint physical limit */
                if (joint->free_tp.max_vel > joint->vel_limit) {
                    joint->free_tp.max_vel = joint->vel_limit;
                }
                /* set vel limit in free TP */
            } else {
                /* except if homing, when we set free_tp max vel in do_homing */
            }

            /* set acc limit in free TP */
            joint->free_tp.max_acc = joint->acc_limit;
            /* execute free TP */
            if (joint->disable_jog == 0) {
                /* do not allow jog over hard limit */
                if ((((joint->free_tp.pos_cmd - joint->free_tp.curr_pos) >= 0) &&
                        GET_JOINT_PHL_FLAG(joint)) ||
                        (((joint->free_tp.pos_cmd - joint->free_tp.curr_pos) < 0) &&
                                GET_JOINT_NHL_FLAG(joint))) {
                    joint->free_tp.max_vel = 0;
                }

                simple_tp_update(&(joint->free_tp), servo_period );
            }
            /* copy free TP output to pos_cmd and coarse_pos */
            joint->pos_cmd = joint->free_tp.curr_pos;
            joint->vel_cmd = joint->free_tp.curr_vel;
            joint->coarse_pos = joint->free_tp.curr_pos;
            /* update joint status flag and overall status flag */
            if ( joint->free_tp.active ) {
                /* active TP means we're moving, so not in position */
                SET_JOINT_INPOS_FLAG(joint, 0);
                SET_MOTION_INPOS_FLAG(0);
                /* if we move at all, clear AT_HOME flag */
                SET_JOINT_AT_HOME_FLAG(joint, 0);
                /* is any limit disabled for this move? */
                if ( emcmotStatus->overrideLimitMask ) {
                    emcmotDebug->overriding = 1;
                }
            } else {
                SET_JOINT_INPOS_FLAG(joint, 1);
                /* joint has stopped, so any outstanding jogs are done */
                joint->kb_jog_active = 0;
            }
        }//for loop for joints
        /* if overriding is true and we're in position, the jog
                   is complete, and the limits should be re-enabled */
        if ( (emcmotDebug->overriding ) && ( GET_MOTION_INPOS_FLAG() ) ) {
            emcmotStatus->overrideLimitMask = 0;
            emcmotDebug->overriding = 0;
        }
        /*! \todo FIXME - this should run at the traj rate */
        switch (emcmotConfig->kinType) {

        case KINEMATICS_IDENTITY:
            rtapi_print_msg(RTAPI_MSG_DBG,"KINEMATICS_IDENTITY ++++\n");
            kinematicsForward(positions, &emcmotStatus->carte_pos_cmd, &fflags, &iflags);
            if (checkAllHomed()) {
                emcmotStatus->carte_pos_cmd_ok = 1;
            } else {
                emcmotStatus->carte_pos_cmd_ok = 0;
            }
            break;

        case KINEMATICS_BOTH:
            rtapi_print_msg(RTAPI_MSG_DBG,"KINEMATICS_BOTH ++++\n");
            if (checkAllHomed()) {
                /* is previous value suitable for use as initial guess? */
                if (!emcmotStatus->carte_pos_cmd_ok) {
                    /* no, use home position as initial guess */
                    emcmotStatus->carte_pos_cmd = emcmotStatus->world_home;
                }
                /* calculate Cartesean position command from joint coarse pos cmd */
                result =
                        kinematicsForward(positions, &emcmotStatus->carte_pos_cmd, &fflags, &iflags);
                /* check to make sure kinematics converged */
                if (result < 0) {
                    /* error during kinematics calculations */
                    emcmotStatus->carte_pos_cmd_ok = 0;
                } else {
                    /* it worked! */
                    emcmotStatus->carte_pos_cmd_ok = 1;
                }
            } else {
                emcmotStatus->carte_pos_cmd_ok = 0;
            }
            break;

        case KINEMATICS_INVERSE_ONLY:
            rtapi_print_msg(RTAPI_MSG_DBG,"KINEMATICS_INVERSE_ONLY ++++\n");
            emcmotStatus->carte_pos_cmd_ok = 0;
            break;

        default:
            emcmotStatus->carte_pos_cmd_ok = 0;
            break;
        }
        /* end of FREE mode */
        break;

        case EMCMOT_MOTION_COORD:
            rtapi_print_msg(RTAPI_MSG_DBG,"EMCMOT_MOTION_COORD ++++\n");

            /* check joint 0 to see if the interpolators are empty */
            while (cubicNeedNextPoint(&(joints[0].cubic))) {
                /* they're empty, pull next point(s) off Cartesian planner */
                /* run coordinated trajectory planning cycle */
                tpRunCycle(&emcmotDebug->coord_tp, period);
                /* gt new commanded traj pos */
                emcmotStatus->carte_pos_cmd = tpGetPos(&emcmotDebug->coord_tp);
                /* OUTPUT KINEMATICS - convert to joints in local array */
                kinematicsInverse(&emcmotStatus->carte_pos_cmd, positions,
                        &iflags, &fflags);
                /* copy to joint structures and spline them up */
                DPS("%11u", _dt);
                DPS("x(%10.5f)%10.5f%10.5f%10.5fs(%10.5f)",
                        emcmotStatus->carte_pos_cmd.tran.x,
                        emcmotStatus->carte_pos_cmd.tran.y,
                        emcmotStatus->carte_pos_cmd.tran.z,
                        emcmotStatus->carte_pos_cmd.a,
                        emcmotStatus->carte_pos_cmd.s);
                for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                    /* point to joint struct */
                    joint = &joints[joint_num];
                    joint->coarse_pos = positions[joint_num];

                    /* spline joints up-- note that we may be adding points
                           that fail soft limits, but we'll abort at the end of
                           this cycle so it doesn't really matter */
                    if (emcmotStatus->usb_cmd == USB_CMD_WOU_CMD_SYNC) { // drain cubic so that assigned position won't be changed
                        cubicDrain(&(joint->cubic));
                    }
                    cubicAddPoint(&(joint->cubic), joint->coarse_pos);
                }
                /* END OF OUTPUT KINS */
            }
            /* there is data in the interpolators */
            /* run interpolation */
            for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                /* point to joint struct */
                joint = &joints[joint_num];
                /* save old command */
                old_pos_cmd = joint->pos_cmd;
                /* interpolate to get new one */
                joint->pos_cmd = cubicInterpolate(&(joint->cubic), 0, 0, 0, 0);
                joint->vel_cmd = (joint->pos_cmd - old_pos_cmd) * servo_freq;
                DPS ("%17.7f", joint->pos_cmd);
            }
            DPS("\n");
            /* report motion status */
            SET_MOTION_INPOS_FLAG(0);
            if (tpIsDone(&emcmotDebug->coord_tp)) {
                SET_MOTION_INPOS_FLAG(1);
            }
            break;

        case EMCMOT_MOTION_TELEOP:
            /* first the desired Accell's are computed based on
                   desired Velocity, current velocity and period */
            emcmotDebug->teleop_data.desiredAccell.tran.x =
                    (emcmotDebug->teleop_data.desiredVel.tran.x -
                            emcmotDebug->teleop_data.currentVel.tran.x) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.tran.x = calc_accel(axes[0].acc_limit, emcmotDebug->teleop_data.desiredAccell.tran.x);
            emcmotDebug->teleop_data.currentVel.tran.x = calc_vel(axes[0].vel_limit,
                                                                  emcmotDebug->teleop_data.currentVel.tran.x,
                                                                  emcmotDebug->teleop_data.currentAccell.tran.x);

            emcmotDebug->teleop_data.desiredAccell.tran.y =
                    (emcmotDebug->teleop_data.desiredVel.tran.y -
                            emcmotDebug->teleop_data.currentVel.tran.y) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.tran.y = calc_accel(axes[1].acc_limit, emcmotDebug->teleop_data.desiredAccell.tran.y);
            emcmotDebug->teleop_data.currentVel.tran.y = calc_vel(axes[1].vel_limit,
                                                                  emcmotDebug->teleop_data.currentVel.tran.y,
                                                                  emcmotDebug->teleop_data.currentAccell.tran.y);

            emcmotDebug->teleop_data.desiredAccell.tran.z =
                    (emcmotDebug->teleop_data.desiredVel.tran.z -
                            emcmotDebug->teleop_data.currentVel.tran.z) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.tran.z = calc_accel(axes[2].acc_limit, emcmotDebug->teleop_data.desiredAccell.tran.z);
            emcmotDebug->teleop_data.currentVel.tran.z = calc_vel(axes[2].vel_limit,
                                                                  emcmotDebug->teleop_data.currentVel.tran.z,
                                                                  emcmotDebug->teleop_data.currentAccell.tran.z);

            /* then the accells for the rotary axes */
            emcmotDebug->teleop_data.desiredAccell.a =
                    (emcmotDebug->teleop_data.desiredVel.a -
                            emcmotDebug->teleop_data.currentVel.a) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.a = calc_accel(axes[3].acc_limit, emcmotDebug->teleop_data.desiredAccell.a);
            emcmotDebug->teleop_data.currentVel.a = calc_vel(axes[3].vel_limit,
                                                             emcmotDebug->teleop_data.currentVel.a,
                                                             emcmotDebug->teleop_data.currentAccell.a);

            emcmotDebug->teleop_data.desiredAccell.b =
                    (emcmotDebug->teleop_data.desiredVel.b -
                            emcmotDebug->teleop_data.currentVel.b) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.b = calc_accel(axes[4].acc_limit, emcmotDebug->teleop_data.desiredAccell.b);
            emcmotDebug->teleop_data.currentVel.b = calc_vel(axes[4].vel_limit,
                                                             emcmotDebug->teleop_data.currentVel.b,
                                                             emcmotDebug->teleop_data.currentAccell.b);

            emcmotDebug->teleop_data.desiredAccell.c =
                    (emcmotDebug->teleop_data.desiredVel.c -
                            emcmotDebug->teleop_data.currentVel.c) /
                            servo_period;
            emcmotDebug->teleop_data.currentAccell.c = calc_accel(axes[5].acc_limit, emcmotDebug->teleop_data.desiredAccell.c);
            emcmotDebug->teleop_data.currentVel.c = calc_vel(axes[5].vel_limit,
                                                             emcmotDebug->teleop_data.currentVel.c,
                                                             emcmotDebug->teleop_data.currentAccell.c);

            /* based on curent position, current vel and period,
                   the next position is computed */
            emcmotStatus->carte_pos_cmd.tran.x +=
                    emcmotDebug->teleop_data.currentVel.tran.x * servo_period;
            emcmotStatus->carte_pos_cmd.tran.y +=
                    emcmotDebug->teleop_data.currentVel.tran.y * servo_period;
            emcmotStatus->carte_pos_cmd.tran.z +=
                    emcmotDebug->teleop_data.currentVel.tran.z * servo_period;
            emcmotStatus->carte_pos_cmd.a +=
                    emcmotDebug->teleop_data.currentVel.a * servo_period;
            emcmotStatus->carte_pos_cmd.b +=
                    emcmotDebug->teleop_data.currentVel.b * servo_period;
            emcmotStatus->carte_pos_cmd.c +=
                    emcmotDebug->teleop_data.currentVel.c *
                    servo_period;

            DP("EMCMOT_MOTION_TELEOP: emcmotStatus->carte_pos_cmd:\n");
            DPS("x.pos_cmd(%f) x.cur_vel(%f) x.desired_vel(%f) x.desired_acc(%f)\n",
                    emcmotStatus->carte_pos_cmd.tran.x,
                    emcmotDebug->teleop_data.currentVel.tran.x,
                    emcmotDebug->teleop_data.desiredVel.tran.x,
                    emcmotDebug->teleop_data.desiredAccell.tran.x
            );
            DPS("y.pos_cmd(%f) y.cur_vel(%f) y.desired_vel(%f) y.desired_acc(%f)\n",
                    emcmotStatus->carte_pos_cmd.tran.y,
                    emcmotDebug->teleop_data.currentVel.tran.y,
                    emcmotDebug->teleop_data.desiredVel.tran.y,
                    emcmotDebug->teleop_data.desiredAccell.tran.y
            );

            DPS("z.pos_cmd(%f) z.cur_vel(%f) z.desired_vel(%f) z.desired_acc(%f)\n",
                    emcmotStatus->carte_pos_cmd.tran.z,
                    emcmotDebug->teleop_data.currentVel.tran.z,
                    emcmotDebug->teleop_data.desiredVel.tran.z,
                    emcmotDebug->teleop_data.desiredAccell.tran.z
            );

            /* the next position then gets run through the inverse kins,
                   to compute the next positions of the joints */

            /* OUTPUT KINEMATICS - convert to joints in local array */
            kinematicsInverse(&emcmotStatus->carte_pos_cmd, positions,
                    &iflags, &fflags);
            /* copy to joint structures and spline them up */
            for (joint_num = 0; joint_num < num_joints; joint_num++) {
                /* point to joint struct */
                joint = &joints[joint_num];
                joint->coarse_pos = positions[joint_num];
                /* spline joints up-- note that we may be adding points
                       that fail soft limits, but we'll abort at the end of
                       this cycle so it doesn't really matter */
                cubicAddPoint(&(joint->cubic), joint->coarse_pos);
            }
            /* END OF OUTPUT KINS */

            /* there is data in the interpolators */
            /* run interpolation */
            for (joint_num = 0; joint_num < num_joints; joint_num++) {
                /* point to joint struct */
                joint = &joints[joint_num];
                /* save old command */
                old_pos_cmd = joint->pos_cmd;
                /* interpolate to get new one */
                joint->pos_cmd = cubicInterpolate(&(joint->cubic), 0, 0, 0, 0);
                joint->vel_cmd = (joint->pos_cmd - old_pos_cmd) * servo_freq;
            }

            /* end of teleop mode */
            break;

        case EMCMOT_MOTION_DISABLED:
            rtapi_print_msg(RTAPI_MSG_DBG,"EMCMOT_MOTION_DISABLED ++++\n");
            /* set position commands to match feedbacks, this avoids
                   disturbances and/or following errors when enabling */
            emcmotStatus->carte_pos_cmd = emcmotStatus->carte_pos_fb;
            for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                /* point to joint struct */
                joint = &joints[joint_num];
                /* save old command */
                joint->pos_cmd = joint->pos_fb;
                /* set joint velocity to zero */
                joint->vel_cmd = 0.0;
            }

            break;
        default:
            break;
    }


    /* check command against soft limits */
    /* This is a backup check, it should be impossible to command
        a move outside the soft limits.  However there is at least
        two cases that isn't caught upstream:
        1) if an arc has both endpoints inside the limits, but the curve extends outside,
        2) if homing params are wrong then after homing joint pos_cmd are outside,
        the upstream checks will pass it.
     */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint data */
        joint = &joints[joint_num];
        /* skip inactive or unhomed axes */
        if ((!GET_JOINT_ACTIVE_FLAG(joint)) || (!GET_JOINT_HOMED_FLAG(joint))) {
            continue;
        }

        /* check for soft limits */
        joint_limit[joint_num][1] = 0;
        joint_limit[joint_num][0] = 0;
        if (joint->pos_cmd > joint->max_pos_limit) {
            joint_limit[joint_num][1] = 1;
            onlimit = 1;
            if (! emcmotStatus->on_soft_limit) {
                reportError(_("joint[%d]: pos_cmd(%f) max_pos_limit(%f)\n"), 
                        joint_num, joint->pos_cmd, joint->max_pos_limit);
            }
        }
        else if (joint->pos_cmd < joint->min_pos_limit) {
            joint_limit[joint_num][0] = 1;
            onlimit = 1;
            if (! emcmotStatus->on_soft_limit) {
                reportError(_("joint[%d]: pos_cmd(%f) min_pos_limit(%f)\n"), 
                        joint_num, joint->pos_cmd, joint->min_pos_limit);
            }
        }
    }
    if ( onlimit ) {
        if ( ! emcmotStatus->on_soft_limit ) {
            /* just hit the limit */
            for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
                if (joint_limit[joint_num][0]) {
                    reportError(_("Exceeded negative soft limit on joint %d"), joint_num);
                } else if (joint_limit[joint_num][1]) {
                    reportError(_("Exceeded positive soft limit on joint %d"), joint_num);
                }
            }
            SET_MOTION_ERROR_FLAG(1);
            emcmotStatus->on_soft_limit = 1;
        }
        onlimit = 0;
    } else {
        emcmotStatus->on_soft_limit = 0;
    }
}

/* NOTES:  These notes are just my understanding of how things work.

There are seven sets of position information.

1) emcmotStatus->carte_pos_cmd
2) emcmotStatus->joints[n].coarse_pos
3) emcmotStatus->joints[n].pos_cmd
4) emcmotStatus->joints[n].motor_pos_cmd
5) emcmotStatus->joints[n].motor_pos_fb
6) emcmotStatus->joints[n].pos_fb
7) emcmotStatus->carte_pos_fb

Their exact contents and meaning are as follows:

1) This is the desired position, in Cartesean coordinates.  It is
   updated at the traj rate, not the servo rate.
   In coord mode, it is determined by the traj planner
   In teleop mode, it is determined by the traj planner?
   In free mode, it is not used, since free mode motion takes
     place in joint space, not cartesean space.  It may be
     displayed by the GUI however, so it is updated by
     applying forward kins to (2), unless forward kins are
     not available, in which case it is copied from (7).

2) This is the desired position, in joint coordinates, but
   before interpolation.  It is updated at the traj rate, not
   the servo rate..
   In coord mode, it is generated by applying inverse kins to (1)
   In teleop mode, it is generated by applying inverse kins to (1)
   In free mode, it is not used, since the free mode planner generates
     a new (3) position every servo period without interpolation.
     However, it is used indirectly by GUIs, so it is copied from (3).

3) This is the desired position, in joint coords, after interpolation.
   A new set of these coords is generated every servo period.
   In coord mode, it is generated from (2) by the interpolator.
   In teleop mode, it is generated from (2) by the interpolator.
   In free mode, it is generated by the simple free mode traj planner.

4) This is the desired position, in motor coords.  Motor coords are
   generated by adding backlash compensation, lead screw error
   compensation, and offset (for homing) to (3).
   It is generated the same way regardless of the mode, and is the
   output to the PID loop or other position loop.

5) This is the actual position, in motor coords.  It is the input from
   encoders or other feedback device (or from virtual encoders on open
   loop machines).  It is "generated" by reading the feedback device.

6) This is the actual position, in joint coordinates.  It is generated
   by subtracting offset, lead screw error compensation, and backlash
   compensation from (5).  It is generated the same way regardless of
   the operating mode.

7) This is the actual position, in Cartesean coordinates.  It is updated
   at the traj rate, not the servo rate.
   OLD VERSION:
   In the old version, there are four sets of code to generate actualPos.
   One for each operating mode, and one for when motion is disabled.
   The code for coord and teleop modes is identical.  The code for free
   mode is somewhat different, in particular to deal with the case where
   one or more axes are not homed.  The disabled code is quite similar,
   but not identical, to the coord mode code.  In general, the code
   calculates actualPos by applying the forward kins to (6).  However,
   where forward kins are not available, actualPos is either copied
   from (1) (assumes no following error), or simply left alone.
   These special cases are handled differently for each operating mode.
   NEW VERSION:
   I would like to both simplify and relocate this.  As far as I can
   tell, actualPos should _always_ be the best estimate of the actual
   machine position in Cartesean coordinates.  So it should always be
   calculated the same way.
   In addition to always using the same code to calculate actualPos,
   I want to move that code.  It is really a feedback calculation, and
   as such it belongs with the rest of the feedback calculations early
   in control.c, not as part of the output generation code as it is now.
   Ideally, actualPos would always be calculated by applying forward
   kinematics to (6).  However, forward kinematics may not be available,
   or they may be unusable because one or more axes aren't homed.  In
   that case, the options are: A) fake it by copying (1), or B) admit
   that we don't really know the Cartesean coordinates, and simply
   don't update actualPos.  Whatever approach is used, I can see no
   reason not to do it the same way regardless of the operating mode.
   I would propose the following:  If there are forward kins, use them,
   unless they don't work because of unhomed axes or other problems,
   in which case do (B).  If no forward kins, do (A), since otherwise
   actualPos would _never_ get updated.

 */

static void compute_screw_comp(void)
{
    int joint_num;
    emcmot_joint_t *joint;
    emcmot_comp_t *comp;
    double dpos;
    double a_max, v_max, v, s_to_go, ds_stop, ds_vel, ds_acc, dv_acc;


    /* compute the correction */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint struct */
        joint = &joints[joint_num];
        if (!GET_JOINT_ACTIVE_FLAG(joint)) {
            /* if joint is not active, skip it */
            continue;
        }
        /* point to compensation data */
        comp = &(joint->comp);
        if ( comp->entries > 0 ) {
            /* there is data in the comp table, use it */
            /* first make sure we're in the right spot in the table */
            while ( joint->pos_cmd < comp->entry->nominal ) {
                comp->entry--;
            }
            while ( joint->pos_cmd >= (comp->entry+1)->nominal ) {
                comp->entry++;
            }
            /* now interpolate */
            dpos = joint->pos_cmd - comp->entry->nominal;
            if (joint->vel_cmd > 0.0) {
                /* moving "up". apply forward screw comp */
                joint->backlash_corr = comp->entry->fwd_trim +
                        comp->entry->fwd_slope * dpos;
            } else if (joint->vel_cmd < 0.0) {
                /* moving "down". apply reverse screw comp */
                joint->backlash_corr = comp->entry->rev_trim +
                        comp->entry->rev_slope * dpos;
            } else {
                /* not moving, use whatever was there before */
            }
        } else {
            /* no compensation data, just use +/- 1/2 of backlash */
            /** FIXME: this can actually be removed - if the user space code
                sends a single compensation entry with any nominal value,
                and with fwd_trim = +0.5 times the backlash value, and
                rev_trim = -0.5 times backlash, the above screw comp code
                will give exactly the same result as this code. */
            /* determine which way the compensation should be applied */
            if (joint->vel_cmd > 0.0) {
                /* moving "up". apply positive backlash comp */
                joint->backlash_corr = 0.5 * joint->backlash;
            } else if (joint->vel_cmd < 0.0) {
                /* moving "down". apply negative backlash comp */
                joint->backlash_corr = -0.5 * joint->backlash;
            } else {
                /* not moving, use whatever was there before */
            }
        }
        /* at this point, the correction has been computed, but
           the value may make abrupt jumps on direction reversal */
        /*
         * 07/09/2005 - S-curve implementation by Bas Laarhoven
         *
         * Implementation:
         *   Generate a ramped velocity profile for backlash or screw error comp.
         *   The velocity is ramped up to the maximum speed setting (if possible),
         *   using the maximum acceleration setting.
         *   At the end, the speed is ramped dowm using the same acceleration.
         *   The algorithm keeps looking ahead. Depending on the distance to go,
         *   the speed is increased, kept constant or decreased.
         *
         * Limitations:
         *   Since the compensation adds up to the normal movement, total
         *   accelleration and total velocity may exceed maximum settings!
         *   Currently this is limited to 150% by implementation.
         *   To fix this, the calculations in get_pos_cmd should include
         *   information from the backlash corection. This makes things
         *   rather complicated and it might be better to implement the
         *   backlash compensation at another place to prevent this kind
         *   of interaction.
         *   More testing under different circumstances will show if this
         *   needs a more complicate solution.
         *   For now this implementation seems to generate smoother
         *   movements and less following errors than the original code.
         */

        /* Limit maximum accelleration and velocity 'overshoot'
         * to 150% of the maximum settings.
         * The TP and backlash shouldn't use more than 100%
         * (together) but this requires some interaction that
         * isn't implemented yet.
         */
        v_max = 0.5 * joint->vel_limit * emcmotStatus->net_feed_scale;
        a_max = 0.5 * joint->acc_limit;
        v = joint->backlash_vel;
        if (joint->backlash_corr >= joint->backlash_filt) {
            s_to_go = joint->backlash_corr - joint->backlash_filt; /* abs val */
            if (s_to_go > 0) {
                // off target, need to move
                ds_vel  = v * servo_period;           /* abs val */
                dv_acc  = a_max * servo_period;       /* abs val */
                ds_stop = 0.5 * (v + dv_acc) *
                        (v + dv_acc) / a_max; /* abs val */
                if (s_to_go <= ds_stop + ds_vel) {
                    // ramp down
                    if (v > dv_acc) {
                        // decellerate one period
                        ds_acc = 0.5 * dv_acc * servo_period; /* abs val */
                        joint->backlash_vel  -= dv_acc;
                        joint->backlash_filt += ds_vel - ds_acc;
                    } else {
                        // last step to target
                        joint->backlash_vel  = 0.0;
                        joint->backlash_filt = joint->backlash_corr;
                    }
                } else {
                    if (v + dv_acc > v_max) {
                        dv_acc = v_max - v;                /* abs val */
                    }
                    ds_acc  = 0.5 * dv_acc * servo_period; /* abs val */
                    ds_stop = 0.5 * (v + dv_acc) *
                            (v + dv_acc) / a_max;  /* abs val */
                    if (s_to_go > ds_stop + ds_vel + ds_acc) {
                        // ramp up
                        joint->backlash_vel  += dv_acc;
                        joint->backlash_filt += ds_vel + ds_acc;
                    } else {
                        // constant velocity
                        joint->backlash_filt += ds_vel;
                    }
                }
            } else if (s_to_go < 0) {
                // safely handle overshoot (should not occur)
                joint->backlash_vel = 0.0;
                joint->backlash_filt = joint->backlash_corr;
            }
        } else {  /* joint->backlash_corr < 0.0 */
            s_to_go = joint->backlash_filt - joint->backlash_corr; /* abs val */
            if (s_to_go > 0) {
                // off target, need to move
                ds_vel  = -v * servo_period;          /* abs val */
                dv_acc  = a_max * servo_period;       /* abs val */
                ds_stop = 0.5 * (v - dv_acc) *
                        (v - dv_acc) / a_max; /* abs val */
                if (s_to_go <= ds_stop + ds_vel) {
                    // ramp down
                    if (-v > dv_acc) {
                        // decellerate one period
                        ds_acc = 0.5 * dv_acc * servo_period; /* abs val */
                        joint->backlash_vel  += dv_acc;   /* decrease */
                        joint->backlash_filt -= ds_vel - ds_acc;
                    } else {
                        // last step to target
                        joint->backlash_vel = 0.0;
                        joint->backlash_filt = joint->backlash_corr;
                    }
                } else {
                    if (-v + dv_acc > v_max) {
                        dv_acc = v_max + v;               /* abs val */
                    }
                    ds_acc = 0.5 * dv_acc * servo_period; /* abs val */
                    ds_stop = 0.5 * (v - dv_acc) *
                            (v - dv_acc) / a_max; /* abs val */
                    if (s_to_go > ds_stop + ds_vel + ds_acc) {
                        // ramp up
                        joint->backlash_vel  -= dv_acc;   /* increase */
                        joint->backlash_filt -= ds_vel + ds_acc;
                    } else {
                        // constant velocity
                        joint->backlash_filt -= ds_vel;
                    }
                }
            } else if (s_to_go < 0) {
                // safely handle overshoot (should not occur)
                joint->backlash_vel = 0.0;
                joint->backlash_filt = joint->backlash_corr;
            }
        }
        /* backlash (and motor offset) will be applied to output later */
        /* end of joint loop */
    }
}

/*! \todo FIXME - once the HAL refactor is done so that metadata isn't stored
   in shared memory, I want to seriously consider moving some of the
   structures into the HAL memory block.  This will eliminate most of
   this useless copying, and make nearly everything accessible to
   halscope and halmeter for debugging.
 */

static void output_to_hal(void)
{
    int joint_num, axis_num, i;
    emcmot_joint_t *joint;
    emcmot_axis_t *axis;
    joint_hal_t *joint_data;
    axis_hal_t *axis_data;
    static int old_motion_index=0, old_hal_index=0;

    /* output USB command to HAL */

    for (i=0; i<4;i++) {
        *(emcmot_hal_data->usb_cmd_param[i]) = emcmotStatus->usb_cmd_param[i];
        emcmotStatus->last_usb_cmd_param[i] = *(emcmot_hal_data->last_usb_cmd_param[i]);
    }
    *(emcmot_hal_data->usb_cmd) = emcmotStatus->usb_cmd;
    emcmotStatus->last_usb_cmd = *(emcmot_hal_data->last_usb_cmd);
    // WORKAROUND: raise align pos cmd flag at least 3 cycles to
    //             make sure the pos_cmd is aligned with pos_fb

    emcmotStatus->usb_cmd = 0;
    /* output machine info to HAL for scoping, etc */
    *(emcmot_hal_data->motion_enabled) = GET_MOTION_ENABLE_FLAG();
    *(emcmot_hal_data->in_position) = GET_MOTION_INPOS_FLAG();
    *(emcmot_hal_data->coord_mode) = GET_MOTION_COORD_FLAG();
    *(emcmot_hal_data->teleop_mode) = GET_MOTION_TELEOP_FLAG();
    *(emcmot_hal_data->homing) = emcmotStatus->homing_active;
    *(emcmot_hal_data->coord_error) = GET_MOTION_ERROR_FLAG();
    *(emcmot_hal_data->on_soft_limit) = emcmotStatus->on_soft_limit;
    *(emcmot_hal_data->spindle_speed_out) = emcmotStatus->spindle.speed_req_rps * emcmotStatus->net_spindle_scale * 60.0;
    *(emcmot_hal_data->spindle_speed_out_rps) = emcmotStatus->spindle.speed_req_rps * emcmotStatus->net_spindle_scale;
    *(emcmot_hal_data->spindle_velocity_mode) = (!emcmotStatus->spindleSync);
    *(emcmot_hal_data->spindle_forward) = (*emcmot_hal_data->spindle_speed_out > 0) ? 1 : 0;
    *(emcmot_hal_data->spindle_reverse) = (*emcmot_hal_data->spindle_speed_out < 0) ? 1 : 0;
    *(emcmot_hal_data->spindle_brake) = (emcmotStatus->spindle.brake != 0) ? 1 : 0;
    *(emcmot_hal_data->spindle_css_error) = emcmotStatus->spindle.css_error;
    *(emcmot_hal_data->spindle_css_factor) = emcmotStatus->spindle.css_factor;

    *(emcmot_hal_data->program_line) = emcmotStatus->id;
    *(emcmot_hal_data->distance_to_go) = emcmotStatus->distance_to_go;
    *(emcmot_hal_data->motion_state) = emcmotStatus->motionState;
    *(emcmot_hal_data->motion_type) = emcmotStatus->motion_type;
    *(emcmot_hal_data->xuu_per_rev) = emcmotStatus->xuu_per_rev;
    *(emcmot_hal_data->yuu_per_rev) = emcmotStatus->yuu_per_rev;
    *(emcmot_hal_data->zuu_per_rev) = emcmotStatus->zuu_per_rev;
    if(GET_MOTION_COORD_FLAG()) {
        *(emcmot_hal_data->current_vel) = emcmotStatus->current_vel;
        *(emcmot_hal_data->requested_vel) = emcmotStatus->requested_vel;
        *(emcmot_hal_data->feed_scale) = emcmotStatus->feed_scale;
    } else if (GET_MOTION_TELEOP_FLAG()) {
        int i;
        double v2 = 0.0;
        for(i=0; i < EMCMOT_MAX_AXIS; i++)
            if(axes[i].teleop_tp.active)
                v2 += axes[i].vel_cmd * axes[i].vel_cmd;
        if(v2 > 0.0)
            emcmotStatus->current_vel = (*emcmot_hal_data->current_vel) = sqrt(v2);
        else
            emcmotStatus->current_vel = (*emcmot_hal_data->current_vel) = 0.0;
        *(emcmot_hal_data->requested_vel) = 0.0;
    } else {
        int i;
        double v2 = 0.0;
        for(i=0; i < emcmotConfig->numJoints; i++)
            if(GET_JOINT_ACTIVE_FLAG(&(joints[i])) && joints[i].free_tp.active)
                v2 += joints[i].vel_cmd * joints[i].vel_cmd;
        if(v2 > 0.0)
            emcmotStatus->current_vel = (*emcmot_hal_data->current_vel) = sqrt(v2);
        else
            emcmotStatus->current_vel = (*emcmot_hal_data->current_vel) = 0.0;
        *(emcmot_hal_data->requested_vel) = 0.0;
    }

    /* These params can be used to examine any internal variable. */
    /* Change the following lines to assign the variable you want to observe
       to one of the debug parameters.  You can also comment out these lines
       and copy elsewhere if you want to observe an automatic variable that
       isn't in scope here. */
    emcmot_hal_data->debug_bit_0 = joints[1].free_tp.active;
    emcmot_hal_data->debug_bit_1 = emcmotStatus->enables_new & AF_ENABLED;
    emcmot_hal_data->debug_float_0 = emcmotStatus->net_feed_scale;
    emcmot_hal_data->debug_float_1 = emcmotStatus->carte_pos_cmd.s; // spindle position command
    emcmot_hal_data->debug_float_2 = emcmotStatus->spindleSpeedIn;
    emcmot_hal_data->debug_float_3 = emcmotStatus->net_spindle_scale;
    emcmot_hal_data->debug_s32_0 = emcmotStatus->overrideLimitMask;
    emcmot_hal_data->debug_s32_1 = emcmotStatus->tcqlen;

    /* two way handshaking for the spindle encoder */
    if(emcmotStatus->spindle_index_enable && !old_motion_index) {
        *emcmot_hal_data->spindle_index_enable = 1;
    }

    if(!*emcmot_hal_data->spindle_index_enable && old_hal_index) {
        // cur.index_enable == 0 .and. old.index_enable == 1
        // to let kinematics/tp.c to set "waiting_for_index" as 0
        emcmotStatus->spindle_index_enable = 0;
    }

    old_motion_index = emcmotStatus->spindle_index_enable;
    old_hal_index = *emcmot_hal_data->spindle_index_enable;

    *(emcmot_hal_data->tooloffset_x) = emcmotStatus->tool_offset.tran.x;
    *(emcmot_hal_data->tooloffset_y) = emcmotStatus->tool_offset.tran.y;
    *(emcmot_hal_data->tooloffset_z) = emcmotStatus->tool_offset.tran.z;
    *(emcmot_hal_data->tooloffset_a) = emcmotStatus->tool_offset.a;
    *(emcmot_hal_data->tooloffset_b) = emcmotStatus->tool_offset.b;
    *(emcmot_hal_data->tooloffset_c) = emcmotStatus->tool_offset.c;
    *(emcmot_hal_data->tooloffset_u) = emcmotStatus->tool_offset.u;
    *(emcmot_hal_data->tooloffset_v) = emcmotStatus->tool_offset.v;
    *(emcmot_hal_data->tooloffset_w) = emcmotStatus->tool_offset.w;

    /* output joint info to HAL for scoping, etc */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint struct */
        joint = &joints[joint_num];
        /* apply backlash and motor offset to output */
        joint->motor_pos_cmd =
                joint->pos_cmd + joint->backlash_filt + joint->motor_offset + joint->blender_offset;
        /* point to HAL data */
        joint_data = &(emcmot_hal_data->joint[joint_num]);
        /* write to HAL pins */
        *(joint_data->motor_offset) = joint->motor_offset;
        *(joint_data->motor_pos_cmd) = joint->motor_pos_cmd;
        *(joint_data->joint_pos_cmd) = joint->pos_cmd;
        *(joint_data->joint_pos_fb) = joint->pos_fb;
        *(joint_data->amp_enable) = GET_JOINT_ENABLE_FLAG(joint);
        *(joint_data->index_enable) = joint->index_enable;
        *(joint_data->homing) = GET_JOINT_HOMING_FLAG(joint);
        *(joint_data->coarse_pos_cmd) = joint->coarse_pos;
        *(joint_data->joint_vel_cmd) = joint->vel_cmd;
        *(joint_data->backlash_corr) = joint->backlash_corr;
        *(joint_data->backlash_filt) = joint->backlash_filt;
        *(joint_data->backlash_vel) = joint->backlash_vel;
        *(joint_data->f_error) = joint->ferror;
        *(joint_data->f_error_lim) = joint->ferror_limit;

        *(joint_data->free_pos_cmd) = joint->free_tp.pos_cmd;
        *(joint_data->free_vel_lim) = joint->free_tp.max_vel;
        *(joint_data->free_tp_enable) = joint->free_tp.enable;
        *(joint_data->kb_jog_active) = joint->kb_jog_active;
        *(joint_data->wheel_jog_active) = joint->wheel_jog_active;

        *(joint_data->active) = GET_JOINT_ACTIVE_FLAG(joint);
        *(joint_data->in_position) = GET_JOINT_INPOS_FLAG(joint);
        *(joint_data->error) = GET_JOINT_ERROR_FLAG(joint);
        *(joint_data->phl) = GET_JOINT_PHL_FLAG(joint);
        *(joint_data->nhl) = GET_JOINT_NHL_FLAG(joint);
        *(joint_data->homed) = GET_JOINT_HOMED_FLAG(joint);
        *(joint_data->f_errored) = GET_JOINT_FERROR_FLAG(joint);
        *(joint_data->faulted) = GET_JOINT_FAULT_FLAG(joint);
        *(joint_data->home_state_pin) = joint->home_state;

        *(joint_data->risc_probe_vel) = joint->risc_probe_vel;
        *(joint_data->risc_probe_dist) = joint->risc_probe_dist;
        *(joint_data->risc_probe_pin) = joint->risc_probe_pin;
        *(joint_data->risc_probe_type) = joint->risc_probe_type;

    }

    // modify update_pos_ack after all joints data are updated
    *(emcmot_hal_data->update_pos_ack) = emcmotStatus->update_pos_ack;

    /* output axis info to HAL for scoping, etc */
    for (axis_num = 0; axis_num < EMCMOT_MAX_AXIS; axis_num++) {
        /* point to axis struct */
        axis = &axes[axis_num];
        /* point to HAL data */
        axis_data = &(emcmot_hal_data->axis[axis_num]);
        /* write to HAL pins */
        *(axis_data->pos_cmd) = axis->pos_cmd;
        *(axis_data->vel_cmd) = axis->vel_cmd;
        *(axis_data->teleop_pos_cmd) = axis->teleop_tp.pos_cmd;
        *(axis_data->teleop_vel_lim) = axis->teleop_tp.max_vel;
        *(axis_data->teleop_tp_enable) = axis->teleop_tp.enable;
    }
}

static void update_status(void)
{
    int joint_num, axis_num, dio, aio;
    emcmot_joint_t *joint;
    emcmot_joint_status_t *joint_status;
    emcmot_axis_t *axis;
    emcmot_axis_status_t *axis_status;
#ifdef WATCH_FLAGS
    static int old_joint_flags[8];
    static int old_motion_flag;
#endif

    /* copy status info from private joint structure to status
       struct in shared memory */
    for (joint_num = 0; joint_num < emcmotConfig->numJoints; joint_num++) {
        /* point to joint data */
        joint = &joints[joint_num];
        /* point to joint status */
        joint_status = &(emcmotStatus->joint_status[joint_num]);
        /* copy stuff */
#ifdef WATCH_FLAGS
        /*! \todo FIXME - this is for debugging */
        if ( old_joint_flags[joint_num] != joint->flag ) {
            rtapi_print ( "Joint %d flag %04X -> %04X\n", joint_num, old_joint_flags[joint_num], joint->flag );
            old_joint_flags[joint_num] = joint->flag;
        }
#endif
        joint_status->flag = joint->flag;
        joint_status->pos_cmd = joint->pos_cmd;
        joint_status->pos_fb = joint->pos_fb;
        joint_status->vel_cmd = joint->vel_cmd;
        joint_status->ferror = joint->ferror;
        joint_status->ferror_high_mark = joint->ferror_high_mark;
        joint_status->backlash = joint->backlash;
        joint_status->max_pos_limit = joint->max_pos_limit;
        joint_status->min_pos_limit = joint->min_pos_limit;
        joint_status->min_ferror = joint->min_ferror;
        joint_status->max_ferror = joint->max_ferror;
        joint_status->home_offset = joint->home_offset;
    }

    for (axis_num = 0; axis_num < EMCMOT_MAX_AXIS; axis_num++) {
        /* point to axis data */
        axis = &axes[axis_num];
        /* point to axis status */
        axis_status = &(emcmotStatus->axis_status[axis_num]);

        axis_status->vel_cmd = axis->vel_cmd;
    }


    for (dio = 0; dio < emcmotConfig->numDIO; dio++) {
        emcmotStatus->synch_di[dio] = *(emcmot_hal_data->synch_di[dio]);
        emcmotStatus->synch_do[dio] = *(emcmot_hal_data->synch_do[dio]);
    }

    for (aio = 0; aio < emcmotConfig->numAIO; aio++) {
        emcmotStatus->analog_input[aio] = *(emcmot_hal_data->analog_input[aio]);
        emcmotStatus->analog_output[aio] = *(emcmot_hal_data->analog_output[aio]);
    }

    /*! \todo FIXME - the rest of this function is stuff that was apparently
       dropped in the initial move from emcmot.c to control.c.  I
       don't know how much is still needed, and how much is baggage.
     */

    /* motion emcmotDebug->coord_tp status */
    emcmotStatus->depth = tpQueueDepth(&emcmotDebug->coord_tp);
    emcmotStatus->activeDepth = tpActiveDepth(&emcmotDebug->coord_tp);
    emcmotStatus->id = tpGetExecId(&emcmotDebug->coord_tp);
    emcmotStatus->motionType = tpGetMotionType(&emcmotDebug->coord_tp);
    emcmotStatus->queueFull = tcqFull(&emcmotDebug->coord_tp.queue);

    /* check to see if we should pause in order to implement
       single emcmotDebug->stepping */
    if (emcmotDebug->stepping && emcmotDebug->idForStep != emcmotStatus->id) {
        tpPause(&emcmotDebug->coord_tp);
        emcmotDebug->stepping = 0;
        emcmotStatus->paused = 1;
    }
#ifdef WATCH_FLAGS
    /*! \todo FIXME - this is for debugging */
    if ( old_motion_flag != emcmotStatus->motionFlag ) {
        rtapi_print ( "Motion flag %04X -> %04X\n", old_motion_flag, emcmotStatus->motionFlag );
        old_motion_flag = emcmotStatus->motionFlag;
    }
#endif
}

