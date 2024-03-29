// Copyright 2021 - The University of Michigan
// Full license can be found in License.md
//////////////////////////////////////////////////////////////////////////////
//
//  Michigan Orbit Analyst Tool
//
//
//  Change Log:
//      |   Developer   |       Date    |   SCR     |   Notes
//      | --------------|---------------|-----------|--------------------------
//      | J. Getchius   | 05/09/2015    |   ---     | Initial Implementation
//      ! A. Ridley     | 03/09/2021    |           | Rewrite begins
///////////////////////////////////////////////////////////////////////////////

#include "options.h"
#include "propagator.h" 
#include "moat_prototype.h"
#include "kalman_9state_new_with_tau.h" 

int nProcs;
int iProc;

int main(int argc, char * argv[]) {

    OPTIONS_T OPTIONS;
    PARAMS_T PARAMS;
    GROUND_STATION_T GROUND_STATION;
    char filename_input[256];
    char filename_input_raw[300];
    int ierr;
    CONSTELLATION_T *CONSTELLATION = malloc(sizeof(CONSTELLATION_T));

    ierr = MPI_Init(&argc, &argv);
     
    /* find out MY process ID, and how many processes were started. */
      
    ierr = MPI_Comm_rank(MPI_COMM_WORLD, &iProc);
    ierr = MPI_Comm_size(MPI_COMM_WORLD, &nProcs);

    strcpy(filename_input, "./input/main_input/");
    strcat(filename_input, argv[1]);
    strcpy(filename_input_raw, "");
    strcat(filename_input_raw, argv[1]);

    int iDebugLevel = -1;

    if (argc > 2 && iProc == 0) iDebugLevel = atoi(argv[2]);

    //  Load Options
    if (iDebugLevel >= 0)
      printf("\n- Loading options... (iProc %d)\n", iProc);

    load_options(&OPTIONS,
		 filename_input,
		 iProc,
		 nProcs,
		 iDebugLevel,
		 filename_input_raw);

    // -------------------------------
    // Set up SPICE
    // -------------------------------

    if (iDebugLevel >= 0 && iProc == 0)
      printf("\n- Loading spice... (iProc %d)\n", iProc);

    if (iDebugLevel >= 1){
      printf("-- (main) Spice eop: %s\n", OPTIONS.eop_file);
      printf("-- (main) Spice planet_ephem_: %s\n", OPTIONS.planet_ephem_file);
      printf("-- (main) Spice earth_binary: %s\n", OPTIONS.earth_binary_pck);
    }
    furnsh_c(OPTIONS.eop_file);
    furnsh_c(OPTIONS.planet_ephem_file);
    furnsh_c(OPTIONS.earth_binary_pck);

    // -------------------------------
    // Load Params
    // -------------------------------

    if (iDebugLevel >= 0)
      printf("\n- Loading parameters... (iProc %d)\n", iProc);

    int degree = (int)(OPTIONS.degree);
    load_params(&PARAMS,
		iDebugLevel,
		OPTIONS.earth_fixed_frame,
		OPTIONS.use_ap_hist,
		iProc,
		OPTIONS.path_to_spice,
		degree,
		OPTIONS.gravity_map,
		OPTIONS.include_earth_pressure);

    // Note about quaternions:
    // ftp://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/q2m_c.html

    // -------------------------------
    //  Build Constellation
    // -------------------------------

    if (iDebugLevel >= 0)
      printf("\n- Initializing Constellation... (iProc: %d)\n", iProc);

    initialize_constellation( CONSTELLATION,
			      &OPTIONS,
			      &PARAMS,
			      &GROUND_STATION,
			      iDebugLevel,
			      iProc,
			      nProcs);

    //  Create a 3d map of the gravitational potential derivatives
    //  dUdr, dUdlat, and dUdlong. These are then used in
    //  compute_gravity to compute the acceleration due to the Earth
    //  gravity

    // AJR - I don't know why this is commented out - AJR

    /* if (OPTIONS.gravity_map == 1){ */
    /*   gravity_map(CONSTELLATION, PARAMS.EARTH.GRAVITY, degree, iProc); */
    /*   printf("Done building the 3D gravity map.\n"); */
    /* } */

    // -------------------------------
    // Kalman Filter Stuff
    // -------------------------------

    // if 0 then it uses the classical propagation like in the
    // previous veresions. If set to 1 then it uses Kalman Filter,
    // which means observations are needed as inputs
    if ( OPTIONS.use_kalman == 1 ){  

      MEAS_T MEAS;
      KALMAN_T KF;

      char filename_meas[1000];
      char *line = NULL;
      size_t len = 0;

      KF.sc = CONSTELLATION->spacecraft[0][0]; // !!!!!!! all sc

      KF.fp_kalman_init =
	fopen(CONSTELLATION->spacecraft[0][0].filename_kalman_init, "r");

      getline(&line, &len, KF.fp_kalman_init);
      strtok(line, "\n");  strtok(line, "\r");

      strcpy(filename_meas, "");
      sscanf(line, "%s", filename_meas);
      getline(&line, &len, KF.fp_kalman_init);

      MEAS.fp_meas = fopen(filename_meas, "r");

      str2et_c(OPTIONS.initial_epoch, &OPTIONS.et_initial_epoch);

      str2et_c(OPTIONS.final_epoch, &OPTIONS.et_final_epoch);
      double min_end_time;
      min_end_time = OPTIONS.et_final_epoch;

      while ((feof(MEAS.fp_meas) == 0) &&
	     (KF.sc.et  <= OPTIONS.et_final_epoch)){// !!!!!! all sc

	if (iProc == 0)
	  print_progress_kalman( min_end_time,
				 KF.sc.et ,
				 OPTIONS.et_initial_epoch,
				 iProc,
				 OPTIONS.nb_gps );

	kalman_filt( &MEAS,
		     &KF,
		     &PARAMS,
		     &OPTIONS,
		     &GROUND_STATION,
		     CONSTELLATION,
		     iProc,
		     iDebugLevel,
		     nProcs);
	
	KF.sc.et_next_time_step =
	  OPTIONS.et_initial_epoch +
	  (int)((MEAS.et - OPTIONS.et_initial_epoch)/OPTIONS.dt) * OPTIONS.dt +
	  OPTIONS.dt;

	if (MEAS.et > OPTIONS.et_final_epoch) break;
      }

      fclose(KF.fp_kalman_init);
      fclose(KF.fp_kalman);
      fclose(KF.fp_kalman_meas);
      fclose(MEAS.fp_meas);
      if (iProc == 0) printf("\n- Done running the Kalman filter.\n");

    } else {

      // -------------------------------
      // Generate the Ephemerides
      // -------------------------------

      if (iDebugLevel >= 0)
	printf("\n- Generating the ephemerides... (iProc: %d)\n", iProc);

      generate_ephemerides( CONSTELLATION,
			    &OPTIONS,
			    &PARAMS,
			    &GROUND_STATION,
			    iProc,
			    nProcs,
			    iDebugLevel);

      // Notify Exit
      if (iProc == 0) 
    	printf("Done propagating the spacecraft.\n");

    }

    MPI_Barrier(MPI_COMM_WORLD);
    free(CONSTELLATION);
    
    ierr = MPI_Finalize();

    return 0;
}

