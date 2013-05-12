/********************************************************************
* Description: tc.h
*   Discriminate-based trajectory planning
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
********************************************************************/
#ifndef TC_H
#define TC_H

#include "posemath.h"
#include "emcpos.h"
#include "emcmotcfg.h"
#include "nurbs.h"

/* values for endFlag */
#define TC_TERM_COND_STOP 1
#define TC_TERM_COND_BLEND 2

#define TC_LINEAR 1
#define TC_CIRCULAR 2
#define TC_SPINDLE_SYNC_MOTION 3
#define TC_NURBS 4

/* structure for individual trajectory elements */

typedef struct {
    PmLine xyz;
    PmLine abc;
    PmLine uvw;
} PmLine9;

typedef struct {
    PmCircle xyz;
    PmLine abc;
    PmLine uvw;
} PmCircle9;

//typedef enum {
//    TAPPING, REVERSING
//} RIGIDTAP_STATE;

typedef unsigned long long iomask_t; // 64 bits on both x86 and x86_64

typedef struct {
    char anychanged;
    iomask_t dio_mask;
    iomask_t aio_mask;
    signed char dios[EMCMOT_MAX_DIO];
    char sync_input_triggered;
    int sync_in;
    int wait_type;
    double timeout;
    double aios[EMCMOT_MAX_AIO];
} syncdio_t;

typedef struct {
    // for RIGID_TAPPING(G33.1), CSS(G33 w/ G96), and THREADING(G33 w/ G97)
    PmLine xyz;             // original, but elongated, move down
    PmCartesian abc;
    PmCartesian uvw;
    double spindle_start_pos;
    int spindle_start_pos_latch;
    double spindle_dir;
} PmSpindleSyncMotion;

enum state_type {
  ACCEL_S0 = 0, // 0
  ACCEL_S1,     // 1
  ACCEL_S2,     // 2
  ACCEL_S3,     // 3
  ACCEL_S4,     // 4
  ACCEL_S5,     // 5
  ACCEL_S6      // 6
};

enum smlblnd_type {
  SMLBLND_INIT = 0, // 0
  SMLBLND_ENABLE,   // 1
  SMLBLND_DISABLE   // 2
};

typedef struct {
    double cycle_time;
    double progress;        // where are we in the segment?  0..target
    double target;          // segment length
    double distance_to_go;  // distance to go for target target..0
    int    motion_param_set;
    double ori_reqvel;      // track of original reqvel in this tc
    double ori_feed_override; // track of original feed override
    double reqvel;          // vel requested by F word, calc'd by task
    double maxaccel;        // accel calc'd by task
    double jerk;            // the accelrate of accel
    double feed_override;   // feed override requested by user
    double maxvel;          // max possible vel (feed override stops here)
    double cur_vel;         // keep track of current step (vel * cycle_time)
    double cur_accel;       // keep track of current acceleration
    double accel_dist;      // keep track of acceleration distance
    double decel_dist;      // distance to start deceleration
    double accel_time;      // keep track of acceleration time
    double vel_from;        // track velocity before speed change
    double target_vel;
    double dist_comp;
    int    on_feed_change;
    int    prev_state;
    nurbs_block_t nurbs_block; // nurbs command block
    double *N;                  // nurbs basis function buffer
    enum state_type accel_state;
    enum smlblnd_type seamless_blend_mode;
    double nexttc_target;
    
    int id;                 // segment's serial number

    union {                 // describes the segment's start and end positions
        PmLine9 line;
        PmCircle9 circle;
        PmSpindleSyncMotion spindle_sync;
    } coords;

    char motion_type;       // TC_LINEAR (coords.line) or 
                            // TC_CIRCULAR (coords.circle) or
                            // TC_SPINDLE_SYNC_MOTION (coords.spindle_sync_motion)
    char active;            // this motion is being executed
    int canon_motion_type;  // this motion is due to which canon function?
    int blend_with_next;    // gcode requests continuous feed at the end of 
                            // this segment (g64 mode)
    int blending;           // segment is being blended into following segment
    double tolerance;       // during the blend at the end of this move, 
                            // stay within this distance from the path.
    
    int synchronized;       // spindle sync required for this move
    int velocity_mode;	// TRUE if spindle sync is in velocity mode, FALSE if in position mode
    double uu_per_rev;      // for sync, user units per rev (e.g. 0.0625 for 16tpi)
    double css_progress_cmd;// feed-forward progress command for CSS motion
    int sync_accel;         // we're accelerating up to sync with the spindle
    unsigned char enables;  // Feed scale, etc, enable bits for this move
    char atspeed;           // wait for the spindle to be at-speed before starting this move
    syncdio_t syncdio;      // synched DIO's for this move. what to turn on/off
    int indexrotary;        // which rotary axis to unlock to make this move, -1 for none

    PmCartesian utvIn;      // unit tangent vector inward
    PmCartesian utvOut;     // unit tangent vector outward
} TC_STRUCT;

/* TC_STRUCT functions */

extern EmcPose tcGetEndpoint(TC_STRUCT * tc);
extern EmcPose tcGetPos(TC_STRUCT * tc);
EmcPose tcGetPosReal(TC_STRUCT * tc, int of_endpoint);
PmCartesian tcGetEndingUnitVector(TC_STRUCT *tc);
PmCartesian tcGetStartingUnitVector(TC_STRUCT *tc);

/* queue of TC_STRUCT elements*/

typedef struct {
    TC_STRUCT *queue;		/* ptr to the tcs */
    int size;			/* size of queue */
    int _len;			/* number of tcs now in queue */
    int start, end;		/* indices to next to get, next to put */
    int allFull;		/* flag meaning it's actually full */
} TC_QUEUE_STRUCT;

/* TC_QUEUE_STRUCT functions */

/* create queue of _size */
extern int tcqCreate(TC_QUEUE_STRUCT * tcq, int _size,
		     TC_STRUCT * tcSpace);

/* free up queue */
extern int tcqDelete(TC_QUEUE_STRUCT * tcq);

/* reset queue to empty */
extern int tcqInit(TC_QUEUE_STRUCT * tcq);

/* put tc on end */
extern int tcqPut(TC_QUEUE_STRUCT * tcq, TC_STRUCT tc);

/* remove n tcs from front */
extern int tcqRemove(TC_QUEUE_STRUCT * tcq, int n);

/* how many tcs on queue */
extern int tcqLen(TC_QUEUE_STRUCT * tcq);

/* look at nth item, first is 0 */
extern TC_STRUCT *tcqItem(TC_QUEUE_STRUCT * tcq, int n, long period);

/* get full status */
extern int tcqFull(TC_QUEUE_STRUCT * tcq);

#endif				/* TC_H */
