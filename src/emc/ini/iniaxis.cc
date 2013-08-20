/********************************************************************
* Description: iniaxis.cc
*   INI file initialization routines for joint/axis NML
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
********************************************************************/

#include <unistd.h>
#include <stdio.h>		// NULL
#include <stdlib.h>		// atol(), _itoa()
#include <string.h>		// strcmp()
#include <ctype.h>		// isdigit()
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <libintl.h>

#include "emc.hh"
#include "rcs_print.hh"
#include "emcIniFile.hh"
#include "iniaxis.hh"		// these decls
#include "emcglb.h"		// EMC_DEBUG
#include "emccfg.h"		// default values for globals
#include "../../libnml/inifile/inifile.hh"
#define _(s) gettext(s)

/*
  loadAxis(int axis)

  Loads ini file params for axis, axis = X, Y, Z, A, B, C, U, V, W 

  TYPE <LINEAR ANGULAR>        type of axis
  UNITS <float>                units per mm or deg
  MAX_VELOCITY <float>         max vel for axis
  MAX_ACCELERATION <float>     max accel for axis
  MIN_LIMIT <float>            minimum soft position limit
  MAX_LIMIT <float>            maximum soft position limit
  FERROR <float>               maximum following error, scaled to max vel
  MIN_FERROR <float>           minimum following error

  calls:

  emcAxisSetMinPositionLimit(int axis, double limit);
  emcAxisSetMaxPositionLimit(int axis, double limit);
  emcAxisSetMaxVelocity(int axis, double vel);
  emcAxisSetMaxAcceleration(int axis, double acc);
  emcAxisLoadComp(int axis, const char * file);
  emcAxisLoadComp(int axis, const char * file);
  */

static int loadAxis(int axis, EmcIniFile *axisIniFile)
{
    char axisString[16];
    //obsolete: double units;
    double limit;
    double home;
    double maxVelocity;
    double maxAcceleration;
    double maxJerk;

    // compose string to match, axis = 0 -> AXIS_X etc.
    switch (axis) {
	case 0: sprintf(axisString, "AXIS_X");break;
	case 1: sprintf(axisString, "AXIS_Y");break;
	case 2: sprintf(axisString, "AXIS_Z");break;
	case 3: sprintf(axisString, "AXIS_A");break;
	case 4: sprintf(axisString, "AXIS_B");break;
	case 5: sprintf(axisString, "AXIS_C");break;
	case 6: sprintf(axisString, "AXIS_U");break;
	case 7: sprintf(axisString, "AXIS_V");break;
	case 8: sprintf(axisString, "AXIS_W");break;
        case 9: sprintf(axisString, "AXIS_S");break;
	default:
	    rcs_print_error("Unknown axis id: %d\n", axis);
	    assert(0); // unknown axis id
    }

    axisIniFile->EnableExceptions(EmcIniFile::ERR_CONVERSION);
    
    try {
        home = 0;
        axisIniFile->Find(&home, "HOME", axisString);
        if (0 != emcAxisSetHome(axis, home)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetHome\n");
            }
            return -1;
        };

        // set min position limit
        limit = -1e99;	                // default
        axisIniFile->Find(&limit, "MIN_LIMIT", axisString);

        if (0 != emcAxisSetMinPositionLimit(axis, limit)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetMinPositionLimit\n");
            }
            return -1;
        }

        // set max position limit
        limit = 1e99;	                // default
        axisIniFile->Find(&limit, "MAX_LIMIT", axisString);

        if (0 != emcAxisSetMaxPositionLimit(axis, limit)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetMaxPositionLimit\n");
            }
            return -1;
        }

        // set maximum velocity
        maxVelocity = DEFAULT_AXIS_MAX_VELOCITY;
        axisIniFile->Find(&maxVelocity, "MAX_VELOCITY", axisString);

        if (0 != emcAxisSetMaxVelocity(axis, maxVelocity)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetMaxVelocity\n");
            }
            return -1;
        }

        maxAcceleration = DEFAULT_AXIS_MAX_ACCELERATION;
        axisIniFile->Find(&maxAcceleration, "MAX_ACCELERATION", axisString);

        if (0 != emcAxisSetMaxAcceleration(axis, maxAcceleration)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetMaxAcceleration\n");
            }
            return -1;
        }

        maxJerk = DEFAULT_AXIS_MAX_JERK;
        axisIniFile->Find(&maxJerk, "MAX_JERK", axisString);
        if (0 != emcAxisSetMaxJerk(axis, maxJerk)) {
            if (emc_debug & EMC_DEBUG_CONFIG) {
                rcs_print_error("bad return from emcAxisSetMaxJerk\n");
            }
            return -1;
        }
                }

    catch(EmcIniFile::Exception &e){
        e.Print();
        return -1;
    }

    return 0;
}


/*
  iniAxis(int axis, const char *filename)

  Loads ini file parameters for specified axis, [0 .. AXES - 1]

  Looks for AXES in TRAJ section for how many to do, up to
  EMC_AXIS_MAX.
 */
int iniAxis(int axis, const char *filename)
{
    int axes;
    EmcIniFile axisIniFile(EmcIniFile::ERR_TAG_NOT_FOUND |
                           EmcIniFile::ERR_SECTION_NOT_FOUND |
                           EmcIniFile::ERR_CONVERSION);

    if (axisIniFile.Open(filename) == false) {
	return -1;
    }

    try {
        axisIniFile.Find(&axes, "AXES", "TRAJ");
    }

    catch(EmcIniFile::Exception &e){
        e.Print();
        return -1;
    }

    /**
     * If we set "AXES as 4" and "COORDINATES as Y Z V W",
     * we will hit "axis==7" for "V", where "axes==4".
     * In this case, the following judgment will fail.
     */
    // if (axis < 0 || axis >= axes) {
    //	// requested axis exceeds machine axes
    //	return -1;
    // }

    // load its values
    if (0 != loadAxis(axis, &axisIniFile)) {
        return -1;
    }

    return 0;
}

/*! \todo FIXME-- begin temporary insert of ini file stuff */

#define INIFILE_MIN_FLOAT_PRECISION 3
#define INIFILE_BACKUP_SUFFIX ".bak"

int iniGetFloatPrec(const char *str)
{
    const char *ptr = str;
    int prec = 0;

    // find '.', return min precision if no decimal point
    while (1) {
	if (*ptr == 0) {
	    return INIFILE_MIN_FLOAT_PRECISION;
	}
	if (*ptr == '.') {
	    break;
	}
	ptr++;
    }

    // ptr is on '.', so step over
    ptr++;

    // count number of digits until whitespace or end or non-digit
    while (1) {
	if (*ptr == 0) {
	    break;
	}
	if (!isdigit(*ptr)) {
	    break;
	}
	// else it's a digit
	prec++;
	ptr++;
    }

    return prec >
	INIFILE_MIN_FLOAT_PRECISION ? prec : INIFILE_MIN_FLOAT_PRECISION;
}

// end temporary insert of ini file stuff

/*
  dumpAxis(int axis, const char *filename, EMC_AXIS_STAT *status)

  This used to rewrite an AXIS_n section of the ini file.  Everyone
  now seems to think this is a bad idea.  It's certainly incompatible
  with template/sample configurations that should not be changed by
  the user OR the program.
 */
int dumpAxis(int axis, const char *filename, EMC_AXIS_STAT * status)
{
    return 0;
}
