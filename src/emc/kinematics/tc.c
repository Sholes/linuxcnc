/*!
********************************************************************
* Description: tc.c
*\brief Discriminate-based trajectory planning
*
*\author Derived from a work by Fred Proctor & Will Shackleford
*\author rewritten by Chris Radek
*
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
********************************************************************/

/*
  FIXME-- should include <stdlib.h> for sizeof(), but conflicts with
  a bunch of <linux> headers
  */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "rtapi.h"		/* rtapi_print_msg */
#include "posemath.h"
#include "emcpos.h"
#include "tc.h"
#include "nurbs.h"
#include "../motion/motion.h"
//#include "hal.h"
//#include "../motion/mot_priv.h"
//#include "motion_debug.h"

#define TRACE 0
#include "dptrace.h"

#if (TRACE != 0)
    static FILE *dptrace = NULL;
    static uint32_t _dt = 0;
#endif

extern emcmot_status_t *emcmotStatus;


int nurbs_findspan (int n, int p, double u, double *U)
{
  // FIXME : this implementation has linear, rather than log complexity
  int ret = 0;
  while ((ret++ < n) && (U[ret] <= u)) {
  };
  return (ret-1);
}

// Basis Function.
//
// INPUT:
//
//   i - knot span  ( from FindSpan() )
//   u - parametric point
//   p - spline degree
//   U - knot sequence
//
// OUTPUT:
//
//   N - Basis functions vector[p+1]  sizeof(double)*(p+1)
//
// Algorithm A2.2 from 'The NURBS BOOK' pg70.
void nurbs_basisfun(int i, double u, int p,
              double *U,
              double *N)
{
  int j,r;
  double saved, temp;

  double *left = (double*)malloc(sizeof(double)*(p+1));
  double *right = (double*)malloc(sizeof(double)*(p+1));

  N[0] = 1.0;
  for (j = 1; j <= p; j++)
    {
      left[j]  = u - U[i+1-j];
      right[j] = U[i+j] - u;
      saved = 0.0;

      for (r = 0; r < j; r++)
        {
          temp = N[r] / (right[r+1] + left[j-r]);
          N[r] = saved + right[r+1] * temp;
          saved = left[j-r] * temp;
        }

      N[j] = saved;

    }
  free(left);
  free(right);

}

PmCartesian tcGetStartingUnitVector(TC_STRUCT *tc) {
    PmCartesian v;

    if(tc->motion_type == TC_LINEAR || tc->motion_type == TC_RIGIDTAP) {
        pmCartCartSub(tc->coords.line.xyz.end.tran, tc->coords.line.xyz.start.tran, &v);
    } else {
        PmPose startpoint;
        PmCartesian radius;
        PmCartesian tan, perp;

        pmCirclePoint(&tc->coords.circle.xyz, 0.0, &startpoint);
        pmCartCartSub(startpoint.tran, tc->coords.circle.xyz.center, &radius);
        pmCartCartCross(tc->coords.circle.xyz.normal, radius, &tan);
        pmCartUnit(tan, &tan);

        pmCartCartSub(tc->coords.circle.xyz.center, startpoint.tran, &perp);
        pmCartUnit(perp, &perp);

        pmCartScalMult(tan, tc->maxaccel, &tan);
        pmCartScalMult(perp, pmSq(0.5 * tc->reqvel)/tc->coords.circle.xyz.radius, &perp);
        pmCartCartAdd(tan, perp, &v);
    }
    pmCartUnit(v, &v);
    return v;
}

PmCartesian tcGetEndingUnitVector(TC_STRUCT *tc) {
    PmCartesian v;

    if(tc->motion_type == TC_LINEAR) {
        pmCartCartSub(tc->coords.line.xyz.end.tran, tc->coords.line.xyz.start.tran, &v);
    } else if(tc->motion_type == TC_RIGIDTAP) {
        // comes out the other way
        pmCartCartSub(tc->coords.line.xyz.start.tran, tc->coords.line.xyz.end.tran, &v);
    } else {
        PmPose endpoint;
        PmCartesian radius;

        pmCirclePoint(&tc->coords.circle.xyz, tc->coords.circle.xyz.angle, &endpoint);
        pmCartCartSub(endpoint.tran, tc->coords.circle.xyz.center, &radius);
        pmCartCartCross(tc->coords.circle.xyz.normal, radius, &v);
    }
    pmCartUnit(v, &v);
    return v;
}

/*! tcGetPos() function
 *
 * \brief This function calculates the machine position along the motion's path.
 *
 * As we move along a TC, from zero to its length, we call this function repeatedly,
 * with an increasing tc->progress.
 * This function calculates the machine position along the motion's path 
 * corresponding to the current progress.
 * It gets called at the end of tpRunCycle()
 * 
 * @param    tc    the current TC that is being planned
 *
 * @return	 EmcPose   returns a position (\ref EmcPose = datatype carrying XYZABC information
 */   

EmcPose tcGetPos(TC_STRUCT * tc) {
    return tcGetPosReal(tc, 0);
}

EmcPose tcGetEndpoint(TC_STRUCT * tc) {
    return tcGetPosReal(tc, 1);
}


EmcPose tcGetPosReal(TC_STRUCT * tc, int of_endpoint)
{
    EmcPose pos;
    PmPose xyz;
    PmPose abc;
    PmPose uvw;
    
    double progress = of_endpoint? tc->target: tc->progress;
#if(TRACE != 0)
    static double last_l, last_u,last_x = 0 , last_y = 0, last_z = 0, last_a = 0;
#endif

    if (tc->motion_type == TC_RIGIDTAP) {
        pmLinePoint(&tc->coords.rigidtap.xyz, tc->coords.rigidtap.xyz.tmag * (progress / tc->target) , &xyz);
        // no rotary move allowed while tapping
        abc.tran = tc->coords.rigidtap.abc;
        uvw.tran = tc->coords.rigidtap.uvw;
        if (!of_endpoint)
        {
            emcmotStatus->spindle_position_cmd = tc->coords.rigidtap.spindle_start_pos + tc->coords.rigidtap.spindle_dir * progress;
        }
    } else if (tc->motion_type == TC_LINEAR) {

        if (tc->coords.line.xyz.tmag > 0.) {
            // progress is along xyz, so uvw and abc move proportionally in order
            // to end at the same time.
            pmLinePoint(&tc->coords.line.xyz, progress, &xyz);
            pmLinePoint(&tc->coords.line.uvw,
                        progress * tc->coords.line.uvw.tmag / tc->target,
                        &uvw);
            pmLinePoint(&tc->coords.line.abc,
                        progress * tc->coords.line.abc.tmag / tc->target,
                        &abc);
        } else if (tc->coords.line.uvw.tmag > 0.) {
            // xyz is not moving
            pmLinePoint(&tc->coords.line.xyz, 0.0, &xyz);
            pmLinePoint(&tc->coords.line.uvw, progress, &uvw);
            // abc moves proportionally in order to end at the same time
            pmLinePoint(&tc->coords.line.abc,
                        progress * tc->coords.line.abc.tmag / tc->target,
                        &abc);
        } else {
            // if all else fails, it's along abc only
            pmLinePoint(&tc->coords.line.xyz, 0.0, &xyz);
            pmLinePoint(&tc->coords.line.uvw, 0.0, &uvw);
            pmLinePoint(&tc->coords.line.abc, progress, &abc);
        }
    } else if (tc->motion_type == TC_CIRCULAR) {//we have TC_CIRCULAR
        // progress is always along the xyz circle.  This simplification
        // is possible since zero-radius arcs are not allowed by the interp.

        pmCirclePoint(&tc->coords.circle.xyz,
                      progress * tc->coords.circle.xyz.angle / tc->target,
                      &xyz);
        // abc moves proportionally in order to end at the same time as the
        // circular xyz move.
        pmLinePoint(&tc->coords.circle.abc,
                    progress * tc->coords.circle.abc.tmag / tc->target,
                    &abc);
        // same for uvw
        pmLinePoint(&tc->coords.circle.uvw,
                    progress * tc->coords.circle.uvw.tmag / tc->target,
                    &uvw);

    } else {
        int s, tmp1,i;
        double       u,*N,R, X, Y, Z, A, B, C, U, V, W, F, D;
        double       curve_accel;
#if(TRACE != 0)
        double delta_l, delta_u, delta_d, delta_x, delta_y, delta_z, delta_a;
#endif
        N = tc->nurbs_block.N;
//        NL = tc->nurbs_block.NL;
        assert(tc->motion_type == TC_NURBS);

        u = progress / tc->target;
        if (u<1) {

            s = nurbs_findspan(tc->nurbs_block.nr_of_ctrl_pts-1,  tc->nurbs_block.order - 1,
                                u, tc->nurbs_block.knots_ptr);  //return span index of u_i
            nurbs_basisfun(s, u, tc->nurbs_block.order - 1 , tc->nurbs_block.knots_ptr , N);    // input: s:knot span index u:u_0 d:B-Spline degree  k:Knots
                           // output: N:basis functions
            // refer to bspeval.cc::line(70) of octave
            // refer to opennurbs_evaluate_nurbs.cpp::line(985) of openNurbs
            // refer to ON_NurbsCurve::Evaluate() for ...
            // refer to opennurbs_knot.cpp::ON_NurbsSpanIndex()
            // http://www.rhino3d.com/nurbs.htm (What is NURBS?)
            //    Some modelers that use older algorithms for NURBS
            //    evaluation require two extra knot values for a total of
            //    degree+N+1 knots. When Rhino is exporting and importing
            //    NURBS geometry, it automatically adds and removes these
            //    two superfluous knots as the situation requires.
            tmp1 = s - tc->nurbs_block.order + 1;
            assert(tmp1 >= 0);
            assert(tmp1 < tc->nurbs_block.nr_of_ctrl_pts);

            R = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1 ; i++) {

                R += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].R;
            }

            X = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    X += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].X;

            }
            X = X/R;
            xyz.tran.x = X;

            Y = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    Y += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].Y;
            }
            Y = Y/R;
            xyz.tran.y = Y;

            Z = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    Z += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].Z;
            }
            Z = Z/R;
            xyz.tran.z = Z;

            A = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    A += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].A;
            }
            A = A/R;
            abc.tran.x = A;

            B = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    B += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].B;
            }
            B = B/R;
            abc.tran.y = B;

            C = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    C += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].C;
            }
            C = C/R;
            abc.tran.z = C;

            U = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    U += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].U;
            }
            U = U/R;
            uvw.tran.x = U;

            V = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    V += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].V;
            }
            V = V/R;
            uvw.tran.y = V;

            W = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    W += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].W;
            }
            W = W/R;
            uvw.tran.z = W;

            F = 0.0;
            F = tc->nurbs_block.ctrl_pts_ptr[tmp1].F;
            tc->reqvel = F;

            D = 0.0;
            for (i=0; i<=tc->nurbs_block.order -1; i++) {
                    D += N[i]*tc->nurbs_block.ctrl_pts_ptr[tmp1+i].D;
            }
            D = D/R;

            // compute allowed feed
            if(!of_endpoint) {
                curve_accel = (tc->cur_vel * tc->cur_vel)/D;
                if(curve_accel > tc->maxaccel) {
                    // modify req_vel
                    tc->reqvel = pmSqrt((tc->maxaccel * D));
                }
            }

#if 0
                if(l == 0 && _dt == 0) {
                    last_l = 0;
                    last_u = 0;
                    last_x = xyz.tran.x;
                    last_y = xyz.tran.y;
                    last_z = xyz.tran.z;
                    last_a = 0;
                    _dt+=1;
                }
                delta_l = l - last_l;
                last_l = l;
                delta_u = u - last_u;
                last_u = u;
                delta_x = xyz.tran.x - last_x;
                delta_y = xyz.tran.y - last_y;
                delta_z = xyz.tran.z - last_z;
                delta_a = abc.tran.x - last_a;
                delta_d = pmSqrt(pmSq(delta_x)+pmSq(delta_y)+pmSq(delta_z));
                last_x = xyz.tran.x;
                last_y = xyz.tran.y;
                last_z = xyz.tran.z;
                last_a = abc.tran.x;
                if( delta_d > 0)
                {
                    if(_dt == 1){
                      /* prepare header for gnuplot */
                        DPS ("%11s%15s%15s%15s%15s%15s%15s%15s%15s\n",
                           "#dt", "u", "l","x","y","z","delta_d", "delta_l","a");
                    }

                    DPS("%11u%15.10f%15.10f%15.5f%15.5f%15.5f%15.5f%15.5f%15.5f\n",
                           _dt, u, l,last_x, last_y, last_z, delta_d, delta_l, last_a);

                    _dt+=1;
                }
#endif // (TRACE != 0)

        }else {
            xyz.tran.x = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].X;
            xyz.tran.y = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].Y;
            xyz.tran.z = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].Z;
            uvw.tran.x = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].U;
            uvw.tran.y = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].V;
            uvw.tran.z = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].W;
            abc.tran.x = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].A;
            abc.tran.y = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].B;
            abc.tran.z = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].C;
           // R = tc->nurbs_block.ctrl_pts_ptr[tc->nurbs_block.nr_of_ctrl_pts-1].R;
        }
    }
    //DP ("GetEndPoint?(%d) R(%.2f) X(%.2f) Y(%.2f) Z(%.2f) A(%.2f)\n",of_endpoint, R, X, Y, Z, A);
    // TODO-eric if R going to show ?
//#if (TRACE != 0)
//    if(_dt == 0){
//        /* prepare header for gnuplot */
//        DPS ("%11s%15s%15s%15s\n", "#dt", "x", "y", "z");
//    }
//    DPS("%11u%15.5f%15.5f%15.5f\n", _dt, xyz.tran.x, xyz.tran.y, xyz.tran.z);
//    _dt+=1;
//#endif // (TRACE != 0)
#if (TRACE != 1)
    if( of_endpoint != 1) {

    }
#endif
    pos.tran = xyz.tran;
    pos.a = abc.tran.x;
    pos.b = abc.tran.y;
    pos.c = abc.tran.z;
    pos.u = uvw.tran.x;
    pos.v = uvw.tran.y;
    pos.w = uvw.tran.z;
//    DP ("GetEndPoint?(%d) tc->id %d MotionType %d X(%.2f) Y(%.2f) Z(%.2f) A(%.2f)\n",
//    		of_endpoint,tc->id,tc->motion_type, pos.tran.x,
//    		pos.tran.y, pos.tran.z, pos.a);
    return pos;
}


/*!
 * \subsection TC queue functions
 * These following functions implement the motion queue that
 * is fed by tpAddLine/tpAddCircle and consumed by tpRunCycle.
 * They have been fully working for a long time and a wise programmer
 * won't mess with them.
 */


/*! tcqCreate() function
 *
 * \brief Creates a new queue for TC elements.
 *
 * This function creates a new queue for TC elements. 
 * It gets called by tpCreate()
 * 
 * @param    tcq       pointer to the new TC_QUEUE_STRUCT
 * @param	 _size	   size of the new queue
 * @param	 tcSpace   holds the space allocated for the new queue, allocated in motion.c
 *
 * @return	 int	   returns success or failure
 */   
int tcqCreate(TC_QUEUE_STRUCT * tcq, int _size, TC_STRUCT * tcSpace)
{
    if (_size <= 0 || 0 == tcq) {
	return -1;
    } else {
	tcq->queue = tcSpace;
	tcq->size = _size;
	tcq->_len = 0;
	tcq->start = tcq->end = 0;
	tcq->allFull = 0;

	if (0 == tcq->queue) {
	    return -1;
	}
	return 0;
    }
}

/*! tcqDelete() function
 *
 * \brief Deletes a queue holding TC elements.
 *
 * This function creates deletes a queue. It doesn't free the space
 * only throws the pointer away. 
 * It gets called by tpDelete() 
 * \todo FIXME, it seems tpDelete() is gone, and this function isn't used.
 * 
 * @param    tcq       pointer to the TC_QUEUE_STRUCT
 *
 * @return	 int	   returns success
 */   
int tcqDelete(TC_QUEUE_STRUCT * tcq)
{
    if (0 != tcq && 0 != tcq->queue) {
	/* free(tcq->queue); */
	tcq->queue = 0;
    }

    return 0;
}

/*! tcqInit() function
 *
 * \brief Initializes a queue with TC elements.
 *
 * This function initializes a queue with TC elements. 
 * It gets called by tpClear() and  
 * 	  	   		  by tpRunCycle() when we are aborting
 * 
 * @param    tcq       pointer to the TC_QUEUE_STRUCT
 *
 * @return	 int	   returns success or failure (if no tcq found)
 */
int tcqInit(TC_QUEUE_STRUCT * tcq)
{
#if (TRACE != 0)
    if(!dptrace){
        dptrace = fopen("tc.log","w");
        fprintf(stderr,"tc.c dptrace not NULL \n");
    }
#endif

    if (0 == tcq) {
	return -1;
    }

    tcq->_len = 0;
    tcq->start = tcq->end = 0;
    tcq->allFull = 0;


    return 0;
}

/*! tcqPut() function
 *
 * \brief puts a TC element at the end of the queue
 *
 * This function adds a tc element at the end of the queue. 
 * It gets called by tpAddLine() and tpAddCircle()
 * 
 * @param    tcq       pointer to the new TC_QUEUE_STRUCT
 * @param	 tc        the new TC element to be added
 *
 * @return	 int	   returns success or failure
 */   
int tcqPut(TC_QUEUE_STRUCT * tcq, TC_STRUCT tc)
{
    /* check for initialized */
    if (0 == tcq || 0 == tcq->queue) {
	    return -1;
    }

    /* check for allFull, so we don't overflow the queue */
    if (tcq->allFull) {
	    return -1;
    }

    /* add it */
    tcq->queue[tcq->end] = tc;
    tcq->_len++;

    /* update end ptr, modulo size of queue */
    tcq->end = (tcq->end + 1) % tcq->size;

    /* set allFull flag if we're really full */
    if (tcq->end == tcq->start) {
	tcq->allFull = 1;
    }

    return 0;
}

/*! tcqRemove() function
 *
 * \brief removes n items from the queue
 *
 * This function removes the first n items from the queue,
 * after checking that they can be removed 
 * (queue initialized, queue not empty, enough elements in it) 
 * Function gets called by tpRunCycle() with n=1
 * \todo FIXME: Optimize the code to remove only 1 element, might speed it up
 * 
 * @param    tcq       pointer to the new TC_QUEUE_STRUCT
 * @param	 n         the number of TC elements to be removed
 *
 * @return	 int	   returns success or failure
 */   
int tcqRemove(TC_QUEUE_STRUCT * tcq, int n)
{
    int i;
    if (n <= 0) {
	    return 0;		/* okay to remove 0 or fewer */
    }

    if ((0 == tcq) || (0 == tcq->queue) ||	/* not initialized */
	((tcq->start == tcq->end) && !tcq->allFull) ||	/* empty queue */
	(n > tcq->_len)) {	/* too many requested */
	    return -1;
    }
    /* if NURBS ?*/
    for(i=tcq->start;i<(tcq->start+n);i++){

        if(tcq->queue[i].motion_type == TC_NURBS) {
            //fprintf(stderr,"Remove TCNURBS PARAM\n");
            free(tcq->queue[i].nurbs_block.knots_ptr);
            free(tcq->queue[i].nurbs_block.ctrl_pts_ptr);
            free(tcq->queue[i].nurbs_block.N);

        }
    }
    /* update start ptr and reset allFull flag and len */
    tcq->start = (tcq->start + n) % tcq->size;
    tcq->allFull = 0;
    tcq->_len -= n;

    return 0;
}

/*! tcqLen() function
 *
 * \brief returns the number of elements in the queue
 *
 * Function gets called by tpSetVScale(), tpAddLine(), tpAddCircle()
 * 
 * @param    tcq       pointer to the TC_QUEUE_STRUCT
 *
 * @return	 int	   returns number of elements
 */   
int tcqLen(TC_QUEUE_STRUCT * tcq)
{
    if (0 == tcq) {
	    return -1;
    }

    return tcq->_len;
}

/*! tcqItem() function
 *
 * \brief gets the n-th TC element in the queue, without removing it
 *
 * Function gets called by tpSetVScale(), tpRunCycle(), tpIsPaused()
 * 
 * @param    tcq       pointer to the TC_QUEUE_STRUCT
 *
 * @return	 TC_STRUCT returns the TC elements
 */   
TC_STRUCT *tcqItem(TC_QUEUE_STRUCT * tcq, int n, long period)
{
    TC_STRUCT *t;
    if ((0 == tcq) || (0 == tcq->queue) ||	/* not initialized */
	(n < 0) || (n >= tcq->_len)) {	/* n too large */
	return (TC_STRUCT *) 0;
    }
    t = &(tcq->queue[(tcq->start + n) % tcq->size]);
    return t;
}

/*! 
 * \def TC_QUEUE_MARGIN
 * sets up a margin at the end of the queue, to reduce effects of race conditions
 */
#define TC_QUEUE_MARGIN 10

/*! tcqFull() function
 *
 * \brief get the full status of the queue 
 * Function returns full if the count is closer to the end of the queue than TC_QUEUE_MARGIN
 *
 * Function called by update_status() in control.c 
 * 
 * @param    tcq       pointer to the TC_QUEUE_STRUCT
 *
 * @return	 int       returns status (0==not full, 1==full)
 */   
int tcqFull(TC_QUEUE_STRUCT * tcq)
{
    if (0 == tcq) {
	   return 1;		/* null queue is full, for safety */
    }

    /* call the queue full if the length is into the margin, so reduce the
       effect of a race condition where the appending process may not see the 
       full status immediately and send another motion */

    if (tcq->size <= TC_QUEUE_MARGIN) {
	/* no margin available, so full means really all full */
	    return tcq->allFull;
    }

    if (tcq->_len >= tcq->size - TC_QUEUE_MARGIN) {
	/* we're into the margin, so call it full */
	    return 1;
    }

    /* we're not into the margin */
    return 0;
}

