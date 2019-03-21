/*------------------------------------------------------------------------
 * Calculate gradient and objective function for SH problem
 *
 *
 * D. Koehn
 * Kiel, 13.12.2017
 * ----------------------------------------------------------------------*/

#include "fd.h"


double grad_obj_sh(struct waveSH *waveSH, struct waveSH_PML *waveSH_PML, struct matSH *matSH, struct fwiSH *fwiSH, struct mpiPSV *mpiPSV, 
         struct seisSH *seisSH, struct seisSHfwi *seisSHfwi, struct acq *acq, float *hc, int iter, int nsrc, int ns, int ntr, int ntr_glob, int nsrc_glob, 
         int nsrc_loc, int ntr_loc, int nstage, float **We, float **Ws, float **Wr, float ** taper_coeff, int hin, int *DTINV_help, 
         MPI_Request * req_send, MPI_Request * req_rec){

        /* global variables */
	extern int MYID, TIME_FILT, IDX, IDY, NX, NY, NT, RUN_MULTIPLE_SHOTS, INV_STF, QUELLART;
        extern int TESTSHOT_START, TESTSHOT_END, TESTSHOT_INCR, SEISMO, EPRECOND, LNORM, READREC;
        extern int N_STREAMER, SWS_TAPER_CIRCULAR_PER_SHOT, QUELLTYPB, QUELLTYP, LOG, L;
        extern int ORDER_SPIKE, ORDER, SHOTINC, RTM_SHOT, WRITE_STF, NTDTINV;
        extern float EPSILON, FC, FC_START, FC_SPIKE_1, FC_SPIKE_2;
        extern float C_vs, C_rho, C_taus, *FL;
        extern char MFILE[STRING_SIZE];

        /* local variables */
	int i, j, l, nshots, ishot, nt, lsnap, itestshot, swstestshot;
	double L2sum, L2_tmp;
        char source_signal_file[STRING_SIZE];

	FILE *FP;

	if ((MYID==0) && (LOG==1)) FP=stdout;

	/* initialization of L2 calculation */
	(*seisSHfwi).L2=0.0;
	(*seisSHfwi).energy=0.0;

	EPSILON=0.0;  /* test step length */

	/* set gradient and preconditioning matrices 0 before next iteration*/
	init_grad((*fwiSH).waveconv_rho);
	init_grad((*fwiSH).waveconv_u);
	init_grad((*fwiSH).waveconv_ts);
	
	if(EPRECOND==4){
	   init_grad((*fwiSH).hess_mu2);
	   init_grad((*fwiSH).hess_rho2);
	   init_grad((*fwiSH).hess_ts2);
	   init_grad((*fwiSH).hess_vs2);
	   init_grad((*fwiSH).hess_rho2p);

	   init_grad((*fwiSH).hess_muts);
	   init_grad((*fwiSH).hess_murho);
	   init_grad((*fwiSH).hess_tsrho);	   	   
	}

	/* calculate FWI gradient weighting coefficients */
	init_grad_coeff(fwiSH,matSH);

	/* calculate tausl */
	for (l=1;l<=L;l++) {
	    (*fwiSH).tausl[l] = 1.0/(2.0*PI*FL[l]);
	}

	itestshot=TESTSHOT_START;
	swstestshot=0;
        SHOTINC=1;

	if (RUN_MULTIPLE_SHOTS) nshots=nsrc; else nshots=1;

	for (ishot=1;ishot<=nshots;ishot+=SHOTINC){
	/*for (ishot=1;ishot<=1;ishot+=1){*/

	/*initialize gradient matrices for each shot with zeros*/
	init_grad((*fwiSH).waveconv_u_shot);
	init_grad((*fwiSH).waveconv_rho_shot);
	init_grad((*fwiSH).waveconv_ts_shot);

	if((EPRECOND==1)||(EPRECOND==3)){
	   init_grad(Ws);
	   init_grad(Wr);
	   init_grad(We);
	}			
	  
	if((N_STREAMER>0)||(READREC==2)){

	   if (SEISMO){
	      (*acq).recpos=receiver(FP, &ntr, ishot);
	      (*acq).recswitch = ivector(1,ntr);
	      (*acq).recpos_loc = splitrec((*acq).recpos,&ntr_loc, ntr, (*acq).recswitch);
	      ntr_glob=ntr;
	      ntr=ntr_loc;
	   }

	   /* Memory for seismic data */
	   alloc_seisSH(ntr,ns,seisSH);
	   
	   /* Memory for full data seismograms */
           alloc_seisSHfull(seisSH,ntr_glob);

	   /* Memory for FWI seismic data */ 
	   alloc_seisSHfwi(ntr,ntr_glob,ns,seisSHfwi);

	}

	for (nt=1;nt<=8;nt++) (*acq).srcpos1[nt][1]=(*acq).srcpos[nt][ishot]; 

	/* set QUELLTYP for each shot */
        QUELLTYP = (*acq).srcpos[8][ishot];

		if (RUN_MULTIPLE_SHOTS){

		        /* find this single source positions on subdomains */
		        if (nsrc_loc>0) free_matrix((*acq).srcpos_loc,1,8,1,1);
		        (*acq).srcpos_loc=splitsrc((*acq).srcpos1,&nsrc_loc, 1);
		}


		else{
		        /* Distribute multiple source positions on subdomains */
		        (*acq).srcpos_loc = splitsrc((*acq).srcpos,&nsrc_loc, nsrc);
		}

	MPI_Barrier(MPI_COMM_WORLD);

	/*==================================================================================
		        Estimate source time function by Wiener deconvolution
	==================================================================================*/

	if((INV_STF)&&(iter==1)){
	  stf_sh(waveSH,waveSH_PML,matSH,fwiSH,mpiPSV,seisSH,seisSHfwi,acq,hc,ishot,nshots,nsrc_loc,nsrc,ns,ntr,ntr_glob,iter,Ws,Wr,hin,DTINV_help,req_send,req_rec);
	}
	 
	/*==================================================================================
		           Starting simulation (forward model)
	==================================================================================*/
		
	/* calculate wavelet for each source point */
	(*acq).signals=NULL;
	(*acq).signals=wavelet((*acq).srcpos_loc,nsrc_loc,ishot);

	if (nsrc_loc){if(QUELLART==6){

	   /* time domain filtering of the source signal */
	   apply_tdfilt((*acq).signals,nsrc_loc,ns,ORDER_SPIKE,FC_SPIKE_2,FC_SPIKE_1);
	   
	   }
	}

	/* time domain filtering*/
	if ((TIME_FILT)&&(INV_STF==0)){
	
	   /* time domain filtering of the source signal */
	   apply_tdfilt((*acq).signals,nsrc_loc,ns,ORDER,FC,FC_START);

	}

	/* output source signal e.g. for cross-correlation of comparison with analytical solutions */
	if(RUN_MULTIPLE_SHOTS){

		if((nsrc_loc>0) && WRITE_STF){
			   sprintf(source_signal_file,"%s_source_signal.%d.su.shot%d", MFILE, MYID,ishot);
			   fprintf(stdout,"\n PE %d outputs source time function in SU format to %s \n ", MYID, source_signal_file);
			   output_source_signal(fopen(source_signal_file,"w"),(*acq).signals,NT,1);
		}                                
		                        
		MPI_Barrier(MPI_COMM_WORLD);
	}
				                                                      
	/* solve forward problem */	
        sh(waveSH,waveSH_PML,matSH,fwiSH,mpiPSV,seisSH,seisSHfwi,acq,hc,ishot,nshots,nsrc_loc,ns,ntr,Ws,Wr,hin,DTINV_help,0,req_send,req_rec);


	/* ===============================================
	   Calculate objective function and data residuals
	   =============================================== */

	if ((ishot >= TESTSHOT_START) && (ishot <= TESTSHOT_END) && ((ishot - TESTSHOT_START) % TESTSHOT_INCR == 0)){
	    swstestshot=1;
	}

	if (ntr > 0){
	   calc_res_SH(seisSH,seisSHfwi,(*acq).recswitch,(*acq).recpos,(*acq).recpos_loc,ntr_glob,ntr,nsrc_glob,(*acq).srcpos,ishot,ns,iter,swstestshot);
	}

	swstestshot=0;

	/* output of time reversed residual seismograms */
	if ((SEISMO)&&(iter==1)&&(ishot==1)){
	   outseis_SHres(seisSH,seisSHfwi,(*acq).recswitch,(*acq).recpos,(*acq).recpos_loc,ntr_glob,(*acq).srcpos,ishot,ns,nstage,FP);       
	}
		          	    		    
	/*================================================================================
		        Starting simulation (adjoint modelling)
	==================================================================================*/
	    
	/* Distribute multiple source positions on subdomains */
	/* define source positions at the receivers */
	(*acq).srcpos_loc_back = matrix(1,6,1,ntr);
	for (i=1;i<=ntr;i++){
	    (*acq).srcpos_loc_back[1][i] = ((*acq).recpos_loc[1][i]);
	    (*acq).srcpos_loc_back[2][i] = ((*acq).recpos_loc[2][i]);
	}
		                            
	/* solve adjoint problem */	  
        sh(waveSH,waveSH_PML,matSH,fwiSH,mpiPSV,seisSH,seisSHfwi,acq,hc,ishot,nshots,nsrc_loc,ns,ntr,Ws,Wr,hin,DTINV_help,1,req_send,req_rec);

	/* assemble SH gradients for each shot */
	ass_gradSH(fwiSH,matSH,iter);

	/* calculate gradients for normalized material parameters */
	for(i=1;i<=NX;i=i+IDX){
	    for(j=1;j<=NY;j=j+IDY){

                (*fwiSH).waveconv_u_shot[j][i] = -(*fwiSH).waveconv_u_shot[j][i] / (NTDTINV * nshots * ntr_glob);
		(*fwiSH).waveconv_rho_shot[j][i] = -(*fwiSH).waveconv_rho_shot[j][i] / (NTDTINV * nshots * ntr_glob);
		(*fwiSH).waveconv_ts_shot[j][i] = -(*fwiSH).waveconv_ts_shot[j][i] / (NTDTINV * nshots * ntr_glob);

	    }
	}

	if((EPRECOND==1)||(EPRECOND==3)){

	  /* calculate energy weights */
	  eprecond1(We,Ws,Wr);
	      
	  /* scale gradient with maximum model parameters and energy weights*/
	  for(i=1;i<=NX;i=i+IDX){
	      for(j=1;j<=NY;j=j+IDY){

		       (*fwiSH).waveconv_u_shot[j][i] = (*fwiSH).waveconv_u_shot[j][i] / We[j][i];
		     (*fwiSH).waveconv_rho_shot[j][i] = (*fwiSH).waveconv_rho_shot[j][i] / We[j][i];
		      (*fwiSH).waveconv_ts_shot[j][i] = (*fwiSH).waveconv_ts_shot[j][i] / We[j][i];

	      }
	  }
	}

	if (SWS_TAPER_CIRCULAR_PER_SHOT){    /* applying a circular taper at the source position to the gradient of each shot */
	
		/* applying the preconditioning */
		taper_grad_shot((*fwiSH).waveconv_rho_shot,taper_coeff,(*acq).srcpos,nsrc,(*acq).recpos,ntr_glob,ishot);
		taper_grad_shot((*fwiSH).waveconv_u_shot,taper_coeff,(*acq).srcpos,nsrc,(*acq).recpos,ntr_glob,ishot);
		taper_grad_shot((*fwiSH).waveconv_ts_shot,taper_coeff,(*acq).srcpos,nsrc,(*acq).recpos,ntr_glob,ishot);
	
	} /* end of SWS_TAPER_CIRCULAR_PER_SHOT == 1 */

	for(i=1;i<=NX;i=i+IDX){
		for(j=1;j<=NY;j=j+IDY){
			(*fwiSH).waveconv_rho[j][i] += (*fwiSH).waveconv_rho_shot[j][i];
			(*fwiSH).waveconv_u[j][i] += (*fwiSH).waveconv_u_shot[j][i];
			(*fwiSH).waveconv_ts[j][i] += (*fwiSH).waveconv_ts_shot[j][i];
		}
	}

	if(RTM_SHOT==1){RTM_SH_out_shot(fwiSH,ishot);}

	if((N_STREAMER>0)||(READREC==2)){

	   if (SEISMO) free_imatrix((*acq).recpos,1,3,1,ntr_glob);

	   if ((ntr>0) && (SEISMO)){

		   free_imatrix((*acq).recpos_loc,1,3,1,ntr);
		   (*acq).recpos_loc = NULL;
	 
		   switch (SEISMO){
		   case 1 : /* particle velocities only */
		           free_matrix((*seisSH).sectionvz,1,ntr,1,ns);
		           (*seisSH).sectionvz=NULL;
		           break;

		    }

	   }

	   free_matrix((*seisSHfwi).sectionread,1,ntr_glob,1,ns);
	   free_ivector((*acq).recswitch,1,ntr);
	   
	   if(QUELLTYPB){
	      free_matrix((*seisSHfwi).sectionvzdata,1,ntr,1,ns);
	      free_matrix((*seisSHfwi).sectionvzdiff,1,ntr,1,ns);
	      free_matrix((*seisSHfwi).sectionvzdiffold,1,ntr,1,ns);
	   }	   
	   
	   ntr=0;
	   ntr_glob=0;
	 
	}

	nsrc_loc=0;

	} /* end of loop over shots (forward and backpropagation) */   

	/* Calculate Pseudo-Hessian for Vs-rho-ts parametrization */
	/* and apply inverse Hessian to gradient */
	if(EPRECOND==4){
	   apply_inv_hessSH(fwiSH, matSH, nshots);
	}

	/* calculate L2 norm of all CPUs*/
	L2sum = 0.0;
        L2_tmp = (*seisSHfwi).L2;
	MPI_Allreduce(&L2_tmp,&L2sum,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);

return L2sum;

}

