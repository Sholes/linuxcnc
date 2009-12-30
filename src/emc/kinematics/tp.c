/********************************************************************
* Description: tp.c
*   Trajectory planner based on TC elements
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
********************************************************************/

#include "rtapi.h"		/* rtapi_print_msg */
#include "rtapi_string.h"       /* NULL */
#include "posemath.h"
#include "tc.h"
#include "tp.h"
#include "rtapi_math.h"
#include "../motion/motion.h"
#include "hal.h"
#include "../motion/mot_priv.h"
#include "motion_debug.h"
#include <assert.h>

// to disable DP(): #define TRACE 0
#define TRACE 0
#include <stdint.h>
#include "dptrace.h"
#if (TRACE!=0)
static FILE* dptrace = 0;
static uint32_t _dt = 0;
#endif

extern emcmot_status_t *emcmotStatus;
extern emcmot_debug_t *emcmotDebug;

int output_chan = 0;
syncdio_t syncdio; //record tpSetDout's here

int tpCreate(TP_STRUCT * tp, int _queueSize, TC_STRUCT * tcSpace)
{
    if (0 == tp) {
	return -1;
    }

    if (_queueSize <= 0) {
	tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
	tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
	return -1;
    }

#if (TRACE!=0)
    if(!dptrace) {
      dptrace = fopen("tp.log", "w");
      /* prepare header for gnuplot */
      DPS ("%11s%15s%15s%15s%15s%15s%15s\n", 
           "#dt", "newaccel", "newvel", "cur_vel", "progress", "target", "tolerance");
    }
    _dt+=1;
#endif

    /* init the rest of our data */
    return tpInit(tp);
}


// this clears any potential DIO toggles
// anychanged signals if any DIOs need to be changed
// dios[i] = 1, DIO needs to get turned on, -1 = off
int tpClearDIOs() {
    int i;
    syncdio.anychanged = 0;
    for (i = 0; i < num_dio; i++)
	syncdio.dios[i] = 0;

    return 0;
}


/*
  tpClear() is a "soft init" in the sense that the TP_STRUCT configuration
  parameters (cycleTime, vMax, and aMax) are left alone, but the queue is
  cleared, and the flags are set to an empty, ready queue. The currentPos
  is left alone, and goalPos is set to this position.

  This function is intended to put the motion queue in the state it would
  be if all queued motions finished at the current position.
 */
int tpClear(TP_STRUCT * tp)
{
    tcqInit(&tp->queue);
    tp->queueSize = 0;
    tp->goalPos = tp->currentPos;
    tp->nextId = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tp->termCond = TC_TERM_COND_BLEND;
    tp->tolerance = 0.0;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->vScale = emcmotStatus->net_feed_scale;
    tp->synchronized = 0;
    tp->velocity_mode = 0;
    tp->uu_per_rev = 0.0;
    emcmotStatus->spindleSync = 0;
    emcmotStatus->current_vel = 0.0;
    emcmotStatus->requested_vel = 0.0;
    emcmotStatus->distance_to_go = 0.0;
    ZERO_EMC_POSE(emcmotStatus->dtg);

    return tpClearDIOs();
}


int tpInit(TP_STRUCT * tp)
{
    tp->cycleTime = 0.0;
    tp->vLimit = 0.0;
    tp->vScale = 1.0;
    tp->aMax = 0.0;
    tp->vMax = 0.0;
    tp->ini_maxvel = 0.0;
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    ZERO_EMC_POSE(tp->currentPos);
    
    return tpClear(tp);
}

int tpSetCycleTime(TP_STRUCT * tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
	return -1;
    }

    tp->cycleTime = secs;

    return 0;
}

// This is called before adding lines or circles, specifying
// vMax (the velocity requested by the F word) and
// ini_maxvel, the max velocity possible before meeting
// a machine constraint caused by an AXIS's max velocity.
// (the TP is allowed to go up to this high when feed 
// override >100% is requested)  These settings apply to
// subsequent moves until changed.

int tpSetVmax(TP_STRUCT * tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
	return -1;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return 0;
}

// I think this is the [TRAJ] max velocity.  This should
// be the max velocity of the TOOL TIP, not necessarily
// any particular axis.  This applies to subsequent moves
// until changed.

int tpSetVlimit(TP_STRUCT * tp, double vLimit)
{
    if (!tp) return -1;

    if (vLimit < 0.) 
        tp->vLimit = 0.;
    else
        tp->vLimit = vLimit;

    return 0;
}

// Set max accel

int tpSetAmax(TP_STRUCT * tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
	return -1;
    }

    tp->aMax = aMax;

    return 0;
}

/*
  tpSetId() sets the id that will be used for the next appended motions.
  nextId is incremented so that the next time a motion is appended its id
  will be one more than the previous one, modulo a signed int. If
  you want your own ids for each motion, call this before each motion
  you append and stick what you want in here.
  */
int tpSetId(TP_STRUCT * tp, int id)
{
    if (0 == tp) {
	return -1;
    }

    tp->nextId = id;

    return 0;
}

/*
  tpGetExecId() returns the id of the last motion that is currently
  executing.
  */
int tpGetExecId(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    return tp->execId;
}

/*
  tpSetTermCond(tp, cond) sets the termination condition for all subsequent
  queued moves. If cond is TC_TERM_STOP, motion comes to a stop before
  a subsequent move begins. If cond is TC_TERM_BLEND, the following move
  is begun when the current move decelerates.
  */
int tpSetTermCond(TP_STRUCT * tp, int cond, double tolerance)
{
    if (0 == tp) {
	return -1;
    }

    if (cond != TC_TERM_COND_STOP && cond != TC_TERM_COND_BLEND) {
	return -1;
    }

    tp->termCond = cond;
    tp->tolerance = tolerance;

    return 0;
}

// Used to tell the tp the initial position.  It sets
// the current position AND the goal position to be the same.  
// Used only at TP initialization and when switching modes.

int tpSetPos(TP_STRUCT * tp, EmcPose pos)
{
    if (0 == tp) {
	return -1;
    }

    tp->currentPos = pos;
    tp->goalPos = pos;

    return 0;
}

int tpAddRigidTap(TP_STRUCT *tp, EmcPose end, double vel, double ini_maxvel, 
                  double acc, unsigned char enables) 
{
    TC_STRUCT tc;
    PmLine line_xyz;
    PmPose start_xyz, end_xyz;
    PmCartesian abc, uvw;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
	return -1;
    }

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_xyz.rot = identity_quat;
    end_xyz.rot = identity_quat;
    
    // abc cannot move
    abc.x = tp->goalPos.a;
    abc.y = tp->goalPos.b;
    abc.z = tp->goalPos.c;

    uvw.x = tp->goalPos.u;
    uvw.y = tp->goalPos.v;
    uvw.z = tp->goalPos.w;

    pmLineInit(&line_xyz, start_xyz, end_xyz);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.coords.rigidtap.reversal_target = line_xyz.tmag;

    // allow 10 turns of the spindle to stop - we don't want to just go on forever
    tc.target = line_xyz.tmag + 10. * tp->uu_per_rev;

    tc.progress = 0.0;
    tc.distance_to_go = tc.target;
    tc.accel_time = 0.0;
    tc.reqvel = vel;
    tc.maxaccel = acc;
    // FIXME: the accel-increase-rate(jerk) is set as tc->maxaccel/sec
    // TODO: define accel-increase-rate(jerk) at CONFIG-FILE
    tc.jerk = 9.0 * acc;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = 1;

    tc.cur_accel = 0.0;
    tc.currentvel = 0.0;
    // tc.blending = 0;
    // tc.blend_vel = 0.0;
    // tc.nexttc_vel= 0.0;

    tc.coords.rigidtap.xyz = line_xyz;
    tc.coords.rigidtap.abc = abc;
    tc.coords.rigidtap.uvw = uvw;
    tc.coords.rigidtap.state = TAPPING;
    tc.motion_type = TC_RIGIDTAP;
    tc.canon_motion_type = 0;
    tc.blend_with_next = 0;
    tc.tolerance = tp->tolerance;

    if(!tp->synchronized) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Cannot add unsynchronized rigid tap move.\n");
        return -1;
    }
    tc.synchronized = tp->synchronized;
    
    tc.uu_per_rev = tp->uu_per_rev;
    tc.velocity_mode = tp->velocity_mode;
    tc.enables = enables;

    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }

    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
	return -1;
    }
    
    // do not change tp->goalPos here,
    // since this move will end just where it started

    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

// Add a straight line to the tc queue.  This is a coordinated
// move in any or all of the six axes.  it goes from the end
// of the previous move to the new end specified here at the
// currently-active accel and vel settings from the tp struct.

int tpAddLine(TP_STRUCT * tp, EmcPose end, int type, double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed)
{
    TC_STRUCT tc;
    PmLine line_xyz, line_uvw, line_abc;
    PmPose start_xyz, end_xyz;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
	return -1;
    }

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_xyz.rot = identity_quat;
    end_xyz.rot = identity_quat;
    start_uvw.rot = identity_quat;
    end_uvw.rot = identity_quat;
    start_abc.rot = identity_quat;
    end_abc.rot = identity_quat;

    pmLineInit(&line_xyz, start_xyz, end_xyz);
    pmLineInit(&line_uvw, start_uvw, end_uvw);
    pmLineInit(&line_abc, start_abc, end_abc);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;

    if (!line_xyz.tmag_zero) 
        tc.target = line_xyz.tmag;
    else if (!line_uvw.tmag_zero)
        tc.target = line_uvw.tmag;
    else
        tc.target = line_abc.tmag;

    tc.progress = 0.0;
    tc.distance_to_go = tc.target;
    tc.accel_time = 0.0;
    tc.reqvel = vel;
    tc.maxaccel = acc;
    // FIXME: the accel-increase-rate(jerk) is set as tc->maxaccel/sec
    // TODO: define accel-increase-rate(jerk) at CONFIG-FILE
    tc.jerk = 9.0 * acc;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = atspeed;

    tc.cur_accel = 0.0;
    tc.currentvel = 0.0;
    // tc.blending = 0;
    // tc.blend_vel = 0.0;
    // tc.nexttc_vel= 0.0;

    tc.coords.line.xyz = line_xyz;
    tc.coords.line.uvw = line_uvw;
    tc.coords.line.abc = line_abc;
    tc.motion_type = TC_LINEAR;
    tc.canon_motion_type = type;
    tc.blend_with_next = tp->termCond == TC_TERM_COND_BLEND;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;

    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }


    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
	return -1;
    }

    tp->goalPos = end;      // remember the end of this move, as it's
                            // the start of the next one.
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

// likewise, this adds a circular (circle, arc, helix) move from
// the end of the last move to this new position.  end is the
// xyzabc of the destination, center/normal/turn specify the arc
// in a way that makes sense to pmCircleInit (we don't care about
// the details here.)  Note that degenerate arcs/circles are not
// allowed; we are guaranteed to have a move in xyz so target is
// always the circle/arc/helical length.

int tpAddCircle(TP_STRUCT * tp, EmcPose end,
		PmCartesian center, PmCartesian normal, int turn, int type,
                double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed)
{
    TC_STRUCT tc;
    PmCircle circle;
    PmLine line_uvw, line_abc;
    PmPose start_xyz, end_xyz;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;
    double helix_z_component;   // z of the helix's cylindrical coord system
    double helix_length;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp || tp->aborting) 
	return -1;

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_xyz.rot = identity_quat;
    end_xyz.rot = identity_quat;
    start_uvw.rot = identity_quat;
    end_uvw.rot = identity_quat;
    start_abc.rot = identity_quat;
    end_abc.rot = identity_quat;

    pmCircleInit(&circle, start_xyz, end_xyz, center, normal, turn);
    pmLineInit(&line_uvw, start_uvw, end_uvw);
    pmLineInit(&line_abc, start_abc, end_abc);

    // find helix length
    pmCartMag(circle.rHelix, &helix_z_component);
    helix_length = pmSqrt(pmSq(circle.angle * circle.radius) +
                          pmSq(helix_z_component));

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.target = helix_length;
    tc.progress = 0.0;
    tc.distance_to_go = tc.target;
    tc.accel_time = 0.0;
    tc.reqvel = vel;
    tc.maxaccel = acc;
    // FIXME: the accel-increase-rate(jerk) is set as tc->maxaccel/sec
    // TODO: define accel-increase-rate(jerk) at CONFIG-FILE
    tc.jerk = 9.0 * acc;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = atspeed;

    tc.cur_accel = 0.0;
    tc.currentvel = 0.0;
    // tc.blending = 0;
    // tc.blend_vel = 0.0;
    // tc.nexttc_vel= 0.0;

    tc.coords.circle.xyz = circle;
    tc.coords.circle.uvw = line_uvw;
    tc.coords.circle.abc = line_abc;
    tc.motion_type = TC_CIRCULAR;
    tc.canon_motion_type = type;
    tc.blend_with_next = tp->termCond == TC_TERM_COND_BLEND;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;
    
    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }


    if (tcqPut(&tp->queue, tc) == -1) {
	return -1;
    }

    tp->goalPos = end;
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

void tcRunCycle(TP_STRUCT *tp, TC_STRUCT *tc, double *v, int *on_final_decel) {
    double /* discr, maxnewvel, */ newvel, newaccel=0;
    // double delta_accel;
    // if(!tc->blending) tc->vel_at_blend_start = tc->currentvel;
    // if(on_final_decel) *on_final_decel = 0;
    
    if (tc->progress < tc->distance_to_go) {
        // postive acceleration
        if (((tc->currentvel * 2) < tc->maxvel) &&
            ((tc->currentvel * 2) < (tc->reqvel * tc->feed_override)) &&
            // FIXME: this calculation might be wrong: 
            ((tc->currentvel * 4 * tc->accel_time) < tc->target)) {
            newaccel = tc->cur_accel + tc->jerk * tc->cycle_time;
            tc->accel_time += tc->cycle_time;
        } else {
            newaccel = tc->cur_accel - tc->jerk * tc->cycle_time;
        }
        if (newaccel > tc->maxaccel) {
            newaccel = tc->maxaccel;
        }
        if (newaccel < 0) {
            newaccel = 0;
        }
        newvel = tc->currentvel + newaccel * tc->cycle_time;
        if (newvel > tc->reqvel * tc->feed_override) {
            newvel = tc->reqvel * tc->feed_override;
            newaccel = (newvel - tc->currentvel)/tc->cycle_time;
        }
        if (newvel > tc->maxvel) {
            newvel = tc->maxvel;
            newaccel = (newvel - tc->currentvel)/tc->cycle_time;
        }
        tc->progress += (tc->currentvel + newvel) * 0.5 * tc->cycle_time;
        if (newaccel > 0) {
            tc->accel_dist = tc->progress;  // keep track of acceleration area for begin of deceleration
        }
    } else {
        if (tc->distance_to_go < tc->accel_dist) {
            // begin of deceleration
            // FIXME: calculate the start point of decel
            if (tc->accel_time >= 0) {
                newaccel = tc->cur_accel - tc->jerk * tc->cycle_time;
                tc->accel_time -= tc->cycle_time;
            } else {
                // if (tc->blending && (tc->currentvel < tc->blend_vel)) {
                //     newaccel = 0;
                // } else {
                    //TODO: find a suitable formula for blending
                    newaccel = tc->cur_accel + tc->jerk * tc->cycle_time;
                // }
                // if(on_final_decel) *on_final_decel = 1;
            }
            if (newaccel < -(tc->maxaccel)) {
                newaccel = -(tc->maxaccel);
            }
            // if (newaccel > 0) {
            //     newaccel = 0;
            // }
            assert (newaccel < tc->maxaccel);
        } else {
            newaccel = 0;
        }
        newvel = tc->currentvel + newaccel * tc->cycle_time;
        if (newvel <= 0) {
            newvel = tc->currentvel;
        }
        tc->progress += (tc->currentvel + newvel) * 0.5 * tc->cycle_time;
    }
    if (tc->progress >= tc->target) {
        newvel = 0;
        newaccel = 0;
        tc->progress = tc->target;
    }
    DPS("%11u%15.5f%15.5f%15.5f%15.5f%15.5f%15.5f\n", 
        _dt, newaccel, newvel, tc->currentvel, tc->progress, tc->target, tc->tolerance);
#if (TRACE!=0)
    _dt += 1;
#endif
    tc->distance_to_go = tc->target - tc->progress;
    tc->cur_accel = newaccel;
    tc->currentvel = newvel;
    if(v) *v = newvel;
    // orig: if(on_final_decel) *on_final_decel = fabs(maxnewvel - newvel) < 0.001;

// orig:     // discr = 0.5 * tc->cycle_time * tc->currentvel - (tc->target - tc->progress);
// orig:     if(discr > 0.0) {
// orig:         // should never happen: means we've overshot the target
// orig:         newvel = maxnewvel = 0.0;
// orig:     } else {
// orig:         // posemath.h: pmSq = ((x)*(x))
// orig:         discr = 0.25 * pmSq(tc->cycle_time) - 2.0 / tc->maxaccel * discr;
// orig:         newvel = maxnewvel = -0.5 * tc->maxaccel * tc->cycle_time + 
// orig:                               tc->maxaccel * pmSqrt(discr);
// orig:         // if (tc->currentvel < 10) {
// orig:         //     delta_accel = tc->maxaccel * 0.1;   // TODO: set jerk as parameter
// orig:         //     newaccel = fabs(tc->cur_accel + delta_accel);
// orig:         //     if (newaccel > tc->maxaccel)
// orig:         //         newaccel = tc->maxaccel;
// orig:         //     else if (newaccel < delta_accel)
// orig:         //         newaccel = delta_accel;
// orig:         // } else {
// orig:         //     newaccel = tc->maxaccel;
// orig:         // }
// orig:         // discr = 0.25 * pmSq(tc->cycle_time) - 2.0 / newaccel * discr;
// orig:         // newvel = maxnewvel = -0.5 * newaccel * tc->cycle_time + 
// orig:         //                       newaccel * pmSqrt(discr);
// orig:     DPS("%11u%15.5f%15.5f%15.5f%15.5f%15.5f\n", 
// orig:         _dt, newaccel, newvel, tc->currentvel, tc->progress, tc->blend_vel);
// orig: #if (TRACE!=0)
// orig:     _dt += 1;
// orig: #endif
// orig:     }
// orig:     if(newvel <= 0.0) {
// orig:         // also should never happen - if we already finished this tc, it was
// orig:         // caught above
// orig:         newvel = newaccel = 0.0;
// orig:         tc->progress = tc->target;
// orig:     } else {
// orig:         // constrain velocity
// orig:         if(newvel > tc->reqvel * tc->feed_override) 
// orig:             newvel = tc->reqvel * tc->feed_override;
// orig:         if(newvel > tc->maxvel) newvel = tc->maxvel;
// orig: 
// orig:         // if the motion is not purely rotary axes (and therefore in angular units) ...
// orig:         if(!(tc->motion_type == TC_LINEAR && tc->coords.line.xyz.tmag_zero && tc->coords.line.uvw.tmag_zero)) {
// orig:             // ... clamp motion's velocity at TRAJ MAX_VELOCITY (tooltip maxvel)
// orig:             // except when it's synced to spindle position.
// orig:             if((!tc->synchronized || tc->velocity_mode) && newvel > tp->vLimit) {
// orig:                 newvel = tp->vLimit;
// orig:             }
// orig:         }
// orig: 
// orig:         // get resulting acceleration
// orig:         newaccel = (newvel - tc->currentvel) / tc->cycle_time;
// orig:         
// orig:         // constrain acceleration and get resulting velocity
// orig:         if(newaccel > 0.0 && newaccel > tc->maxaccel) {
// orig:             newaccel = tc->maxaccel;
// orig:             newvel = tc->currentvel + newaccel * tc->cycle_time;
// orig:         }
// orig:         if(newaccel < 0.0 && newaccel < -tc->maxaccel) {
// orig:             newaccel = -tc->maxaccel;
// orig:             newvel = tc->currentvel + newaccel * tc->cycle_time;
// orig:         }
// orig:         // update position in this tc
// orig:         tc->progress += (newvel + tc->currentvel) * 0.5 * tc->cycle_time;
// orig:     }
// orig:     tc->cur_accel = newaccel;
// orig:     tc->currentvel = newvel;
// orig:     if(v) *v = newvel;
// orig:     if(on_final_decel) *on_final_decel = fabs(maxnewvel - newvel) < 0.001;
}


void tpToggleDIOs(TC_STRUCT * tc) {
    int i=0;
    if (tc->syncdio.anychanged != 0) { // we have DIO's to turn on or off
	for (i=0; i < num_dio; i++) {
	    if (tc->syncdio.dios[i] > 0) emcmotDioWrite(i, 1); // turn DIO[i] on
	    if (tc->syncdio.dios[i] < 0) emcmotDioWrite(i, 0); // turn DIO[i] off
	}
	tc->syncdio.anychanged = 0; //we have turned them all on/off, nothing else to do for this TC the next time
    }
}

// This is the brains of the operation.  It's called every TRAJ period
// and is expected to set tp->currentPos to the new machine position.
// Lots of other tp fields (depth, done, etc) have to be twiddled to
// communicate the status; I think those are spelled out here correctly
// and I can't clean it up without breaking the API that the TP presents
// to motion.  It's not THAT bad and in the interest of not touching
// stuff outside this directory, I'm going to leave it for now.

int tpRunCycle(TP_STRUCT * tp, long period)
{
    // vel = (new position - old position) / cycle time
    // (two position points required)
    //
    // acc = (new vel - old vel) / cycle time
    // (three position points required)

    TC_STRUCT *tc, *nexttc;
    double primary_vel;
    int on_final_decel;
    EmcPose primary_before, primary_after;
    EmcPose secondary_before, secondary_after;
    EmcPose primary_displacement, secondary_displacement;
    static double spindleoffset;
    static int waiting_for_index = 0;
    static int waiting_for_atspeed = 0;
    // double save_vel;
    static double revs;
    EmcPose target;

    emcmotStatus->tcqlen = tcqLen(&tp->queue);
    emcmotStatus->requested_vel = 0.0;
    tc = tcqItem(&tp->queue, 0, period);
    if(!tc) {
        // this means the motion queue is empty.  This can represent
        // the end of the program OR QUEUE STARVATION.  In either case,
        // I want to stop.  Some may not agree that's what it should do.
        tcqInit(&tp->queue);
        tp->goalPos = tp->currentPos;
        tp->done = 1;
        tp->depth = tp->activeDepth = 0;
        tp->aborting = 0;
        tp->execId = 0;
        tp->motionType = 0;
        tpResume(tp);
	// when not executing a move, use the current enable flags
	emcmotStatus->enables_queued = emcmotStatus->enables_new;
        return 0;
    }

    if (tc->target == tc->progress && waiting_for_atspeed != tc->id) {
        // if we're synced, and this move is ending, save the
        // spindle position so the next synced move can be in
        // the right place.
        if(tc->synchronized)
            spindleoffset += tc->target/tc->uu_per_rev;
        else
            spindleoffset = 0.0;

        // done with this move
        tcqRemove(&tp->queue, 1);

        // so get next move
        tc = tcqItem(&tp->queue, 0, period);
        if(!tc) return 0;
    }

    // now we have the active tc.  get the upcoming one, if there is one.
    // it's not an error if there isn't another one - we just don't
    // do blending.  This happens in MDI for instance.
    if(!emcmotDebug->stepping && tc->blend_with_next) 
        nexttc = tcqItem(&tp->queue, 1, period);
    else
        nexttc = NULL;

    {
	int this_synch_pos = tc->synchronized && !tc->velocity_mode;
	int next_synch_pos = nexttc && nexttc->synchronized && !nexttc->velocity_mode;
	if(!this_synch_pos && next_synch_pos) {
	    // we'll have to wait for spindle sync; might as well
	    // stop at the right place (don't blend)
	    tc->blend_with_next = 0;
	    nexttc = NULL;
	}
    }

    if(nexttc && nexttc->atspeed) {
        // we'll have to wait for the spindle to be at-speed; might as well
        // stop at the right place (don't blend), like above
        tc->blend_with_next = 0;
        nexttc = NULL;
    }

    if(tp->aborting) {
        // an abort message has come
        if( waiting_for_index ||
            waiting_for_atspeed || 
            (tc->currentvel == 0.0 && !nexttc) || 
            (tc->currentvel == 0.0 && nexttc && nexttc->currentvel == 0.0) ) {
            tcqInit(&tp->queue);
            tp->goalPos = tp->currentPos;
            tp->done = 1;
            tp->depth = tp->activeDepth = 0;
            tp->aborting = 0;
            tp->execId = 0;
            tp->motionType = 0;
            tp->synchronized = 0;
            waiting_for_index = 0;
            waiting_for_atspeed = 0;
            emcmotStatus->spindleSync = 0;
            tpResume(tp);
            return 0;
        } else {
            tc->reqvel = 0.0;
            if(nexttc) nexttc->reqvel = 0.0;
        }
    }

    // this is no longer the segment we were waiting_for_index for
    if(waiting_for_index && waiting_for_index != tc->id) 
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for index on motion id %d, but reached id %d\n",
                waiting_for_index, tc->id);
        waiting_for_index = 0;
    }
    if(waiting_for_atspeed && waiting_for_atspeed != tc->id)  
    {

        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for atspeed on motion id %d, but reached id %d\n",
                waiting_for_atspeed, tc->id);
        waiting_for_atspeed = 0;
    }

    // check for at-speed before marking the tc active
    if(waiting_for_atspeed) {
        if(!emcmotStatus->spindle_is_atspeed) {
            /* spindle is still not at the right speed: wait */
            return 0;
        } else {
            waiting_for_atspeed = 0;
        }
    }

    if(tc->active == 0) {
        // this means this tc is being read for the first time.

        // wait for atspeed, if motion requested it.  also, force
        // atspeed check for the start of all spindle synchronized
        // moves.
        if((tc->atspeed || (tc->synchronized && !tc->velocity_mode && !emcmotStatus->spindleSync)) && 
           !emcmotStatus->spindle_is_atspeed) {
            waiting_for_atspeed = tc->id;
            return 0;
        }

        tc->active = 1;
        tc->currentvel = 0;
        tp->depth = tp->activeDepth = 1;
        tp->motionType = tc->canon_motion_type;
        // tc->blending = 0;

        // honor accel constraint in case we happen to make an acute angle
        // with the next segment.
        if(tc->blend_with_next) 
            tc->maxaccel /= 2.0;

        if(tc->synchronized) {
            if(!tc->velocity_mode && !emcmotStatus->spindleSync) {
                // if we aren't already synced, wait
                waiting_for_index = tc->id;
                // ask for an index reset
                emcmotStatus->spindle_index_enable = 1;
                spindleoffset = 0.0;
                // don't move: wait
                return 0;
            }
        }
    }

    if(waiting_for_index) {
        if(emcmotStatus->spindle_index_enable) {
            /* haven't passed index yet */
            return 0;
        } else {
            /* passed index, start the move */
            emcmotStatus->spindleSync = 1;
            waiting_for_index=0;
            tc->sync_accel=1;
            revs=0;
        }
    }

    if (tc->motion_type == TC_RIGIDTAP) {
        static double old_spindlepos;
        double new_spindlepos = emcmotStatus->spindleRevs;

        switch (tc->coords.rigidtap.state) {
        case TAPPING:
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                // command reversal
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = REVERSING;
            }
            break;
        case REVERSING:
            if (new_spindlepos < old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                // we've stopped, so set a new target at the original position
                tc->coords.rigidtap.spindlerevs_at_reversal = new_spindlepos + spindleoffset;
                
                pmLinePoint(&tc->coords.rigidtap.xyz, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, start, end);
                tc->coords.rigidtap.reversal_target = aux->tmag;
                tc->target = aux->tmag + 10. * tc->uu_per_rev;
                tc->progress = 0.0;

                tc->coords.rigidtap.state = RETRACTION;
            }
            old_spindlepos = new_spindlepos;
            break;
        case RETRACTION:
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = FINAL_REVERSAL;
            }
            break;
        case FINAL_REVERSAL:
            if (new_spindlepos > old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                pmLinePoint(aux, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, start, end);
                tc->target = aux->tmag;
                tc->progress = 0.0;
                tc->synchronized = 0;
                tc->reqvel = tc->maxvel;
                
                tc->coords.rigidtap.state = FINAL_PLACEMENT;
            }
            old_spindlepos = new_spindlepos;
            break;
        case FINAL_PLACEMENT:
            // this is a regular move now, it'll stop at target above.
            break;
        }
    }


    if(!tc->synchronized) emcmotStatus->spindleSync = 0;


    if(nexttc && nexttc->active == 0) {
        // this means this tc is being read for the first time.

        nexttc->currentvel = 0;
        tp->depth = tp->activeDepth = 1;
        nexttc->active = 1;
        // nexttc->blending = 0;

        // honor accel constraint if we happen to make an acute angle with the
        // above segment or the following one
        if(tc->blend_with_next || nexttc->blend_with_next)
            nexttc->maxaccel /= 2.0;
    }


    if(tc->synchronized) {
        double pos_error;
        double oldrevs = revs;

        if(tc->velocity_mode) {
            pos_error = fabs(emcmotStatus->spindleSpeedIn) * tc->uu_per_rev;
            if(nexttc) pos_error -= nexttc->progress; /* ?? */
            if(!tp->aborting) {
                tc->feed_override = emcmotStatus->net_feed_scale;
                tc->reqvel = pos_error;
            }
        } else {
            double spindle_vel, target_vel;
            if(tc->motion_type == TC_RIGIDTAP && 
               (tc->coords.rigidtap.state == RETRACTION || 
                tc->coords.rigidtap.state == FINAL_REVERSAL))
                revs = tc->coords.rigidtap.spindlerevs_at_reversal - 
                    emcmotStatus->spindleRevs;
            else
                revs = emcmotStatus->spindleRevs;

            pos_error = (revs - spindleoffset) * tc->uu_per_rev - tc->progress;
            if(nexttc) pos_error -= nexttc->progress;

            if(tc->sync_accel) {
                // ysli: TODO: study tc->sync_accel
                // detect when velocities match, and move the target accordingly.
                // acceleration will abruptly stop and we will be on our new target.
                spindle_vel = revs/(tc->cycle_time * tc->sync_accel++);
                target_vel = spindle_vel * tc->uu_per_rev;
                if(tc->currentvel >= target_vel) {
                    // move target so as to drive pos_error to 0 next cycle
                    spindleoffset = revs - tc->progress/tc->uu_per_rev;
                    tc->sync_accel = 0;
                    tc->reqvel = target_vel;
                } else {
                    // beginning of move and we are behind: accel as fast as we can
                    tc->reqvel = tc->maxvel;
                }
            } else {
                // we have synced the beginning of the move as best we can -
                // track position (minimize pos_error).
                double errorvel;
                spindle_vel = (revs - oldrevs) / tc->cycle_time;
                target_vel = spindle_vel * tc->uu_per_rev;
                errorvel = pmSqrt(fabs(pos_error) * tc->maxaccel);
                if(pos_error<0) errorvel = -errorvel;
                tc->reqvel = target_vel + errorvel;
            }
            tc->feed_override = 1.0;
        }
        if(tc->reqvel < 0.0) tc->reqvel = 0.0;
        if(nexttc) {
	    if (nexttc->synchronized) {
		nexttc->reqvel = tc->reqvel;
		nexttc->feed_override = 1.0;
		if(nexttc->reqvel < 0.0) nexttc->reqvel = 0.0;
	    } else {
		nexttc->feed_override = emcmotStatus->net_feed_scale;
	    }
	}
    } else {
        tc->feed_override = emcmotStatus->net_feed_scale;
        if(nexttc) {
	    nexttc->feed_override = emcmotStatus->net_feed_scale;
	}
    }
    /* handle pausing */
    if(tp->pausing && (!tc->synchronized || tc->velocity_mode)) {
        tc->feed_override = 0.0;
        if(nexttc) {
	    nexttc->feed_override = 0.0;
	}
    }

    //TODO: removing blend_vel // calculate the approximate peak velocity the nexttc will hit.
    //TODO: removing blend_vel // we know to start blending it in when the current tc goes below
    //TODO: removing blend_vel // this velocity...
    //TODO: removing blend_vel if(nexttc && nexttc->maxaccel) {
    //TODO: removing blend_vel     tc->blend_vel = pmSq(nexttc->maxaccel)/nexttc->jerk;
    //TODO: removing blend_vel     if (tc->blend_vel * 2 * nexttc->maxaccel/nexttc->jerk > nexttc->target) {
    //TODO: removing blend_vel         // has to lower tc->blend_vel;
    //TODO: removing blend_vel         tc->blend_vel = 0.5 * nexttc->target * nexttc->jerk / nexttc->maxaccel;
    //TODO: removing blend_vel     }
    //TODO: removing blend_vel     if(tc->blend_vel > nexttc->maxvel) {
    //TODO: removing blend_vel         tc->blend_vel = nexttc->maxvel;
    //TODO: removing blend_vel     }
    //TODO: removing blend_vel     if(tc->blend_vel > nexttc->reqvel * nexttc->feed_override) {
    //TODO: removing blend_vel         // segment has a cruise phase so let's blend over the 
    //TODO: removing blend_vel         // whole accel period if possible
    //TODO: removing blend_vel         tc->blend_vel = nexttc->reqvel * nexttc->feed_override;
    //TODO: removing blend_vel     }
    //TODO: removing blend_vel     if(tc->maxaccel < nexttc->maxaccel) {
    //TODO: removing blend_vel         tc->blend_vel *= pmSq(tc->maxaccel/nexttc->maxaccel)
    //TODO: removing blend_vel                          / (tc->jerk/nexttc->jerk);
    //TODO: removing blend_vel     }

    //TODO: removing blend_vel     if(tc->tolerance) {
    //TODO: removing blend_vel         /* see diagram blend.fig.  T (blend tolerance) is given, theta
    //TODO: removing blend_vel          * is calculated from dot(s1,s2)
    //TODO: removing blend_vel          *
    //TODO: removing blend_vel          * blend criteria: we are decelerating at the end of segment s1
    //TODO: removing blend_vel          * and we pass distance d from the end.  
    //TODO: removing blend_vel          * find the corresponding velocity v when passing d.
    //TODO: removing blend_vel          *
    //TODO: removing blend_vel          * in the drawing note d = 2T/cos(theta)
    //TODO: removing blend_vel          *
    //TODO: removing blend_vel          * when v1 is decelerating at a to stop, v = at, t = v/a
    //TODO: removing blend_vel          * so required d = .5 a (v/a)^2
    //TODO: removing blend_vel          *
    //TODO: removing blend_vel          * equate the two expressions for d and solve for v
    //TODO: removing blend_vel          */
    //TODO: removing blend_vel         double tblend_vel;
    //TODO: removing blend_vel         double dot;
    //TODO: removing blend_vel         double theta;
    //TODO: removing blend_vel         PmCartesian v1, v2;

    //TODO: removing blend_vel         v1 = tcGetEndingUnitVector(tc);
    //TODO: removing blend_vel         v2 = tcGetStartingUnitVector(nexttc);
    //TODO: removing blend_vel         pmCartCartDot(v1, v2, &dot);

    //TODO: removing blend_vel         theta = acos(-dot)/2.0; 
    //TODO: removing blend_vel         if(cos(theta) > 0.001) {
    //TODO: removing blend_vel             tblend_vel = 2.0 * pmSqrt(tc->maxaccel * tc->tolerance / cos(theta));
    //TODO: removing blend_vel             if(tblend_vel < tc->blend_vel)
    //TODO: removing blend_vel                 tc->blend_vel = tblend_vel;
    //TODO: removing blend_vel         }
    //TODO: removing blend_vel     }
    //TODO: removing blend_vel }

    primary_before = tcGetPos(tc);
    tcRunCycle(tp, tc, &primary_vel, &on_final_decel);
    primary_after = tcGetPos(tc);
    pmCartCartSub(primary_after.tran, primary_before.tran, 
            &primary_displacement.tran);
    primary_displacement.a = primary_after.a - primary_before.a;
    primary_displacement.b = primary_after.b - primary_before.b;
    primary_displacement.c = primary_after.c - primary_before.c;

    primary_displacement.u = primary_after.u - primary_before.u;
    primary_displacement.v = primary_after.v - primary_before.v;
    primary_displacement.w = primary_after.w - primary_before.w;

    // blend criteria
    //orig:  if((tc->blending && nexttc) || 
    //orig:          (nexttc && on_final_decel && primary_vel < tc->blend_vel)) 
    if (nexttc && (tc->distance_to_go <= tc->tolerance)) {
        // make sure we continue to blend this segment even when its 
        // accel reaches 0 (at the very end)
        // tc->blending = 1;

        if(tc->currentvel > nexttc->currentvel) {
            target = tcGetEndpoint(tc);
            tp->motionType = tc->canon_motion_type;
	    emcmotStatus->distance_to_go = tc->distance_to_go;
	    emcmotStatus->enables_queued = tc->enables;
	    // report our line number to the guis
	    tp->execId = tc->id;
            emcmotStatus->requested_vel = tc->reqvel;
        } else {
	    tpToggleDIOs(nexttc); //check and do DIO changes
            target = tcGetEndpoint(nexttc);
            tp->motionType = nexttc->canon_motion_type;
	    emcmotStatus->distance_to_go = nexttc->distance_to_go;
	    emcmotStatus->enables_queued = nexttc->enables;
	    // report our line number to the guis
	    tp->execId = nexttc->id;
            emcmotStatus->requested_vel = nexttc->reqvel;
        }

        emcmotStatus->current_vel = tc->currentvel + nexttc->currentvel;

        secondary_before = tcGetPos(nexttc);
        // orig: save_vel = nexttc->reqvel;
        // orig: nexttc->reqvel = nexttc->feed_override > 0.0 ? 
        // orig:     ((tc->vel_at_blend_start - primary_vel) / nexttc->feed_override) :
        // orig:     0.0;
        tcRunCycle(tp, nexttc, NULL, NULL);
        // tc->nexttc_vel = nexttc->currentvel;
        // orig: nexttc->reqvel = save_vel;

        secondary_after = tcGetPos(nexttc);
        pmCartCartSub(secondary_after.tran, secondary_before.tran, 
                &secondary_displacement.tran);
        secondary_displacement.a = secondary_after.a - secondary_before.a;
        secondary_displacement.b = secondary_after.b - secondary_before.b;
        secondary_displacement.c = secondary_after.c - secondary_before.c;

        secondary_displacement.u = secondary_after.u - secondary_before.u;
        secondary_displacement.v = secondary_after.v - secondary_before.v;
        secondary_displacement.w = secondary_after.w - secondary_before.w;

        pmCartCartAdd(tp->currentPos.tran, primary_displacement.tran, 
                &tp->currentPos.tran);
        pmCartCartAdd(tp->currentPos.tran, secondary_displacement.tran, 
                &tp->currentPos.tran);
        tp->currentPos.a += primary_displacement.a + secondary_displacement.a;
        tp->currentPos.b += primary_displacement.b + secondary_displacement.b;
        tp->currentPos.c += primary_displacement.c + secondary_displacement.c;

        tp->currentPos.u += primary_displacement.u + secondary_displacement.u;
        tp->currentPos.v += primary_displacement.v + secondary_displacement.v;
        tp->currentPos.w += primary_displacement.w + secondary_displacement.w;
    } else { // if (nexttc && (tc->distance_to_go <= tc->tolerance))
	tpToggleDIOs(tc); //check and do DIO changes
        target = tcGetEndpoint(tc);
        tp->motionType = tc->canon_motion_type;
	emcmotStatus->distance_to_go = tc->distance_to_go;
        tp->currentPos = primary_after;
        emcmotStatus->current_vel = tc->currentvel;
        emcmotStatus->requested_vel = tc->reqvel;
	emcmotStatus->enables_queued = tc->enables;
	// report our line number to the guis
	tp->execId = tc->id;
    }

    emcmotStatus->dtg.tran.x = target.tran.x - tp->currentPos.tran.x;
    emcmotStatus->dtg.tran.y = target.tran.y - tp->currentPos.tran.y;
    emcmotStatus->dtg.tran.z = target.tran.z - tp->currentPos.tran.z;
    emcmotStatus->dtg.a = target.a - tp->currentPos.a;
    emcmotStatus->dtg.b = target.b - tp->currentPos.b;
    emcmotStatus->dtg.c = target.c - tp->currentPos.c;
    emcmotStatus->dtg.u = target.u - tp->currentPos.u;
    emcmotStatus->dtg.v = target.v - tp->currentPos.v;
    emcmotStatus->dtg.w = target.w - tp->currentPos.w;

    return 0;
}

int tpSetSpindleSync(TP_STRUCT * tp, double sync, int mode) {
    if(sync) {
        tp->synchronized = 1;
        tp->uu_per_rev = sync;
        tp->velocity_mode = mode;
    } else
        tp->synchronized = 0;

    return 0;
}

int tpPause(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }
    tp->pausing = 1;
    return 0;
}

int tpResume(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }
    tp->pausing = 0;
    return 0;
}

int tpAbort(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    if (!tp->aborting) {
	/* to abort, signal a pause and set our abort flag */
	tpPause(tp);
	tp->aborting = 1;
    }
    return tpClearDIOs(); //clears out any already cached DIOs
}

int tpGetMotionType(TP_STRUCT * tp)
{
    return tp->motionType;
}

EmcPose tpGetPos(TP_STRUCT * tp)
{
    EmcPose retval;

    if (0 == tp) {
        ZERO_EMC_POSE(retval);
	return retval;
    }

    return tp->currentPos;
}

int tpIsDone(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->done;
}

int tpQueueDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->activeDepth;
}

int tpSetAout(TP_STRUCT *tp, unsigned char index, double start, double end) {
    return 0;
}

int tpSetDout(TP_STRUCT *tp, int index, unsigned char start, unsigned char end) {
    if (0 == tp) {
	return -1;
    }
    syncdio.anychanged = 1; //something has changed
    if (start > 0)
	syncdio.dios[index] = 1; // the end value can't be set from canon currently, and has the same value as start
    else 
	syncdio.dios[index] = -1;
    return 0;    
}

// vim:sw=4:sts=4:et:
