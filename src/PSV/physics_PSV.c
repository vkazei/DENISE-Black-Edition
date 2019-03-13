/*
 * Forward, FWI, RTM and RTMOD modules (PSV problem)  
 *
 * Daniel Koehn
 * Kiel, 26/04/2016
 */

#include "fd.h"

void physics_PSV(){

	/* global variables */
	extern int MODE;

	/* 2D PSV Forward Problem */
	printf("\n\n!!! MODE = %i!!!\n\n",MODE);
	if(MODE==0){
	   printf("\n\n!!! INSIDEE FD_PSV() !!!\n\n");
	   FD_PSV();
	}

	/* 2D PSV Full Waveform Inversion */
	printf("\n\n!!! BEFORE FWI_PSV() !!!\n\n");
	if(MODE==1){
	   printf("\n\n!!! INSIDE FWI_PSV() !!!\n\n");
	   FWI_PSV();
	   printf("\n\n!!! AFTER FWI_PSV() !!!\n\n");
	}

        /* 2D PSV Reverse Time Migration */
	if(MODE==2){
	   RTM_PSV();
	}

}



