/*
 * Solve 2D PSV forward problem by finite-differences
 *
 * Daniel Koehn
 * Kiel, 26/04/2016
 */

#include "fd.h"

void FD_PSV()
{

        /* global variables */
        extern int MYID, FDORDER, NX, NY, NT, L, READMOD, QUELLART, QUELLTYP, ORDER_SPIKE, RUN_MULTIPLE_SHOTS, TIME_FILT, ORDER, READREC;
        extern int LOG, SEISMO, N_STREAMER, FW, NXG, NYG, IENDX, IENDY, NTDTINV, IDXI, IDYI, NXNYI, INV_STF, DTINV, SNAP, SNAP_SHOT;
        extern int WRITE_STF;
        extern float FC_SPIKE_1, FC_SPIKE_2, FC, FC_START, TIME, DT;
        extern char LOG_FILE[STRING_SIZE], MFILE[STRING_SIZE];
        extern FILE *FP;
        extern int COLOR, MYID_SHOT, NCOLORS, NSHOT1, NSHOT2, NSHOTS;
        extern MPI_Comm SHOT_COMM;

        /* local variables */
        int ns, nseismograms = 0, nt, nd, fdo3, j, i, iter, h, hin, iter_true, SHOTINC, s = 0;
        int buffsize, swstestshot, ntr = 0, ntr_loc = 0, ntr_glob = 0, nsrc = 0, nsrc_loc = 0, nsrc_glob = 0, ishot, nshots = 0, itestshot;
        int ishot1, ishot2;

        char *buff_addr, ext[10], *fileinp;

        double time1, time2, time7, time8, time_av_v_update = 0.0, time_av_s_update = 0.0, time_av_v_exchange = 0.0, time_av_s_exchange = 0.0, time_av_timestep = 0.0;

        float **taper_coeff, *epst1, *hc = NULL, **Ws, **Wr;
        int *DTINV_help;

        MPI_Request *req_send, *req_rec;
        MPI_Status *send_statuses, *rec_statuses;

        if (MYID_SHOT == 0)
        {
                time1 = MPI_Wtime();
                clock();
        }

        printf("In FD_PSV MYID = %d, COLOR =%d, MYID_SHOT = %d \n", MYID, COLOR, MYID_SHOT);

        /* open log-file (each PE is using different file) */
        /*	fp=stdout; */
        sprintf(ext, ".%i", MYID);
        strcat(LOG_FILE, ext);

        if ((MYID == 0) && (LOG == 1))
                FP = stdout;
        else
                FP = fopen(LOG_FILE, "w");
        fprintf(FP, " This is the log-file generated by PE %d \n\n", MYID);

        /* ----------------------- */
        /* define FD grid geometry */
        /* ----------------------- */

        /* domain decomposition */
        initproc();

        MPI_Barrier(MPI_COMM_WORLD);
        printf("In FD_PSV (62) MYID = %d, COLOR =%d, MYID_SHOT = %d \n", MYID, COLOR, MYID_SHOT);
        MPI_Barrier(MPI_COMM_WORLD);

        NT = iround(TIME / DT); /* number of timesteps */

        /* output of parameters to log-file or stdout */
        if (MYID == 0)
                write_par(FP);

        /* NXG, NYG denote size of the entire (global) grid */
        NXG = NX;
        NYG = NY;

        /* In the following, NX and NY denote size of the local grid ! */
        NX = IENDX;
        NY = IENDY;

        NTDTINV = ceil((float)NT / (float)DTINV); /* round towards next higher integer value */

        /* save every IDXI and IDYI spatial point during the forward modelling */
        IDXI = 1;
        IDYI = 1;

        NXNYI = (NX / IDXI) * (NY / IDYI);
        SHOTINC = 1;
        INV_STF = 0;

        /* define data structures for PSV problem */
        struct wavePSV;
        struct wavePSV_PML;
        struct matPSV;
        struct mpiPSV;
        struct fwiPSV;
        struct seisPSV;
        struct seisPSVfwi;
        struct acq;

        nd = FDORDER / 2 + 1;
        fdo3 = 2 * nd;
        buffsize = 2.0 * 2.0 * fdo3 * (NX + NY) * sizeof(MPI_FLOAT);

        MPI_Barrier(MPI_COMM_WORLD);
        printf("In FD_PSV (101) MYID = %d, COLOR =%d, MYID_SHOT = %d \n", MYID, COLOR, MYID_SHOT);
        MPI_Barrier(MPI_COMM_WORLD);

        /* allocate buffer for buffering messages */
        buff_addr = malloc(buffsize);
        if (!buff_addr)
                err("allocation failure for buffer for MPI_Bsend !");
        MPI_Buffer_attach(buff_addr, buffsize);

        /* allocation for request and status arrays */
        req_send = (MPI_Request *)malloc(REQUEST_COUNT * sizeof(MPI_Request));
        req_rec = (MPI_Request *)malloc(REQUEST_COUNT * sizeof(MPI_Request));
        send_statuses = (MPI_Status *)malloc(REQUEST_COUNT * sizeof(MPI_Status));
        rec_statuses = (MPI_Status *)malloc(REQUEST_COUNT * sizeof(MPI_Status));

        /* --------- add different modules here ------------------------ */
        ns = NT; /* in a FWI one has to keep all samples of the forward modeled data
	at the receiver positions to calculate the adjoint sources and to do 
	the backpropagation; look at function saveseis_glob.c to see that every
	NDT sample for the forward modeled wavefield is written to su files*/

        if (SEISMO && (READREC != 2))
        {

                acq.recpos = receiver(FP, &ntr, ishot);
                acq.recswitch = ivector(1, ntr);
                acq.recpos_loc = splitrec(acq.recpos, &ntr_loc, ntr, acq.recswitch);
                ntr_glob = ntr;
                ntr = ntr_loc;

                if (N_STREAMER > 0)
                {
                        free_imatrix(acq.recpos, 1, 3, 1, ntr_glob);
                        if (ntr > 0)
                                free_imatrix(acq.recpos_loc, 1, 3, 1, ntr);
                        free_ivector(acq.recswitch, 1, ntr_glob);
                }
        }

        if ((N_STREAMER == 0) && (READREC != 2))
        {

                /* Memory for seismic data */
                alloc_seisPSV(ntr, ns, &seisPSV);

                /* Memory for full data seismograms */
                alloc_seisPSVfull(&seisPSV, ntr_glob);
        }

        /* estimate memory requirement of the variables in megabytes*/

        switch (SEISMO)
        {
        case 1: /* particle velocities only */
                nseismograms = 2;
                break;
        case 2: /* pressure only */
                nseismograms = 1;
                break;
        case 3: /* curl and div only */
                nseismograms = 2;
                break;
        case 4: /* everything */
                nseismograms = 5;
                break;
        }

        /* calculate memory requirements for PSV forward problem */
        mem_PSV(nseismograms, ntr, ns, fdo3, nd, buffsize);

        /* allocate memory for PSV forward problem */
        alloc_PSV(&wavePSV, &wavePSV_PML);

        /* calculate damping coefficients for CPMLs (PSV problem)*/
        if (FW > 0)
        {
                PML_pro(wavePSV_PML.d_x, wavePSV_PML.K_x, wavePSV_PML.alpha_prime_x, wavePSV_PML.a_x, wavePSV_PML.b_x, wavePSV_PML.d_x_half, wavePSV_PML.K_x_half, wavePSV_PML.alpha_prime_x_half, wavePSV_PML.a_x_half,
                        wavePSV_PML.b_x_half, wavePSV_PML.d_y, wavePSV_PML.K_y, wavePSV_PML.alpha_prime_y, wavePSV_PML.a_y, wavePSV_PML.b_y, wavePSV_PML.d_y_half, wavePSV_PML.K_y_half, wavePSV_PML.alpha_prime_y_half,
                        wavePSV_PML.a_y_half, wavePSV_PML.b_y_half);
        }

        /* allocate memory for PSV material parameters */
        alloc_matPSV(&matPSV);

        /* allocate memory for PSV MPI variables */
        alloc_mpiPSV(&mpiPSV);

        /* memory for source position definition */
        acq.srcpos1 = fmatrix(1, 8, 1, 1);

        fprintf(FP, " ... memory allocation for PE %d was successfull.\n\n", MYID);

        /* Holberg coefficients for FD operators*/
        hc = holbergcoeff();

        MPI_Barrier(MPI_COMM_WORLD);

        /* Reading source positions from SOURCE_FILE */
        acq.srcpos = sources(&nsrc);
        nsrc_glob = nsrc;
        NSHOTS = nsrc;

        /* create model grids */
        if (L)
        {
                if (READMOD)
                        readmod_visc_PSV(matPSV.prho, matPSV.ppi, matPSV.pu, matPSV.ptaus, matPSV.ptaup, matPSV.peta);
                else
                        model(matPSV.prho, matPSV.ppi, matPSV.pu, matPSV.ptaus, matPSV.ptaup, matPSV.peta);
        }
        else
        {
                if (READMOD)
                        readmod_elastic_PSV(matPSV.prho, matPSV.ppi, matPSV.pu);
                else
                        model_elastic(matPSV.prho, matPSV.ppi, matPSV.pu);
        }

        /* check if the FD run will be stable and free of numerical dispersion */
        if (L)
        {
                checkfd_ssg_visc(FP, matPSV.prho, matPSV.ppi, matPSV.pu, matPSV.ptaus, matPSV.ptaup, matPSV.peta, hc);
        }
        else
        {
                checkfd_ssg_elastic(FP, matPSV.prho, matPSV.ppi, matPSV.pu, hc);
        }

        /* For the calculation of the material parameters between gridpoints
        they have to be averaged. For this, values lying at 0 and NX+1,
        for example, are required on the local grid. These are now copied from the
        neighbouring grids */
        if (L)
        {
                matcopy_PSV(matPSV.prho, matPSV.ppi, matPSV.pu, matPSV.ptaus, matPSV.ptaup);
        }
        else
        {
                matcopy_elastic_PSV(matPSV.prho, matPSV.ppi, matPSV.pu);
        }

        MPI_Barrier(MPI_COMM_WORLD);

        av_mue(matPSV.pu, matPSV.puipjp, matPSV.prho);
        av_rho(matPSV.prho, matPSV.prip, matPSV.prjp);
        if (L)
                av_tau(matPSV.ptaus, matPSV.ptausipjp);

        /* Preparing memory variables for update_s (viscoelastic) */
        if (L)
                prepare_update_s_visc_PSV(matPSV.etajm, matPSV.etaip, matPSV.peta, matPSV.fipjp, matPSV.pu, matPSV.puipjp, matPSV.ppi, matPSV.prho, matPSV.ptaus, matPSV.ptaup, matPSV.ptausipjp, matPSV.f, matPSV.g,
                                          matPSV.bip, matPSV.bjm, matPSV.cip, matPSV.cjm, matPSV.dip, matPSV.d, matPSV.e);

        if (RUN_MULTIPLE_SHOTS)
                nshots = nsrc;
        else
                nshots = 1;

        ishot1 = 1;
        ishot2 = nshots;

        if (SNAP)
        {
                ishot1 = SNAP_SHOT;
                ishot2 = SNAP_SHOT;
        }

        // dividing shots into NCOLORS groups
        NSHOT1 = (1 + (NSHOTS-1) / NCOLORS) * COLOR + 1;
        NSHOT2 = min(NSHOT1 + (NSHOTS-1) / NCOLORS, NSHOTS);
        

	fprintf(FP, "Before shots PE %d was successfull, NSHOT1 %d, NSHOT2 %d, NSHOTS %d,\n\n", MYID, NSHOT1, NSHOT2, NSHOTS);
        for (ishot = NSHOT1; ishot <= NSHOT2; ishot += SHOTINC)
        {
		fprintf(FP, "Before shots PE %d was successfull.\n\n", MYID);
                /*for (ishot=1;ishot<=1;ishot+=1){*/
                /*if(ishot!=10 && ishot!=11 && ishot!=12 && ishot!=13 && ishot!=14 && ishot!=29){*/

                if ((N_STREAMER > 0) || (READREC == 2))
                {

                        if (SEISMO)
                        {
                                acq.recpos = receiver(FP, &ntr, ishot);
                                acq.recswitch = ivector(1, ntr);
                                acq.recpos_loc = splitrec(acq.recpos, &ntr_loc, ntr, acq.recswitch);
                                ntr_glob = ntr;
                                ntr = ntr_loc;
                        }

                        /* Memory for seismic data */
                        alloc_seisPSV(ntr, ns, &seisPSV);

                        /* Memory for full data seismograms */
                        alloc_seisPSVfull(&seisPSV, ntr_glob);
                }

                for (nt = 1; nt <= 8; nt++)
                        acq.srcpos1[nt][1] = acq.srcpos[nt][ishot];

                /* set QUELLTYP for each shot */
                QUELLTYP = acq.srcpos[8][ishot];

                if (RUN_MULTIPLE_SHOTS)
                {

                        /* find this single source positions on subdomains */
                        if (nsrc_loc > 0)
                                free_matrix(acq.srcpos_loc, 1, 8, 1, 1);
                        acq.srcpos_loc = splitsrc(acq.srcpos1, &nsrc_loc, 1);
                }
                else
                {
                        /* Distribute multiple source positions on subdomains */
                        acq.srcpos_loc = splitsrc(acq.srcpos, &nsrc_loc, nsrc);
                }

                MPI_Barrier(SHOT_COMM);
		printf("\n\n\BEFORE STARTING SIMULATION\n\n");
                /*==================================================================================
                   Starting simulation (forward model)
		==================================================================================*/

                /* calculate wavelet for each source point */
                acq.signals = NULL;
                acq.signals = wavelet(acq.srcpos_loc, nsrc_loc, ishot);

                /* time domain filtering of spike source signal */
                if (nsrc_loc)
                {
                        if (QUELLART == 6)
                        {

                                apply_tdfilt(acq.signals, nsrc_loc, ns, ORDER_SPIKE, FC_SPIKE_2, FC_SPIKE_1);
                        }
                }

                /* output source signal */
                if (RUN_MULTIPLE_SHOTS)
                {

                        if ((nsrc_loc > 0) && (WRITE_STF))
                        {
                                char source_signal_file[STRING_SIZE];
                                sprintf(source_signal_file, "%s_source_signal.%d.su.shot%d", MFILE, MYID, ishot);
                                fprintf(stdout, "\n PE %d outputs source time function in SU format to %s \n ", MYID, source_signal_file);
                                output_source_signal(fopen(source_signal_file, "w"), acq.signals, NT, 1);
                        }

                        MPI_Barrier(SHOT_COMM);
                }

                /* solve forward problem */
                psv(&wavePSV, &wavePSV_PML, &matPSV, &fwiPSV, &mpiPSV, &seisPSV, &seisPSVfwi, &acq, hc, ishot, nshots, nsrc_loc, ns, ntr, Ws, Wr, hin, DTINV_help, 0, req_send, req_rec);

                /* output of forward model seismograms */
                outseis_PSVfor(&seisPSV, acq.recswitch, acq.recpos, acq.recpos_loc, ntr_glob, acq.srcpos, ishot, ns, iter, FP);

                if ((N_STREAMER > 0) || (READREC == 2))
                {

                        if (SEISMO)
                                free_imatrix(acq.recpos, 1, 3, 1, ntr_glob);

                        if ((ntr > 0) && (SEISMO))
                        {

                                free_imatrix(acq.recpos_loc, 1, 3, 1, ntr);
                                acq.recpos_loc = NULL;

                                switch (SEISMO)
                                {
                                case 1: /* particle velocities only */
                                        free_matrix(seisPSV.sectionvx, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectionvy, 1, ntr, 1, ns);
                                        seisPSV.sectionvx = NULL;
                                        seisPSV.sectionvy = NULL;
                                        break;
                                case 2: /* pressure only */
                                        free_matrix(seisPSV.sectionp, 1, ntr, 1, ns);
                                        break;
                                case 3: /* curl and div only */
                                        free_matrix(seisPSV.sectioncurl, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectiondiv, 1, ntr, 1, ns);
                                        break;
                                case 4: /* everything */
                                        free_matrix(seisPSV.sectionvx, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectionvy, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectionp, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectioncurl, 1, ntr, 1, ns);
                                        free_matrix(seisPSV.sectiondiv, 1, ntr, 1, ns);
                                        break;
                                }
                        }

                        ntr = 0;
                        ntr_glob = 0;
                }

                nsrc_loc = 0;
        //} /* exclude shots */
        } /* end of loop over shots */

        MPI_Barrier(MPI_COMM_WORLD);
        printf("Loop over shots halas \n");
        printf("In FD_PSV (after shots) MYID = %d, COLOR =%d, MYID_SHOT = %d \n", MYID, COLOR, MYID_SHOT);
        MPI_Barrier(MPI_COMM_WORLD);

        /* deallocate memory for PSV forward problem */
        dealloc_PSV(&wavePSV, &wavePSV_PML);

        free_matrix(matPSV.prho, -nd + 1, NY + nd, -nd + 1, NX + nd);
        free_matrix(matPSV.prip, -nd + 1, NY + nd, -nd + 1, NX + nd);
        free_matrix(matPSV.prjp, -nd + 1, NY + nd, -nd + 1, NX + nd);

        free_matrix(matPSV.ppi, -nd + 1, NY + nd, -nd + 1, NX + nd);
        free_matrix(matPSV.pu, -nd + 1, NY + nd, -nd + 1, NX + nd);
        free_matrix(matPSV.puipjp, -nd + 1, NY + nd, -nd + 1, NX + nd);

        free_matrix(mpiPSV.bufferlef_to_rig, 1, NY, 1, fdo3);
        free_matrix(mpiPSV.bufferrig_to_lef, 1, NY, 1, fdo3);
        free_matrix(mpiPSV.buffertop_to_bot, 1, NX, 1, fdo3);
        free_matrix(mpiPSV.bufferbot_to_top, 1, NX, 1, fdo3);

        free_vector(hc, 0, 6);

        if (nsrc_loc > 0)
        {
                free_matrix(acq.signals, 1, nsrc_loc, 1, NT);
                free_matrix(acq.srcpos_loc, 1, 8, 1, nsrc_loc);
                free_matrix(acq.srcpos_loc_back, 1, 6, 1, nsrc_loc);
        }

        /* free memory for global source positions */
        free_matrix(acq.srcpos, 1, 8, 1, nsrc);

        /* free memory for source position definition */
        free_matrix(acq.srcpos1, 1, 8, 1, 1);

        if ((N_STREAMER == 0) || (READREC != 2))
        {

                if (SEISMO)
                        free_imatrix(acq.recpos, 1, 3, 1, ntr_glob);

                if ((ntr > 0) && (SEISMO))
                {

                        free_imatrix(acq.recpos_loc, 1, 3, 1, ntr);
                        acq.recpos_loc = NULL;

                        switch (SEISMO)
                        {
                        case 1: /* particle velocities only */
                                free_matrix(seisPSV.sectionvx, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectionvy, 1, ntr, 1, ns);
                                seisPSV.sectionvx = NULL;
                                seisPSV.sectionvy = NULL;
                                break;
                        case 2: /* pressure only */
                                free_matrix(seisPSV.sectionp, 1, ntr, 1, ns);
                                break;
                        case 3: /* curl and div only */
                                free_matrix(seisPSV.sectioncurl, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectiondiv, 1, ntr, 1, ns);
                                break;
                        case 4: /* everything */
                                free_matrix(seisPSV.sectionvx, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectionvy, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectionp, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectioncurl, 1, ntr, 1, ns);
                                free_matrix(seisPSV.sectiondiv, 1, ntr, 1, ns);
                                break;
                        }
                }

                free_ivector(acq.recswitch, 1, ntr);

                if (SEISMO)
                {
                        free_matrix(seisPSV.fulldata, 1, ntr_glob, 1, NT);
                }

                if (SEISMO == 1)
                {
                        free_matrix(seisPSV.fulldata_vx, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_vy, 1, ntr_glob, 1, NT);
                }

                if (SEISMO == 2)
                {
                        free_matrix(seisPSV.fulldata_p, 1, ntr_glob, 1, NT);
                }

                if (SEISMO == 3)
                {
                        free_matrix(seisPSV.fulldata_curl, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_div, 1, ntr_glob, 1, NT);
                }

                if (SEISMO == 4)
                {
                        free_matrix(seisPSV.fulldata_vx, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_vy, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_p, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_curl, 1, ntr_glob, 1, NT);
                        free_matrix(seisPSV.fulldata_div, 1, ntr_glob, 1, NT);
                }
        }

        /* free memory for viscoelastic modeling variables */
        if (L)
        {
                free_matrix(matPSV.ptaus, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_matrix(matPSV.ptausipjp, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_matrix(matPSV.ptaup, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_vector(matPSV.peta, 1, L);
                free_vector(matPSV.etaip, 1, L);
                free_vector(matPSV.etajm, 1, L);
                free_vector(matPSV.bip, 1, L);
                free_vector(matPSV.bjm, 1, L);
                free_vector(matPSV.cip, 1, L);
                free_vector(matPSV.cjm, 1, L);
                free_matrix(matPSV.f, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_matrix(matPSV.g, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_matrix(matPSV.fipjp, -nd + 1, NY + nd, -nd + 1, NX + nd);
                free_f3tensor(matPSV.dip, -nd + 1, NY + nd, -nd + 1, NX + nd, 1, L);
                free_f3tensor(matPSV.d, -nd + 1, NY + nd, -nd + 1, NX + nd, 1, L);
                free_f3tensor(matPSV.e, -nd + 1, NY + nd, -nd + 1, NX + nd, 1, L);
        }
  
        /* de-allocate buffer for messages */
        MPI_Buffer_detach(buff_addr, &buffsize);

        MPI_Barrier(MPI_COMM_WORLD);

        if (MYID == 0)
        {
                fprintf(FP, "\n **Info from main (written by PE %d): \n", MYID);
                fprintf(FP, " CPU time of program per PE: %li seconds.\n", clock() / CLOCKS_PER_SEC);
                time8 = MPI_Wtime();
                fprintf(FP, " Total real time of program: %4.2f seconds.\n", time8 - time1);
                time_av_v_update = time_av_v_update / (double)NT;
                time_av_s_update = time_av_s_update / (double)NT;
                time_av_v_exchange = time_av_v_exchange / (double)NT;
                time_av_s_exchange = time_av_s_exchange / (double)NT;
                time_av_timestep = time_av_timestep / (double)NT;
                /* fprintf(FP," Average times for \n");
	              fprintf(FP," velocity update:  \t %5.3f seconds  \n",time_av_v_update);
	              fprintf(FP," stress update:  \t %5.3f seconds  \n",time_av_s_update);
	              fprintf(FP," velocity exchange:  \t %5.3f seconds  \n",time_av_v_exchange);
	              fprintf(FP," stress exchange:  \t %5.3f seconds  \n",time_av_s_exchange);
	              fprintf(FP," timestep:  \t %5.3f seconds  \n",time_av_timestep);*/
        }

        fclose(FP);
}

