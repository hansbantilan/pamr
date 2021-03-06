//=============================================================================
// evolve.c --- evolution and initial data calculation routines 
//=============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bbhutil.h>
#include "globals_w.h"
#include "util_w.h"
#include "mg_w.h"
#include "evolve_w.h"
#include "cls_w.h"
#include "m_util_r8_w.h"
#include "regrid_script_w.h"
#include "io_w.h"
#include <time.h>
#include <math.h>

void sync_free_data(int L);
void inject_free_data(int Lf);
void interp_free_data(int Lc);

//=============================================================================
// global info for regridding script
//=============================================================================
int rgs_gnum=0;
int *rgs_glev=0;
real *rgs_gbbox=0;
real rgs_t;
int rgs_L1;
int rgs_Lf;

//=============================================================================
// evolves the entire hierarchy 'steps' coarse steps forward in time
//
// The flow of this routine is almost a verbatim copy of cycle() in ad;
// in particular, we manually implement the recursion via stacks so that
// we can easily check-point the evolution.
//
// NOTE: some hard-coded assumptions, and properties of the flow of the
// algorithm:
//
// o AMRD_num_evo_tl >= 2
//
// o the time-sequence is *always* tl=2,3,...,AMRD_num_evo_tl,1,
//   *except* during tstep(), when it is tl=1,2,3,...,AMRD_num_evo_tl
//
// o we are using linear extrapolation for MG variables, and saving 
//   them when in sync with 1 parent level. 
//
// o when we regrid from lev..fin_lev, f_extrap_tm1 is in 
//   sync with tl=2, and for levels > lev f_extrap_tm2 is in sync 
//   with tl=3(or 1) of the parent level
//
// o we only compute corrections to f_extrap_tm2 after re-solves in
//   evolve(), and *not* in regrid().
//
// o during tstep, the time of f_extrap_tm1 is *always* that of 'tn'
//   of the parent level (which has been evolved to 'tnp1' already).
//
// if (ic==1) then evolve called for initial data calculation ... hence disable
// some things like saves, regrids, etc.
//
// certain variables are global, for check-pointing
//=============================================================================
int AMRD_stack[2*PAMR_MAX_LEVS],AMRD_top;
real prev_cp_time_hrs=0,tot_cpu_time_hrs=0;
void richardson_extrap(int lev);
void evolve(int coarse_steps, int ic)
{
   int csteps,clev,frg=1,i;
   real ct,cdx[PAMR_MAX_DIM],cdt;
   real cm1t,cm1dx[PAMR_MAX_DIM],cm1dt;
   int rho_sp[PAMR_MAX_LEVS],rho_tm[PAMR_MAX_LEVS],Lf;
   int test_ic_save_only=0;
   real prev_cpu_time_hrs,cpu_time_hrs,delta_time_hrs,ltime;
   char cp_filename[256];
   real c_rgs_t;
   int c_rgs_L1,c_rgs_Lf,done;
   static int first_cp=1;

   if (AMRD_num_evo_tl<2) AMRD_stop("evolve ... we currently require AMRD_num_evo_tl to be >=2","");

   PAMR_get_rho(rho_sp,rho_tm,PAMR_MAX_LEVS);
   Lf=PAMR_get_max_lev(PAMR_AMRH);

   prev_cpu_time_hrs=clock()/((real)CLOCKS_PER_SEC*3600.0e0);
   if (!ic)
   {
      fixup_ivec(1,coarse_steps+1,0,AMRD_save_ivec0);
      if (!AMRD_cp_restart && do_ivec(1,coarse_steps+1,AMRD_save_ivec0)) save0(0);

      for (i=0; i<PAMR_MAX_LEVS; i++) fixup_ivec(1,AMRD_MAX_IVEC_SIZE,0,AMRD_save_ivec[i]);
      for (i=1; i<=Lf; i++)
         if (!AMRD_cp_restart && do_ivec(AMRD_lsteps[i-1]+1,AMRD_MAX_IVEC_SIZE,AMRD_save_ivec[i-1])) saveL(i,0);
      if (test_ic_save_only) AMRD_stop("test_ic_save_only","");
   }

   if (AMRD_cp_restart)
   {
      frg=0;
      // if we were reading from a regrid script, then need to advance to the
      // same point within the regrid file
      if (AMRD_regrid_script==AMRD_READ_REGRID_SCRIPT)
      {
         c_rgs_t=rgs_t;
         c_rgs_L1=rgs_L1;
         c_rgs_Lf=rgs_Lf;
         rgs_t=-1;
         rgs_L1=-1;
         rgs_Lf=-1;
         done=0;
         while(!done)
         {
            if (!(rgs_read_next(&rgs_t,&rgs_L1,&rgs_Lf,&rgs_gnum,&rgs_glev,&rgs_gbbox)))
            {
               printf("evolve: error reading next-step info from regridding script ... turning off script\n");
               AMRD_regrid_script=AMRD_NO_REGRID_SCRIPT;
               done=1;
            }
            else if (fabs(rgs_t-c_rgs_t)<1.0e-8 && c_rgs_L1==rgs_L1 && rgs_Lf==rgs_Lf) done=1;
         }
      }

      // increase number of coarse steps if requested
      if ((AMRD_stack[1]+AMRD_lsteps[0])<coarse_steps) AMRD_stack[1]=coarse_steps-AMRD_lsteps[0];
      if (AMRD_evo_trace && !my_rank) printf("\nContinuing evolution from previous checkpoint file ... \n");
   }
   else
   {
      AMRD_top=0;
      AMRD_stack[AMRD_top++]=1;
      AMRD_stack[AMRD_top++]=coarse_steps;

      if (AMRD_evo_trace && my_rank==0) 
      {
         printf("\nevolve: coarse_steps=%i\n",coarse_steps);
         printf("%s\n",line_break);
      }
   }

   while(AMRD_top)
   {
      //-----------------------------------------------------------------------
      // time to check-point? (note, clock does wrap around ... in that case
      // just set previous time to zero).
      //-----------------------------------------------------------------------
      cpu_time_hrs=clock()/((real)CLOCKS_PER_SEC*3600.0e0);
      delta_time_hrs=max(0,cpu_time_hrs-prev_cpu_time_hrs);
      prev_cpu_time_hrs=cpu_time_hrs;
      tot_cpu_time_hrs+=delta_time_hrs;
      ltime=tot_cpu_time_hrs;
      MPI_Allreduce(&ltime,&tot_cpu_time_hrs,1,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD);

      if ( !ic && AMRD_cp_delta_t_hrs >0 && AMRD_cp_save_fname &&
          ( (first_cp && ((prev_cp_time_hrs+AMRD_cp_first_delta_t_hrs)<tot_cpu_time_hrs)) ||
            (!first_cp && ((prev_cp_time_hrs+AMRD_cp_delta_t_hrs)<tot_cpu_time_hrs)) ) )
      {
         first_cp=0;
         sprintf(cp_filename,"%s_%c",AMRD_cp_save_fname,AMRD_curr_cp_tag);

         if (AMRD_curr_cp_tag=='Z') AMRD_curr_cp_tag='a';
         else if (AMRD_curr_cp_tag=='z') AMRD_curr_cp_tag='A';
         else AMRD_curr_cp_tag++;
         if (AMRD_curr_cp_tag=='A') if (!my_rank) printf("WARNING ... more than 52 checkpoints ... AMRD_curr_cp_tag wrapped around\n");

         if (my_rank==0) printf("\n%s\nt(%i)=%lf, saving state to file %s\n",line_break,Lf,PAMR_get_time(Lf),cp_filename);

         if (!(amrd_do_cp(cp_filename,AMRD_CP_SAVE))) if (!my_rank) printf("WARNING ... AMRD check-point to %s failed\n",cp_filename);
         //         if (!(PAMR_cp(cp_filename,-1))) if (!my_rank) printf("WARNING ... PAMR check-point to %s  failed\n",cp_filename);
         if (!(PAMR_cp(cp_filename))) if (!my_rank) printf("WARNING ... PAMR check-point to %s  failed\n",cp_filename);

         if (my_rank==0) printf("\n%s\n",line_break);
         prev_cp_time_hrs=tot_cpu_time_hrs;
      }

      csteps=AMRD_stack[AMRD_top-1];
      clev=AMRD_stack[AMRD_top-2];
      PAMR_get_dxdt(clev,cdx,&cdt); ct=PAMR_get_time(clev);
      if (clev>1) { PAMR_get_dxdt(clev-1,cm1dx,&cm1dt); cm1t=PAMR_get_time(clev-1); }
      //-----------------------------------------------------------------------
      // if (csteps>0) we're descending the evolution tree (i.e. regrid, 
      // evolve, etc. ), else if (csteps<0) we're ascending (i.e. inject, etc.)
      //-----------------------------------------------------------------------
      if (csteps>0)
      {
         //--------------------------------------------------------------------
         // regrid, if time to do so, and we haven't already on a 
         // coarser level (because regridding always involves the current to 
         // finest level)
         //--------------------------------------------------------------------
         if (!ic && clev < AMRD_max_lev && clev >= AMRD_regrid_min_lev && AMRD_lsteps[clev-1]>0 && 
             !(AMRD_lsteps[clev-1]%AMRD_regrid_interval) &&
             (clev==AMRD_regrid_min_lev || ((AMRD_lsteps[clev-2]-1)%AMRD_regrid_interval) || (int)(fabs(cm1t-ct-cm1dt)/cdt)))
         {
            for (i=clev; i<Lf; i++) 
               if (!AMRD_tre_valid[i]) printf("evolve ... WARNING ... time to regrid, but TRE(%i) is invalid!\n",i+1);
            if (!frg || !AMRD_skip_frg) 
            {
               if (AMRD_regrid_script==AMRD_READ_REGRID_SCRIPT)
               {
                  if (fabs(rgs_t-ct)>(cdt/2) && my_rank==0) 
                     printf("evolve ... WARNING ... regrid time from script (%lf) != t (%lf)\n",rgs_t,ct);
                  if (rgs_L1!=(clev+1))
                     AMRD_stop("evolve ... ERROR ... corrupt level information from regrid script","");
               }

               regrid(clev,1,1);
               Lf=PAMR_get_max_lev(PAMR_AMRH);

               if (AMRD_regrid_script==AMRD_READ_REGRID_SCRIPT)
               {
                  if (!(rgs_read_next(&rgs_t,&rgs_L1,&rgs_Lf,&rgs_gnum,&rgs_glev,&rgs_gbbox)))
                  {
                     printf("evolve: error reading next-step info from regridding script ... turning off script\n");
                     AMRD_regrid_script=AMRD_NO_REGRID_SCRIPT;
                  }
               }
               else if (AMRD_regrid_script==AMRD_WRITE_REGRID_SCRIPT)
               {
                  rgs_L1=clev+1; rgs_t=ct; rgs_Lf=Lf;
                  if (!(rgs_write_next(rgs_t,rgs_L1,rgs_Lf,rgs_gnum,rgs_glev,rgs_gbbox)))
                  {
                     printf("evolve: error writing next-step info to regridding script ... turning off script\n");
                     AMRD_regrid_script=AMRD_NO_REGRID_SCRIPT;
                  }
               }
            } 
            frg=0;
         }
         if (clev==1 && AMRD_calc_global_var_norms) calc_global_norms();
         //--------------------------------------------------------------------
         // take a single coarse step on the current level
         //--------------------------------------------------------------------
         if (app_pre_tstep) app_pre_tstep(clev);

         tstep(clev,ic);

         app_post_tstep(clev);

         if (AMRD_num_fc_vars>0 && clev<Lf && clev>1) { 
           //--------------------------------------------------------------------
           // interpolate the fcs variables to the next level
           //--------------------------------------------------------------------
           
           PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
           for(i=0; i<AMRD_num_fc_vars; i++) PAMR_set_tf_bit(AMRD_fcs_gfn[i],PAMR_FIRST_ORDER_EXTENSIVE);
           PAMR_interp(clev,1,PAMR_AMRH); PAMR_thaw_tf_bits();
         }

         AMRD_stack[AMRD_top-2]=clev;
         AMRD_stack[AMRD_top-1]=-csteps;
         //--------------------------------------------------------------------
         // now "recursively" evolve finer levels
         //--------------------------------------------------------------------
         if (clev<Lf)
         {
            AMRD_stack[AMRD_top++]=clev+1;
            AMRD_stack[AMRD_top++]=rho_tm[clev-1];
         }
      }
      else
      {
         csteps=abs(csteps)-1;
         if (csteps)
         {
            AMRD_stack[AMRD_top-2]=clev;
            AMRD_stack[AMRD_top-1]=csteps;
         }
         else
         {
            if (clev>1)
            {
              if (AMRD_num_f_tre_vars > 0) compute_ssh_tre(clev);

              if (AMRD_num_fc_vars>0 && clev > 2) {
                PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
                for(i=0; i<AMRD_num_fc_vars; i++) PAMR_set_tf_bit(AMRD_fcs_gfn[i],PAMR_SYNC);
                PAMR_sync(clev,1,PAMR_AMRH,0); PAMR_thaw_tf_bits();

                // inject fcs variables.
                PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
                for(i=0; i<AMRD_num_fc_vars; i++) PAMR_set_tf_bit(AMRD_fcs_gfn[i],PAMR_NN_ADD);
                PAMR_inject(clev,1,PAMR_AMRH); 
                PAMR_thaw_tf_bits();

                // Now zero type B cells on level clev.
                call_app_1int_ifc_mask(app_fcs_var_clear,1,clev,PAMR_AMRH);
                
                // Apply the flux correction.
                call_app(app_flux_correct,clev-1,PAMR_AMRH);
                
                // Post flux correction (which now does the primitive variable inversion).
                call_app(app_post_flux_correct,clev-1,PAMR_AMRH);

                // Now zero type A cells on level clev-1.
                call_app_1int_ifc_mask(app_fcs_var_clear,0,clev-1,PAMR_AMRH);
              }

              if (AMRD_re_interp_width>0) PAMR_AMR_bdy_interp(clev-1,2,AMRD_re_interp_width);
              if (AMRD_re_interp_width_c>0 && clev > 2) {
                // Force first order conservative interpolation.
                PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
                for(i=0; i<AMRD_num_hyperbolic_vars; i++)
                  if (PAMR_var_type(AMRD_hyperbolic_vars[i]) == PAMR_CELL_CENTERED){ 
                                 PAMR_set_tf_bit(AMRD_AMR_f_gfn[1][i],PAMR_FIRST_ORDER_CONS);
                  }
                PAMR_AMR_bdy_interp_c(clev-1,2,AMRD_re_interp_width_c); PAMR_thaw_tf_bits();
              }

              if (AMRD_magic_cookie) richardson_extrap(clev);

                 if(AMRD_num_inject_wavg_vars){  //Multiply by wavg function
                      apply_wavg(0, clev-1, PAMR_AMRH);
                         apply_wavg(0, clev, PAMR_AMRH);
              }
              PAMR_inject(clev,2,PAMR_AMRH);

              // Inject work variables here.  
              if (AMRD_num_AMRH_work_inject_vars>0 && clev > 2) {
                PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
                for(i=0; i<AMRD_num_AMRH_work_inject_vars; i++)
                  PAMR_set_tf_bit(AMRD_AMRH_work_inject_gfn[i],AMRD_AMRH_work_inject_op[i]);
                PAMR_inject(clev,1,PAMR_AMRH); PAMR_thaw_tf_bits();
              }
                 if(AMRD_num_inject_wavg_vars){ //Divide by wavg function
                      apply_wavg(1, clev-1, PAMR_AMRH);
                         apply_wavg(1, clev, PAMR_AMRH);
              }

              if (AMRD_periodic[0] || (AMRD_dim>1 && AMRD_periodic[1]) || (AMRD_dim>2 && AMRD_periodic[2]))
                PAMR_sync(clev-1,2,PAMR_AMRH,0); // need to resync incase parent only touches left-periodic boundary
            }
            AMRD_top-=2;
         }
         //--------------------------------------------------------------------
         // if on the coarsest level, or csteps>0 (implying clev..finlev is in
         // sync), but we're not at the finest level (which is handled in tstep)
         // we resolve the elliptics at tnp1 (tl=2 here)
         //--------------------------------------------------------------------
         if (clev==1 || csteps>0)
         {
            if (clev<Lf)
            {
               //evo_dump(clev,Lf,"evo_before_resolve_",0,0);
               set_mg_brs_vars(clev,Lf);
               if (clev!=Lf) 
               {
                  for (i=clev; i<=Lf; i++) PAMR_c_to_v(i,2,PAMR_AMRH,0);
                  solve_elliptics(clev,2,0);
               }
               if (!(ic && clev==1 && AMRD_lsteps[0]==coarse_steps)) update_mg_extrap_vars(clev,Lf);
               //evo_dump(clev,Lf,"evo_after_resolve_",0,0);
            }
            for (i=clev; i<=Lf; i++)
               if (!ic && do_ivec(AMRD_lsteps[i-1]+1,AMRD_MAX_IVEC_SIZE,AMRD_save_ivec[i-1])) saveL(i,AMRD_lsteps[i-1]);
         }

         //--------------------------------------------------------------------
         // with save_ivec0, we save all levels, at times in-sync with coarsest
         //--------------------------------------------------------------------
         if (!ic && clev==1 && do_ivec(AMRD_lsteps[0]+1,coarse_steps+1,AMRD_save_ivec0)) save0(AMRD_lsteps[0]);
      }
   }
}

//=============================================================================
// Evolve level lev one time step. If this is the finest level in the
// hierarchy, then the elliptics are solved in tandem with the evolution 
// equations, else they are extrapolated from past data. After the time step
// is complete, we cyclically shift all the levels, so that tl=2 is the
// most advanced level, and tl=1 is the most retarded.
//=============================================================================
void tstep(int lev, int ic)
{
   int valid,i,di,j,Lf,iter,ltrace=0,mgLf=0,do_ibnd_interp=0,pbt[2*PAMR_MAX_DIM],k;
   static int first=1;
   real tnp1,tnp1_lm1,dx_lm1[PAMR_MAX_DIM],dt_lm1,dx[PAMR_MAX_DIM],dt,f1,f2;
   real mg_res,evo_res,mask_off;
   int rho_sp[PAMR_MAX_LEVS],rho_tm[PAMR_MAX_LEVS],cAMR_bdy_width;
   int even=PAMR_EVEN,odd=PAMR_ODD,stride;
   real *c_eps,frac,t_end;

   if (AMRD_evo_trace>2 && my_rank==0) printf("\ntstep(lev=%i)\n",lev);

   PAMR_get_rho(rho_sp,rho_tm,PAMR_MAX_LEVS);
   Lf=PAMR_get_max_lev(PAMR_AMRH);

   //printf("lev=%i, Lf=%i\n",lev,Lf);

   set_cmask_bdy(lev,PAMR_AMRH);
   PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
   PAMR_set_tf_bit(AMRD_cmask_gfn,PAMR_SYNC);
   PAMR_set_tf_bit(AMRD_cmask_c_gfn,PAMR_SYNC);
   PAMR_sync(lev,1,PAMR_AMRH,0); PAMR_thaw_tf_bits();

   PAMR_get_dxdt(lev,dx,&dt);
   tnp1=PAMR_get_time(lev)+dt; 
   if (lev>1) { tnp1_lm1=PAMR_get_time(lev-1); PAMR_get_dxdt(lev-1,dx_lm1,&dt_lm1); }

   if (!ic) evo_dump(lev,lev,"tstep_before_",0,0);

   //-----------------------------------------------------------------------
   // initialize hyperbolic tl=1 variables
   //-----------------------------------------------------------------------
   if (AMRD_np1_initial_guess==AMRD_NP1_IG_FIRST_ORDER)
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         for (i=0; i<AMRD_num_hyperbolic_vars; i++) 
         {
           if (PAMR_var_type(AMRD_hyperbolic_vars[i]) == PAMR_VERTEX_CENTERED) 
             for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_f[0][i])[j]=(AMRD_AMR_f[1][i])[j];
           else 
             for (j=0; j<AMRD_g_size_c; j++) (AMRD_AMR_f[0][i])[j]=(AMRD_AMR_f[1][i])[j];
         }
         valid=PAMR_next_g();
      }
   }
   else if (AMRD_np1_initial_guess!=AMRD_NP1_IG_NONE) AMRD_stop("invalid np1_initial_guess","");

   //--------------------------------------------------------------------------
   // we solve the elliptics if on the finest level
   //--------------------------------------------------------------------------
   if (lev==Lf && AMRD_num_elliptic_vars>0) 
   {
      mg_res=AMRD_MG_tol+1;
      if (!(mgLf=PAMR_build_mgh(Lf,Lf,1))) AMRD_stop("tstep: PAMR_build_mgh() failed\n",0);
   }
   else mg_res=0;

   if (AMRD_num_tnp1_liipb_vars || AMRD_num_tnp1_liiab_vars || AMRD_num_tnp1_liibb_vars) do_ibnd_interp=1;
   //--------------------------------------------------------------------------
   // now solve the equations (evo_ssc --- seperate stopping criteria,
   // but for MG only at this stage)
   //
   // NOTE: if there are *no* overlapping grids in the sequential
   // hierarchy, then we could set cAMR_bdy_width=0 always; otherwise,
   // cAMR_bdy_width needs to be 1 because the ghost_width mechanism
   // of PAMR cannot yet (ever?) deal with sequential-overlap ghost regions.
   // However, regardless, the first iteration needs to be zero, to properly
   // sync any communication differences at AMR boundaries within the parallel
   // ghost zones.
   //--------------------------------------------------------------------------
   iter=0;
   evo_res=AMRD_evo_tol+1;
   while ((iter < AMRD_evo_min_iter || evo_res > AMRD_evo_tol || mg_res > AMRD_MG_tol) && iter < AMRD_evo_max_iter)
   {
      if (evo_res > AMRD_evo_tol || !AMRD_evo_ssc || iter < AMRD_evo_min_iter)
      {

         //-----------------------------------------------------------------------
         // initialize interior boundaries via interpolation from the
         // parent level, and extrapolate elliptic variables
         //-----------------------------------------------------------------------
         if (iter<=AMRD_num_t_interp_substeps)
         {
            //-----------------------------------------------------------------------
            // extrapolate elliptic variables. We assume t(extrap_tm1)=tnp1_lm1-dt_lm1,
            // and t(extrap_tm2)=tnp1_lm1-2*dt_lm1
            //-----------------------------------------------------------------------

            if (lev>1)
            {
               if (AMRD_num_t_interp_substeps==0 || iter==AMRD_num_t_interp_substeps) frac=1; 
               else frac = AMRD_t_interp_substeps[iter];

               t_end  = tnp1-dt + frac*dt;  

               f1=(t_end-(tnp1_lm1-2*dt_lm1))/dt_lm1;
               f2=-(t_end-(tnp1_lm1-dt_lm1))/dt_lm1;
               valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
               while(valid)
               {
                  ev_ldptr();   
                  for (i=0; i<AMRD_num_elliptic_vars; i++) 
                  {
                     if (AMRD_MG_extrap_method==AMRD_MG_EXTRAP_2ND_FROM_ETM1)
                     {
                        for (j=0; j<AMRD_g_size; j++) 
                           (AMRD_AMR_mgf[0][i])[j]=f1*(AMRD_AMR_mgf_extrap_tm1[i])[j]+f2*(AMRD_AMR_mgf_extrap_tm2[i])[j];
                     }
                     else if (AMRD_MG_extrap_method==AMRD_MG_EXTRAP_2ND_FROM_TN)
                     {
                        for (j=0; j<AMRD_g_size; j++) 
                           (AMRD_AMR_mgf[0][i])[j]=(AMRD_AMR_mgf[1][i])[j]+
                              dt/dt_lm1*((AMRD_AMR_mgf_extrap_tm2[i])[j]-(AMRD_AMR_mgf_extrap_tm1[i])[j]);
                     }
                     else AMRD_stop("evolve: invalid MG_extrap_method\n","");
                  }
                  valid=PAMR_next_g();
               }
	    }

            //--------------------------------------------------------------------
            // initialize interior boundaries via interpolation from the
            // parent level, and extrapolate elliptic variables
            //--------------------------------------------------------------------
            if (lev>2)
            {
               if (iter>=AMRD_num_t_interp_substeps) frac=1; else frac=AMRD_t_interp_substeps[iter];

               if (AMRD_max_t_interp_order<=2 || AMRD_num_evo_tl<=2) set_amr_bdy_2nd_ord(lev,tnp1,dt,tnp1_lm1,dt_lm1,frac);
               else if (AMRD_max_t_interp_order==3 && AMRD_num_evo_tl>=3) set_amr_bdy_3rd_ord(lev,tnp1,dt,tnp1_lm1,dt_lm1,frac);
               else AMRD_stop("AMRD: invalid combination of AMRD_max_t_interp_order and AMRD_num_evo_tl","");
               set_amr_bdy_2nd_ord_hydro(lev,tnp1,dt,tnp1_lm1,dt_lm1,frac);
     
               //--------------------------------------------------------------------
               // interpolate AMR boundaries if desired
               //--------------------------------------------------------------------
               valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
               while(valid)
               {
                  ev_ldptr();   
                  for(i=0; i<AMRD_num_interp_AMR_bdy_vars; i++)
                     interp_AMR_bdy(AMRD_interp_AMR_bdy_f[i],AMRD_w1,rho_sp[lev-2]);
            
                  valid=PAMR_next_g();
               }
         
               //--------------------------------------------------------------------------
               // sync any interpolated boundaries, unless on finest (because then
               // the vcycle and relaxation will do so for us)
               // NOTE: could optimize a bit by not syncing AMR vars here
               //--------------------------------------------------------------------------
               if (AMRD_num_interp_AMR_bdy_vars>0 && lev<Lf)
               {
                  PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
                  for(i=0; i<AMRD_num_interp_AMR_bdy_vars; i++) PAMR_set_tf_bit(AMRD_interp_AMR_bdy_f_gfn[i],PAMR_SYNC);
                  PAMR_sync(lev,1,PAMR_AMRH,0); PAMR_thaw_tf_bits();
               }
            }

            //--------------------------------------------------------------------------
            // allow a 'standard' way to automatically add KO dissipation. We dissipate
            // the retard time level ... it may be important to do so here, rather
            // than doing the advanced level after the evolution step, for this 
            // will help smooth injection boundaries on coarser levels. However,
            // we also offer an option to post-dissipate (AMRD_num_tnp1_diss_vars)
            //
            // NOTE: it is important to do this *after* setting the boundaries,
            // because set_amr_bdy_3rd_ord() may use tn boundary values
            // as temporary variables, afterwhich it reinitializes level tn 
            // boundaries. Thus we need to dissipate afterwards.
            //--------------------------------------------------------------------------
            if (iter==0 && AMRD_tn_eps_diss!=0 && AMRD_num_tn_diss_vars>0 && (!(AMRD_lsteps[lev-1]%AMRD_diss_freq)))
            {
               valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
               while(valid)
               {
                  ev_ldptr();
                  mask_off=AMRD_CMASK_OFF;
                  for (i=0; i<AMRD_num_tn_diss_vars;i++) 
                  {
                     for (k=0; k<2*AMRD_dim; k++) if (AMRD_g_phys_bdy[k]) pbt[k]=AMRD_tn_diss_pbt[i][k]; else pbt[k]=PAMR_UNKNOWN;
                     c_eps=&AMRD_tn_eps_diss; if (AMRD_do_ex<0) c_eps=AMRD_chr;
                     for (stride=AMRD_diss_stride; stride>0; stride--)
                        dmdiss3d_(AMRD_tn_diss[i],AMRD_w1,c_eps,&AMRD_diss_bdy,
                                  pbt,&even,&odd,
                                  AMRD_cmask,&mask_off,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                                  AMRD_chr,&AMRD_ex,&AMRD_do_ex,&AMRD_diss_use_6th_order,&stride);
                  }
                  valid=PAMR_next_g();
               }
               PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
               for(i=0; i<AMRD_num_tn_diss_vars; i++) PAMR_set_tf_bit(AMRD_tn_diss_gfn[i],PAMR_SYNC);
               //-----------------------------------------------------------------------
               // because the bits are frozen, the time-level flag doesn't matter, so 
               // so time levels > 2 will also get sync'd if AMRD_diss_freq>1
               //-----------------------------------------------------------------------
               PAMR_sync(lev,2,PAMR_AMRH,0); PAMR_thaw_tf_bits(); 
            }

            if (!ic && iter==0) evo_dump(lev,lev,"tstep_after_init_",0,0);
         }

         call_app_1int_ifc_mask(app_evolve,iter+1,lev,PAMR_AMRH);
         if (do_ibnd_interp) ibnd_interp(lev);

         if (AMRD_c_to_v_in_tstep) PAMR_c_to_v(lev,1,PAMR_AMRH,0);
         if (AMRD_v_to_c_in_tstep) PAMR_v_to_c(lev,1,PAMR_AMRH,0);

         if (iter==0) cAMR_bdy_width=0; else cAMR_bdy_width=1;
         PAMR_sync(lev,1,PAMR_AMRH,cAMR_bdy_width);

         if (AMRD_resid_in_evo) evo_res=call_rr_app(app_evo_residual,lev,PAMR_AMRH,0,MPI_MAX);
      }

      if (lev==Lf && AMRD_num_elliptic_vars>0 && (!AMRD_evo_ssc || mg_res > AMRD_MG_tol || iter < AMRD_evo_min_iter) && iter >= (AMRD_MG_start_iter-1)) mg_res=vcycle(iter,0);
      iter++;
      if (AMRD_evo_trace>1 && my_rank==0) printf("\niter %i\t\tevo_res(lev=%i)=%12.8e\tmg_res=%12.8e\n",iter,lev,evo_res,mg_res);
      //if (!ic) evo_dump(lev,lev,"tstep_after_vcycle_",iter,3);
   }

   if (iter==AMRD_evo_max_iter && my_rank==0 && (evo_res > AMRD_evo_tol || mg_res > AMRD_MG_tol) && AMRD_resid_in_evo)
      printf("\nWARNING ... failed to solve evolution (mg) equations to within %lf (%lf) in %i iterations (lev=%i): res=%12.8e (%12.8e)\n",
             AMRD_evo_tol,AMRD_MG_tol,AMRD_evo_max_iter,lev,evo_res,mg_res);

   if (lev==Lf && AMRD_num_elliptic_vars>0) PAMR_destroy_mgh();

   //--------------------------------------------------------------------------
   // post-dissipate
   //--------------------------------------------------------------------------
   if (AMRD_tnp1_eps_diss!=0 && AMRD_num_tnp1_diss_vars>0 && (!(AMRD_lsteps[lev-1]%AMRD_diss_freq)))
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         mask_off=AMRD_CMASK_OFF;
         for (i=0; i<AMRD_num_tnp1_diss_vars;i++) 
         {
            for (k=0; k<2*AMRD_dim; k++) if (AMRD_g_phys_bdy[k]) pbt[k]=AMRD_tnp1_diss_pbt[i][k]; else pbt[k]=PAMR_UNKNOWN;
            c_eps=&AMRD_tnp1_eps_diss; if (AMRD_do_ex<0) c_eps=AMRD_chr;
            for (stride=AMRD_diss_stride; stride>0; stride--)
               dmdiss3d_(AMRD_tnp1_diss[i],AMRD_w1,c_eps,&AMRD_diss_bdy,
                      pbt,&even,&odd,
                      AMRD_cmask,&mask_off,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                      AMRD_chr,&AMRD_ex,&AMRD_do_ex,&AMRD_diss_use_6th_order,&stride);
         }
         valid=PAMR_next_g();
      }
      PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
      for(i=0; i<AMRD_num_tnp1_diss_vars; i++) PAMR_set_tf_bit(AMRD_tnp1_diss_gfn[i],PAMR_SYNC);
      PAMR_sync(lev,1,PAMR_AMRH,0); PAMR_thaw_tf_bits();
   }

   if (!ic) evo_dump(lev,lev,"tstep_before_tick_",0,3);

   PAMR_tick(lev);
   for (i=AMRD_num_evo_tl-1; i>0; i--) PAMR_swap_tl(lev,i+1,i);
   AMRD_lsteps[lev-1]++;
   
   if (AMRD_evo_trace && my_rank==0)
   {
      printf("\n");
      for (i=1;i<lev;i++) printf(" "); 
      printf("L%i step %i: T=%f, iters=%i (cpu t=%7.2lf)",lev,AMRD_lsteps[lev-1],tnp1,iter,tot_cpu_time_hrs);
   }
}

//=============================================================================
// routines that do the temporal interpolation for setting AMR boundaries
//
// Note the current time sequence: (N=num_evo_tl, eg. rho_tm= 2:1)
//
//        ...   n-1          n          n+1        n-(N-2)
// lm1:   ------ x --------- x --------- x --------- x
//
//                                       n    n+1
// l  :                      . --- . --- . --- . --- .
//
// hence the swaps below temporarily realign the levels so that PAMR_AMR_bdy_interp
// can interpolate to the desired fine grid locations
// 
//        ...   n-2         n-1          n          n+1   
// lm1:   ------ x --------- x --------- x --------- x
//
//                                n-1    n    n+1
// l  :                      . --- . --- . --- . --- .
//
//=============================================================================
void set_amr_bdy_2nd_ord(int lev, real tnp1, real dt, real tnp1_lm1, real dt_lm1, real frac)
{
   int rho,valid,i,j,Lf;
   real f1,f2,fn,fn0;

   PAMR_swap_tl(lev-1,1,2);
   PAMR_AMR_bdy_interp(lev-1,1,AMRD_AMR_bdy_width);
   PAMR_AMR_bdy_interp_c(lev-1,1,AMRD_AMR_bdy_width_c);
   PAMR_swap_tl(lev-1,1,2);

   fn0=(int)((tnp1-(tnp1_lm1-dt_lm1))/dt+0.5); // fn0 is now at tnp1 on the fine level
   fn=fn0-1+frac;                              // adjust it to a fraction of the fine level step 
   rho=dt_lm1/dt+0.5;

   //--------------------------------------------------------------------
   // we want f(lev,tnp1) =  fn/rho*f(lev-1,tnp1_lm1) 
   //                      + (1-fn/rho)*f(lev-1,tnp1_lm1-dt_lm1)
   //
   // currently f(lev,tn) =  (fn0-1)/rho*f(lev-1,tnp1_lm1) 
   //                      + (1-(fn0-1)/rho)*f(lev-1,tnp1_lm1-dt_lm1)
   //      and  f(lev,tnp1) = f(lev-1,tnp1_lm1)
   //
   // hence f(lev,tnp1) <= (1+fn-fn0)/(1+rho-fn0)*f(lev,tnp1)+(rho-fn)/(1+rho-fn0)*f(lev,tn)
   //
   // NOTE: below we interpolate at *ALL* interior boundaries ... PAMR
   //       has only interpolated at the boundary of the union of grids
   //--------------------------------------------------------------------
   f1=(1+fn-fn0)/(1+rho-fn0);
   f2=(rho-fn)/(1+rho-fn0);
   valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
   while(valid)
   {
     ev_ldptr();
     
     for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
       if (!in_list(AMRD_hyperbolic_vars[i],AMRD_past_bdy_interp_vars,AMRD_num_past_bdy_interp_vars)) {

         if (PAMR_var_type(AMRD_hyperbolic_vars[i]) == PAMR_VERTEX_CENTERED) {        
           for (j=0; j<AMRD_g_size; j++) {
             if (AMRD_cmask[j]==AMRD_CMASK_OFF) {
               (AMRD_AMR_f[0][i])[j]=f1*(AMRD_AMR_f[0][i])[j]+f2*(AMRD_AMR_f[1][i])[j];
             }
           }
         } else {
           for (j=0; j<AMRD_g_size_c; j++) {
             if (AMRD_cmask_c[j]==AMRD_CMASK_OFF) {
               (AMRD_AMR_f[0][i])[j]=f1*(AMRD_AMR_f[0][i])[j]+f2*(AMRD_AMR_f[1][i])[j];
             }
           }
         }
       }
     }
     valid=PAMR_next_g();
   }
}

//=============================================================================
// Modified for the hydro evolution.
// Note that this routine assumes only two time levels.
// WARNING!  There is no need for the interpolation here to be strictly conservative.  
// Thus, we do not extensivise before interpolating.  You will have a bug if you
// try to use extensive interpolation for the AMR boundaries.
//=============================================================================
void set_amr_bdy_2nd_ord_hydro(int lev, real tnp1, real dt, real tnp1_lm1, real dt_lm1, real frac)
{
   int valid,i,j,Lf;
   real f1,f2;
   real tnph;
   real t;

   if (AMRD_num_past_bdy_interp_vars == 0) return;

   // Note that we are altering the AMR boundaries on both time levels.
   // It is necessary to interpolate boundaries on the "past" timestep
   // since these are filled with junk or data from a first order 
   // conservative interpolation of the boundaries.

   // Do the swapping
   for (i=1; i<AMRD_num_evo_tl; i++) PAMR_swap_tl(lev-1,i+1,i);

   // First, future and past of vertex centered vars
   PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
   for (j=0; j<AMRD_num_evo_tl; j++) {
     for (i=0; i<AMRD_num_past_bdy_interp_vars; i++) {
       if (PAMR_gfn_var_type(AMRD_past_bdy_interp_vars_gfn[j][i])==PAMR_VERTEX_CENTERED) {
         PAMR_set_tf_bit(AMRD_past_bdy_interp_vars_gfn[j][i],AMRD_past_bdy_interp_op[i]);
       }
     }
   }
   PAMR_AMR_bdy_interp(lev-1,1,AMRD_AMR_bdy_width);

   // Now, future and past of cell centered vars
   PAMR_clear_tf_bits();
   for (j=0; j<AMRD_num_evo_tl; j++) {
     for (i=0; i<AMRD_num_past_bdy_interp_vars; i++) {
       if (PAMR_gfn_var_type(AMRD_past_bdy_interp_vars_gfn[j][i])==PAMR_CELL_CENTERED) {
         PAMR_set_tf_bit(AMRD_past_bdy_interp_vars_gfn[j][i],AMRD_past_bdy_interp_op[i]);
       }
     }
   }
   PAMR_AMR_bdy_interp_c(lev-1,1,AMRD_AMR_bdy_width_c);
   PAMR_thaw_tf_bits();

   // Now undo that swapping
   for (i=AMRD_num_evo_tl-1; i>0; i--) PAMR_swap_tl(lev-1,i+1,i);

   Lf=PAMR_get_max_lev(PAMR_AMRH);
   // Note:  It is possible that this kind of averaging (applied to conserved
   // variables) could result in an unphysical state--especially in the 
   // relativistic case.

   // Note, t is the target time for the interpolation.
   t = tnp1 - (1.0-frac)*dt;
   f1 = (t - (tnp1_lm1 - dt_lm1))/dt_lm1;
   f2 = 1.0 - f1;

   valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
   while(valid)
   {
      ev_ldptr();
      for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
        if (in_list(AMRD_hyperbolic_vars[i],AMRD_past_bdy_interp_vars,AMRD_num_past_bdy_interp_vars)) {
          if (PAMR_var_type(AMRD_hyperbolic_vars[i]) == PAMR_VERTEX_CENTERED) {        
            for (j=0; j<AMRD_g_size; j++) {
              if (AMRD_cmask[j]==AMRD_CMASK_OFF) {
                (AMRD_AMR_f[0][i])[j]=f1*(AMRD_AMR_f[0][i])[j]+f2*(AMRD_AMR_f[1][i])[j];
              }
            }
          } else {
            for (j=0; j<AMRD_g_size_c; j++) {
              if (AMRD_cmask_c[j]==AMRD_CMASK_OFF) {
                (AMRD_AMR_f[0][i])[j]=f1*(AMRD_AMR_f[0][i])[j]+f2*(AMRD_AMR_f[1][i])[j];
              }
            }
          }
        }
      }
      valid=PAMR_next_g();
   }
}

void set_amr_bdy_3rd_ord(int lev, real tnp1, real dt, real tnp1_lm1, real dt_lm1, real frac)
{
   int rho,tns,valid,i,j;
   real a,b,c,d,e,g,f1,f2,f3,m0,m;

   m0=(int)((tnp1-(tnp1_lm1-dt_lm1))/dt+0.5);
   m=m0-1+frac;
   rho=dt_lm1/dt+0.5;

   //--------------------------------------------------------------------
   // third order interpolation is a bit trickier, as we only have
   // 1 'temporary' variable, f(lev,tnp1), to hold boundary values,
   // but we need 3 to store the interpolated values from
   // the three parent levels.
   //
   // FP : in v1, had a different method for m=1 ... do not remember
   //      why. Below should work just as well for m=1. Because
   //      of substepping with new version need a unified scheme
   //
   // Like with 2nd_order, we can make use of the prior interpolation 
   // stages as follows. We want:
   //
   // f(lev,tnp1) = - m*(rho-m)/2/rho^2 * f(lev-1,tnm1)
   //               + (rho+m)*(rho-m)/rho^2 * f(lev-1,tn)
   //               + m*(rho+m)/2/rho^2 * f(lev-1,tnp1)
   //
   //           == f1 * f(lev-1,tnm1) + f2 * f(lev-1,tn) + f3 * f(lev-1,tnp1)
   //
   // f(lev,tn) =   - (m0-1)*(rho-(m0-1))/2/rho^2 * f(lev-1,tnm1)
   //               + (rho+(m0-1))*(rho-(m0-1))/rho^2 * f(lev-1,tn)
   //               + (m0-1)*(rho+(m0-1))/2/rho^2 * f(lev-1,tnp1)
   //
   //           == a * f(lev-1,tnm1) + b * f(lev-1,tn) + c * f(lev-1,tnp1)
   //
   // f(lev,tnm1) = - (m0-2)*(rho-(m0-2))/2/rho^2 * f(lev-1,tnm1)
   //               + (rho+(m0-2))*(rho-(m0-2))/rho^2 * f(lev-1,tn)
   //               + (m0-2)*(rho+(m0-2))/2/rho^2 * f(lev-1,tnp1)
   //
   //           == d * f(lev-1,tnm1) + e * f(lev-1,tn) + g * f(lev-1,tnp1)
   //
   //  solve for f(lev-1,tn) and f(lev-1,tnm1):
   //
   //  f(lev-1,tn) = 1/(a*e-d*b) * (a*f(lev,tnm1) - d*f(lev,tn) + (g*a - c*d)*f(lev-1,tnp1))
   //  
   //  f(lev-1,tnm1) = 1/(a*e-d*b) * (-b*f(lev,tnm1) + e*f(lev,tn) - (e*c - b*g)*f(lev-1,tnp1))
   //--------------------------------------------------------------------
   
   f1=-m*(rho-m)/((real)(2*rho*rho));
   f2=(rho+m)*(rho-m)/((real)(rho*rho));
   f3=m*(rho+m)/((real)(2*rho*rho));
   
   if (m<=rho) 
   {

      PAMR_swap_tl(lev-1,1,2);
      PAMR_AMR_bdy_interp(lev-1,1,AMRD_AMR_bdy_width);
      PAMR_AMR_bdy_interp_c(lev-1,1,AMRD_AMR_bdy_width_c);
      PAMR_swap_tl(lev-1,1,2);
 
      a=-(m0-1)*(rho-(m0-1))/((real)(2*rho*rho));
      b=(rho+(m0-1))*(rho-(m0-1))/((real)(rho*rho));
      c=(m0-1)*(rho+(m0-1))/((real)(2*rho*rho));

      d=-(m0-2)*(rho-(m0-2))/((real)(2*rho*rho));
      e=(rho+(m0-2))*(rho-(m0-2))/((real)(rho*rho));
      g=(m0-2)*(rho+(m0-2))/((real)(2*rho*rho));

      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid) {
        ev_ldptr();

        for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
          if (PAMR_var_type(AMRD_hyperbolic_vars[i]) == PAMR_VERTEX_CENTERED) {        
            for (j=0; j<AMRD_g_size; j++) {
              if (AMRD_cmask[j]==AMRD_CMASK_OFF) {
                (AMRD_AMR_f[0][i])[j]=
                  f1*(1.0/(a*e-d*b) * (-b*(AMRD_AMR_f[2][i])[j] + e*(AMRD_AMR_f[1][i])[j] - (e*c - b*g)*(AMRD_AMR_f[0][i])[j]) ) +
                  f2*(1.0/(a*e-d*b) * ( a*(AMRD_AMR_f[2][i])[j] - d*(AMRD_AMR_f[1][i])[j] + (g*a - c*d)*(AMRD_AMR_f[0][i])[j]) ) + 
                  f3*(AMRD_AMR_f[0][i])[j];
              }
            }
          } else {
            for (j=0; j<AMRD_g_size_c; j++) {
              if (AMRD_cmask_c[j]==AMRD_CMASK_OFF) {
                (AMRD_AMR_f[0][i])[j]=
                  f1*(1.0/(a*e-d*b) * (-b*(AMRD_AMR_f[2][i])[j] + e*(AMRD_AMR_f[1][i])[j] - (e*c - b*g)*(AMRD_AMR_f[0][i])[j]) ) +
                  f2*(1.0/(a*e-d*b) * ( a*(AMRD_AMR_f[2][i])[j] - d*(AMRD_AMR_f[1][i])[j] + (g*a - c*d)*(AMRD_AMR_f[0][i])[j]) ) + 
                  f3*(AMRD_AMR_f[0][i])[j];
              }
            }
          }
        }
        valid=PAMR_next_g();
      }
   }
   else AMRD_stop("set_amr_bdy_3rd_ord: error ... invalid m","");
}

//=============================================================================
// Regrid from lev (>1) to fin_lev (<max_lev) i.e, hierarchy can change 
// from lev+1 to fin_lev+1)
//
// NOTE:
// When using a regridding script ... 
//   if we're reading, then we assume that rgs_gnum,rgs_glev and rgs_gbbox 
//   contain the desired information. Afterwards, regrid() will free the
//   memory.
//
//   if we're writing, then the new grid info is saved in 
//   rgs_gnum,rgs_glev and rgs_gbbox (regrid() will free()!)
//=============================================================================
void regrid(int lev, int resolve, int trace)
{
   int Lf,i,j,L1,L2,valid,tl,tnm1,tn,tnp1,k,Lf0,m,io,pbt[2*PAMR_MAX_DIM];
   real t,frac,mask_off;
   int ltrace=1,syncd=0;
   real *old_bbox[PAMR_MAX_LEVS],fn[AMRD_MAX_TIMES];
   int old_num[PAMR_MAX_LEVS],rho_sp[PAMR_MAX_LEVS],rho_tm[PAMR_MAX_LEVS],rho;
   int even=PAMR_EVEN,odd=PAMR_ODD,stride;
   real *c_eps;

   if (AMRD_regrid_script!=AMRD_READ_REGRID_SCRIPT)
   {
      if (rgs_gbbox) free(rgs_gbbox); rgs_gbbox=0;
      if (rgs_glev) free(rgs_glev); rgs_glev=0;
      rgs_gnum=0;
   }

   Lf=PAMR_get_max_lev(PAMR_AMRH);
   PAMR_get_rho(rho_sp,rho_tm,AMRD_max_lev);

   if (lev==1) lev++;
   if (lev>=AMRD_max_lev) return;

   if (AMRD_evo_trace && my_rank==0) printf("\n\nregrid from lev=%i..%i\n",lev+1,min(Lf+1,AMRD_max_lev));
   if (trace) evo_dump(lev,Lf,"evo_before_regrid_",0,0);

   //--------------------------------------------------------------------------
   // save a copy of the existing grid structure, for doing temporal
   // interpolation below
   //--------------------------------------------------------------------------
   for (i=0; i<PAMR_MAX_LEVS; i++) { old_num[i]=0; old_bbox[i]=0; }
   for (L1=lev+1; L1<=Lf; L1++)
   {
      valid=PAMR_init_s_iter(L1,PAMR_AMRH,1);
      while(valid) { old_num[L1-1]++; valid=PAMR_next_g(); }
      if (!(old_bbox[L1-1]=(real *)malloc(sizeof(real)*2*AMRD_dim*old_num[L1-1]))) 
         AMRD_stop("regrid ... out of memory","");
      valid=PAMR_init_s_iter(L1,PAMR_AMRH,1); i=0;
      while(valid) { PAMR_get_g_bbox(&((old_bbox[L1-1])[i++*2*AMRD_dim])); valid=PAMR_next_g(); }
   }
  
   if (Lf>=AMRD_max_lev) Lf0=AMRD_max_lev-1; else Lf0=Lf;
   if (AMRD_regrid_script!=AMRD_READ_REGRID_SCRIPT)
   {
      calc_tre(lev,Lf0);
      simple_cls(lev,Lf0,&rgs_gbbox,&rgs_glev,&rgs_gnum);

      if (rgs_gnum)
      {
         L1=Lf0;
         L2=1;
         for (i=0; i<rgs_gnum; i++) 
         { 
            L1=min(L1,rgs_glev[i]);
            L2=max(L2,rgs_glev[i]);
         }
         adjust_cls(L1,L2,&rgs_gbbox,&rgs_glev,&rgs_gnum);
         for (i=0; i<rgs_gnum; i++) rgs_glev[i]++;
      }
   }
   else 
   {
      for (i=0; i<rgs_gnum; i++) Lf0=max(Lf0,rgs_glev[i]-1); 
   }
   t=PAMR_get_time(lev);

   //--------------------------------------------------------------------------
   // The call to PAMR_compose_hierarchy below is going to do some 
   // interpolation of the hydrodynamic conserved variables.
   //--------------------------------------------------------------------------

   IFL0
   {
      printf("       re-composing hierarchy (rgs_gnum=%i, from L=%i to %i, [rank=%i])\n",rgs_gnum,lev+1,Lf0+1,my_rank);
      for (i=0; i<rgs_gnum; i++) 
      {
         if (AMRD_dim==1) printf("            [%i] L=%i, bbox=[%18.16e,%18.16e]\n",my_rank,
            rgs_glev[i],rgs_gbbox[i*2],rgs_gbbox[i*2+1]);
         else if (AMRD_dim==2) printf("       [%i] L=%i, bbox=[%18.16e,%18.16e][%18.16e,%18.16e]\n",my_rank,
            rgs_glev[i],rgs_gbbox[i*4],rgs_gbbox[i*4+1],rgs_gbbox[i*4+2],rgs_gbbox[i*4+3]);
         else if (AMRD_dim==3) printf("       [%i] L=%i, bbox=[%18.16e,%18.16e][%18.16e,%18.16e][%18.16e,%18.16e]\n",my_rank,
            rgs_glev[i],rgs_gbbox[i*6],rgs_gbbox[i*6+1],rgs_gbbox[i*6+2],rgs_gbbox[i*6+3],
                        rgs_gbbox[i*6+4],rgs_gbbox[i*6+5]);
      }
   }
   if (Lf>AMRD_max_lev) Lf0=Lf-1; // upon  a restart can decrease max_lev
   // FP TEST!
   // PAMR_set_trace_lev(4);
   if (!(PAMR_compose_hierarchy(lev+1,Lf0+1,rgs_gnum,rgs_glev,rgs_gbbox,t))) AMRD_stop("","");
   // PAMR_set_trace_lev(0);
   Lf=PAMR_get_max_lev(PAMR_AMRH);

   IFL0 printf("       post-regrid memory usage for node 0: %i\n",total_mem());

   //--------------------------------------------------------------------------
   // PAMR does not interpret time-levels, hence we need to do temporal
   // interpolation (including the 'extrap' variables)
   //--------------------------------------------------------------------------
   for (L1=lev+1; L1<=Lf; L1++)
   {
      //-----------------------------------------------------------------------
      // old level L1 contains the region where we do *not* need to interpolate
      //-----------------------------------------------------------------------
      valid=PAMR_init_s_iter(L1,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         set_cmask_oldgs(old_bbox[L1-1],old_num[L1-1]);

         //--------------------------------------------------------------------
         // at this stage, the time sequence is tl=2,3,4,...,AMRD_num_evo_tl,1 
         // and all levels are in sync at tl=2.
         //
         // NOTE: the interpolation below assumes that all the fine levels
         // are located within the most advanced "temporal cell" on the coarse
         // level (i.e. between tnp1 and tn) ... hence the following check
         //--------------------------------------------------------------------
         rho=rho_tm[L1-2];
         if (rho < (AMRD_num_evo_tl-1)) 
            AMRD_stop("regrid error: not yet updated for the case num_evo_tl > rho_tm+1","");

         tnp1=2; 
         tn=3; if (AMRD_num_evo_tl<3) tn=1;
         tnm1=4; if (AMRD_num_evo_tl<4) tnm1=1;

         frac=1.0/((real)rho);
            
         //----------------------------------------------------------
         // for the extrapolated variables ... here, level L1 *is*
         // in sync with level L1-1 ; the time of extrap_tm1(L1) is
         // that of tl=2 and the time of extrap_tmp2(L1) is that
         // of tl=1 (or 3 if AMRD_num_evo_tl>2) of level L-1 ...
         // ; hence we can simply copy the
         // interpolated values over (before we 'fix' these below)
         //----------------------------------------------------------
         if (AMRD_num_evo_tl==2) tl=1; else tl=3;
         for (k=0; k<AMRD_num_elliptic_vars; k++) {
           if (PAMR_var_type(AMRD_elliptic_vars[k]) == PAMR_VERTEX_CENTERED) {        
             for (j=0; j<AMRD_g_size; j++) {
               if (AMRD_cmask[j]==AMRD_CMASK_OFF) {
                 (AMRD_AMR_mgf_extrap_tm1[k])[j]=(AMRD_AMR_mgf[1][k])[j];
                 (AMRD_AMR_mgf_extrap_tm2[k])[j]=(AMRD_AMR_mgf[tl-1][k])[j];
               }
             }
           } else {
             for (j=0; j<AMRD_g_size_c; j++) {
               if (AMRD_cmask_c[j]==AMRD_CMASK_OFF) {
                 (AMRD_AMR_mgf_extrap_tm1[k])[j]=(AMRD_AMR_mgf[1][k])[j];
                 (AMRD_AMR_mgf_extrap_tm2[k])[j]=(AMRD_AMR_mgf[tl-1][k])[j];
               }
             }
           }
         }
               
         if (AMRD_max_t_interp_order<=2 || AMRD_num_evo_tl<=2) io=2;
         else if (AMRD_max_t_interp_order==3 && AMRD_num_evo_tl>=3) io=3;
         else AMRD_stop("AMRD: invalid combination of AMRD_max_t_interp_order and AMRD_num_evo_tl","");

         // vertex centered variables

         for (j=0; j<AMRD_g_size; j++) if (AMRD_cmask[j]==AMRD_CMASK_OFF) {
           for (k=0; k<AMRD_num_hyperbolic_vars; k++) if (PAMR_var_type(AMRD_hyperbolic_vars[k]) == PAMR_VERTEX_CENTERED) {
             for (i=2; i<=AMRD_num_evo_tl; i++) {
               if (i==AMRD_num_evo_tl) tl=1; else tl=i+1;

               if (io==2) fn[tl-1]=(1-(i-1)*frac)*(AMRD_AMR_f[tnp1-1][k])[j]+(i-1)*frac*(AMRD_AMR_f[tn-1][k])[j];
               else {
                 m=rho-(i-1);
                 fn[tl-1]=(-m*(rho-m)/((real)(2*rho*rho)))*(AMRD_AMR_f[tnm1-1][k])[j]+
                       (m+rho)*(rho-m)/((real)(rho*rho))*(AMRD_AMR_f[tn-1][k])[j]+
                       (m+rho)*(m)/((real)(2*rho*rho))*(AMRD_AMR_f[tnp1-1][k])[j];
               }
             }

             for (i=0; i<AMRD_num_evo_tl; i++) if (i!=1) (AMRD_AMR_f[i][k])[j]=fn[i];
           }

           // ASSUMING elliptic vars are vertex centered

           for (k=0; k<AMRD_num_elliptic_vars; k++) {
              if (PAMR_var_type(AMRD_elliptic_vars[k]) != PAMR_VERTEX_CENTERED)
                 AMRD_stop("AMRD in regrid(): elliptic variables are assumed to be vertex-centered at present","");

              for (i=2; i<=AMRD_num_evo_tl; i++) {
                if (i==AMRD_num_evo_tl) tl=1; else tl=i+1;
                if (io==2) fn[tl-1]=(1-(i-1)*frac)*(AMRD_AMR_mgf[tnp1-1][k])[j]+(i-1)*frac*(AMRD_AMR_mgf[tn-1][k])[j];
                else {
                   m=rho-(i-1);
                   fn[tl-1]=(-m*(rho-m)/((real)(2*rho*rho)))*(AMRD_AMR_mgf[tnm1-1][k])[j]+
                     (m+rho)*(rho-m)/((real)(rho*rho))*(AMRD_AMR_mgf[tn-1][k])[j]+
                     (m+rho)*(m)/((real)(2*rho*rho))*(AMRD_AMR_mgf[tnp1-1][k])[j];
                }
              }

              for (i=0; i<AMRD_num_evo_tl; i++) if (i!=1) (AMRD_AMR_mgf[i][k])[j]=fn[i];
           }
         }

         // cell centered variables

         for (j=0; j<AMRD_g_size_c; j++) if (AMRD_cmask_c[j]==AMRD_CMASK_OFF) {
           for (k=0; k<AMRD_num_hyperbolic_vars; k++) if (PAMR_var_type(AMRD_hyperbolic_vars[k]) == PAMR_CELL_CENTERED) {
             for (i=2; i<=AMRD_num_evo_tl; i++) {

               if (i==AMRD_num_evo_tl) tl=1; else tl=i+1;

               if (io==2) fn[tl-1]=(1-(i-1)*frac)*(AMRD_AMR_f[tnp1-1][k])[j]+(i-1)*frac*(AMRD_AMR_f[tn-1][k])[j];
               else {
                  m=rho-(i-1);
                  fn[tl-1]=(-m*(rho-m)/((real)(2*rho*rho)))*(AMRD_AMR_f[tnm1-1][k])[j]+
                       (m+rho)*(rho-m)/((real)(rho*rho))*(AMRD_AMR_f[tn-1][k])[j]+
                       (m+rho)*(m)/((real)(2*rho*rho))*(AMRD_AMR_f[tnp1-1][k])[j];
               }
             }

             for (i=0; i<AMRD_num_evo_tl; i++) if (i!=1) (AMRD_AMR_f[i][k])[j]=fn[i];
           }
         }

         valid=PAMR_next_g();
      }
   }

   //--------------------------------------------------------------------------
   // call post regrid function, and
   // KO smooth new levels if desired...
   //--------------------------------------------------------------------------
   if (AMRD_rg_eps_diss!=0 && AMRD_num_rg_diss_vars>0) 
      IFL0 printf("       smoothing %i grid functions\n",AMRD_tnum_rg_diss_vars);
   mask_off=AMRD_CMASK_OFF; 
   for (i=max(3,lev+1); i<=Lf; i++)
   {
      set_cmask_bdy(i,PAMR_AMRH);
      call_app(app_post_regrid,i,PAMR_AMRH);
      if (AMRD_rg_eps_diss!=0 && AMRD_num_rg_diss_vars>0)
      {
         valid=PAMR_init_s_iter(i,PAMR_AMRH,0);
         while(valid)
         {
            ev_ldptr();
            for (j=0; j<AMRD_tnum_rg_diss_vars;j++) 
            {
               for (k=0; k<2*AMRD_dim; k++) if (AMRD_g_phys_bdy[k]) pbt[k]=AMRD_rg_diss_pbt[j][k]; else pbt[k]=PAMR_UNKNOWN;
               c_eps=&AMRD_rg_eps_diss; if (AMRD_do_ex<0) c_eps=AMRD_chr;
               for (stride=AMRD_diss_stride; stride>0; stride--)
                  dmdiss3d_(AMRD_rg_diss[j],AMRD_w1,c_eps,&AMRD_diss_bdy,
                         pbt,&even,&odd,
                         AMRD_cmask,&mask_off,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                         AMRD_chr,&AMRD_ex,&AMRD_do_ex,&AMRD_diss_use_6th_order,&stride);
            }
            valid=PAMR_next_g();
         }
      }
   }


   //---------------------------------------------------------------------------
   // Reset the flux correction mask
   //---------------------------------------------------------------------------
   for (i=lev-1; i<=Lf; i++) {
     set_fc_mask(i);
   }

   if (resolve)
   {
      //-----------------------------------------------------------------------
      // sync vars, incase of boundary-interpolation differences, and 
      // differences that may have been introduced via smoothing
      // (and note that all tl's are communicated in PAMR_sync, as set in
      //  set_regrid_sync);
      //-----------------------------------------------------------------------
      set_regrid_sync();
      // PAMR_set_trace_lev(3); // FP test
      for (i=max(3,lev+1); i<=Lf; i++) PAMR_sync(i,1,PAMR_AMRH,0);  
      // PAMR_set_trace_lev(0);

      PAMR_thaw_tf_bits();
      syncd=1;

      if (lev==2) 
      {
         for (i=1; i<=Lf; i++) PAMR_c_to_v(i,2,PAMR_AMRH,0);
         solve_elliptics(1,2,0); 
      }
      else
      {
         for (i=lev; i<=Lf; i++) PAMR_c_to_v(i,2,PAMR_AMRH,0);
         solve_elliptics(lev,2,0);
      }
   }

   for (i=0; i<PAMR_MAX_LEVS; i++) { if (old_bbox[i]) free(old_bbox[i]); }

   if (AMRD_regrid_script==AMRD_READ_REGRID_SCRIPT)
   {
      if (rgs_gbbox) free(rgs_gbbox); rgs_gbbox=0;
      if (rgs_glev) free(rgs_glev); rgs_glev=0;
      rgs_gnum=0;
   }
   
   if (trace) evo_dump(lev,Lf,"evo_after_regrid_",0,0);
}

//=============================================================================
// computes the set of f_tre variables for level lev, 
// by subtracting f(lev)-f(lev-1), at time level 2. 
// This routine uses the f_tre variables at
// lev-1 for work variables, and hence invalidates the TRE there.
//=============================================================================
void compute_ssh_tre(int lev)
{
   int valid,i,j;

   real t;
   t=PAMR_get_time(lev);

   if (!AMRD_using_cc_tre) 
   {
     if (lev<=1) return;
     AMRD_tre_valid[lev-1]=1;
     AMRD_tre_valid[lev-2]=0;
     
     valid=PAMR_init_s_iter(lev-1,PAMR_AMRH,0);
     while(valid)
     {
         ev_ldptr();
         for (i=0; i<AMRD_num_f_tre_vars; i++)
           for (j=0; j<AMRD_g_size; j++) (AMRD_f_tre[i])[j]=0;
         valid=PAMR_next_g();
     }
     
     valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
     while(valid)
     {
       ev_ldptr();
       for (i=0; i<AMRD_num_f_tre_vars; i++) {
         if (PAMR_var_type(AMRD_f_tre_vars[i])==PAMR_VERTEX_CENTERED) {
           for (j=0; j<AMRD_g_size; j++) (AMRD_f_tre[i])[j]=(AMRD_AMR_f[1][AMRD_f_tre_fn[i]])[j];
         } else {
           for (j=0; j<AMRD_g_size_c; j++) (AMRD_f_tre[i])[j]=(AMRD_AMR_f[1][AMRD_f_tre_fn[i]])[j];
         }
       }
       valid=PAMR_next_g();
     }
     
     PAMR_freeze_tf_bits();
     PAMR_clear_tf_bits();
     for (i=0; i<AMRD_num_f_tre_vars; i++) PAMR_set_tf_bit(AMRD_f_tre_gfn[i],PAMR_STRAIGHT_INJECT);
     PAMR_inject(lev,1,PAMR_AMRH);
     
     // only compute where AMRD_f_tre[i]!=0, as this is where parent is valid. Usually
     // not a problem, but can be for periodic boundaries where the child only touches the
     // left boundary and doesn't wrap
     valid=PAMR_init_s_iter(lev-1,PAMR_AMRH,0);
     while(valid)
     {
       ev_ldptr();
       for (i=0; i<AMRD_num_f_tre_vars; i++) 
       {
         if (PAMR_var_type(AMRD_f_tre_vars[i])==PAMR_VERTEX_CENTERED) 
         {
           for (j=0; j<AMRD_g_size; j++) if ((AMRD_f_tre[i])[j]!=0) (AMRD_f_tre[i])[j]-=(AMRD_AMR_f[1][AMRD_f_tre_fn[i]])[j];
         } 
         else 
         {
           for (j=0; j<AMRD_g_size_c; j++) if ((AMRD_f_tre[i])[j]!=0) (AMRD_f_tre[i])[j]-=(AMRD_AMR_f[1][AMRD_f_tre_fn[i]])[j];
         }
       }
       valid=PAMR_next_g();
     }
     
     call_app(app_scale_tre,lev-1,PAMR_AMRH);
     
     for (i=0; i<AMRD_num_f_tre_vars; i++) PAMR_set_tf_bit(AMRD_f_tre_gfn[i],PAMR_SECOND_ORDER);
     PAMR_interp(lev-1,1,PAMR_AMRH);
     PAMR_thaw_tf_bits(); 
   } 
   else // using cc-tre
   {
     call_app(app_scale_tre,lev,PAMR_AMRH);
     AMRD_tre_valid[lev-1]=1;
     AMRD_tre_valid[lev-2]=1;
   }
}

void disable_t0_vars(void)
{
   int i;

   AMRD_is_t0=0;

   for (i=0; i<AMRD_num_elliptic_vars_t0; i++)
      PAMR_disable_var(AMRD_elliptic_vars_t0[i]);
}

void enable_t0_vars(void)
{
   int i;

   AMRD_is_t0=1;

   for (i=0; i<AMRD_num_elliptic_vars_t0; i++)
      PAMR_enable_var(AMRD_elliptic_vars_t0[i]);
}

//=============================================================================
// calculates the initial data and grid hierarchy
// id_method:
//    0 - generate initial hierarchy via evolution (using id_pl_method=0)
//    1 - generate initial hierarhcy by solving the elliptics only
//        (always use this if max_lev<=2)
//   -99 - read from regrid script
//
// id_pl_method: (past-level initialization), after initial hierarchy 
//               determined
//
//    0 - first order extrapolation (i.e. straight copy)
//    1 - backwards then forwards one coarse step
//    2 - backwards <n> small "global" steps then extrapolate 
//    3 - do nothing (assume app_t0_cnst_data_f() will do it after MG,
//                    or user will do it prior to first evolution step)
//
//        DO NOT USE 3 during a constrained evolution, as the
//        MG extrapolation variables will not get set correctly
//        (0 is probably not advisable then either)
//
// Also, assume AMRD_num_tl>=2, and tl=2 is the t=0 surface
//=============================================================================
void generate_id(void)
{
   int Lf,dL,L,i,tl,valid,j,L1;
   real t,lambda,tx,dt_fl,dt_cl,dx[PAMR_MAX_DIM];
   int rho_tm[PAMR_MAX_LEVS],rho_sp[PAMR_MAX_LEVS],rho_tm0[PAMR_MAX_LEVS];
   int pLf,TRE_norm_save;
   int ltrace=1;

   enable_t0_vars();

   if (AMRD_num_evo_tl<2) AMRD_stop("generate_id: error ... currently only works with AMRD_num_evo_tl>=2\n",0);
   if (AMRD_max_lev<=2) AMRD_id_method=1;
   if (AMRD_regrid_script==AMRD_READ_REGRID_SCRIPT) AMRD_id_method=-99;
   switch(AMRD_id_method)
   {
      case 0: if (my_rank==0) printf("\nUsing id_method=0 (evolution) to determine initial hierarchy\n"); break;
      case 1: if (my_rank==0) printf("\nUsing id_method=1 (elliptics) to determine initial hierarchy\n"); break;
      case -99: if (my_rank==0) printf("\nReading initial hierarchy from regrid script\n"); break;
      default: AMRD_stop("generate_id: error .. unknown id_method",0); 
   }

   if (AMRD_id_method==0 || AMRD_id_method==-99)
   {
      Lf=PAMR_get_max_lev(PAMR_AMRH);
      if (AMRD_id_method==-99)
      {
         if (!(rgs_read_next(&t,&L1,&Lf,&rgs_gnum,&rgs_glev,&rgs_gbbox)))
            AMRD_stop("generate_id: error reading regridding script","");
         if (fabs(t-AMRD_t0)>1e-15) AMRD_stop("generate_id: error ... invalid time from regridding script","");
         if (rgs_gnum>0 && rgs_Lf>AMRD_max_lev) AMRD_stop("generate_id: error ... max_lev < Lf from script","");
         if (rgs_gnum>0 && L1!=3) AMRD_stop("generate_id: regridding script error ... L1!=3","");
         if (rgs_gnum>0 && Lf<2) AMRD_stop("generate_id: regridding script error ... Lf<2","");
         if (rgs_gnum>0) regrid(2,0,0);
         Lf=PAMR_get_max_lev(PAMR_AMRH);
         if (!(rgs_read_next(&rgs_t,&rgs_L1,&rgs_Lf,&rgs_gnum,&rgs_glev,&rgs_gbbox)))
         {
            if (my_rank==0)
               printf("generate_id: error reading next-step info from regridding script ... turning off script\n");
            AMRD_regrid_script=AMRD_NO_REGRID_SCRIPT;
         }
         dL=0;
      }
      else dL=Lf;

      while(dL>=0)
      {
         for (L=1; L<=Lf; L++) 
         {
            PAMR_set_time(L,AMRD_t0);
            if (AMRD_id_method==-99) call_app(app_AMRH_var_clear,L,PAMR_AMRH);
            call_app(app_free_data,L,PAMR_AMRH);
            PAMR_sync(L,2,PAMR_AMRH,0);
            zero_f_tre(L);
            AMRD_lsteps[L-1]=0;
            AMRD_tre_valid[L-1]=0;

            if (AMRD_c_tag || AMRD_v_tag) {
              PAMR_c_to_v(L,2,PAMR_AMRH,0);
            }
            sync_free_data(L);
            if (L<Lf) interp_free_data(L);
         }
         for (L=Lf; L>1; L--) inject_free_data(L);

         IFL0 printf("\ngenerate_id: reset initial data ... solving elliptics\n");
         init_rest();
         solve_elliptics(1,2,0);

         if (AMRD_calc_global_var_norms) calc_global_norms();
         if (dL>0)
         {
            dL=-1;
            if (Lf<AMRD_max_lev && AMRD_max_lev>2)
            {
               IFL0 printf("\ngenerate_id: evolving 1 coarse grid step. Lf=%i\n",Lf);
               AMRD_is_t0=0; evolve(1,1); AMRD_is_t0=1;
               pLf=Lf;
               regrid(AMRD_regrid_min_lev,0,0);
               Lf=PAMR_get_max_lev(PAMR_AMRH);
               dL=max(0,Lf-pLf);
               gh_stats();
            }
         } else dL=-1;
      }
   }
   else if (AMRD_id_method==1)
   {
      Lf=PAMR_get_max_lev(PAMR_AMRH);
      dL=Lf;
      while(dL)
      {
         for (L=1; L<=Lf; L++) 
         {
            call_app(app_free_data,L,PAMR_AMRH);
            PAMR_sync(L,2,PAMR_AMRH,0);
            zero_f_tre(L);
            if (AMRD_c_tag || AMRD_v_tag) {
              PAMR_c_to_v(L,1,PAMR_AMRH,0);
              PAMR_c_to_v(L,2,PAMR_AMRH,0);
            }
            sync_free_data(L);
            if (L<Lf) interp_free_data(L);
         }
         for (L=Lf; L>1; L--) inject_free_data(L);


         IFL0 printf("\ngenerate_id: reset initial data ... re-solving elliptics\n");
         if (AMRD_num_f_tre_vars) {
           solve_elliptics(1,2,1);
         } else {
           solve_elliptics(1,2,0);
         }

         if (AMRD_calc_global_var_norms) calc_global_norms();

         dL=0;
         if (Lf<AMRD_max_lev && AMRD_max_lev>2)
         {
            pLf=Lf;
            TRE_norm_save=AMRD_TRE_norm;
            AMRD_TRE_norm=0;
            regrid(AMRD_regrid_min_lev,1,0);
            AMRD_TRE_norm=TRE_norm_save;
            Lf=PAMR_get_max_lev(PAMR_AMRH);
            dL=Lf-pLf;
         }
      }
      init_rest();
   }

   if (AMRD_id_pl_method==0) 
   {

      IFL0 printf("\ngenerate id: generating past level info by 1rst order extrapolation\n"
                    "             (i.e. straight-copy from current level)\n");
      // init_rest() above has already done this for us.
   }
   else if (AMRD_id_pl_method==3) 
   {

      IFL0 printf("\ngenerate id: assuming user has initialized past level info in app_t0_cnst_data_f()"
                  "\n(only called if there are initial elliptics to solve), or user will intialize"
                  "\npast level info during first evolution step\n");
   }
   else if (AMRD_id_pl_method==1)
   {
      IFL0 printf("\ngenerate id: initial hierarchy determined ... evolving backwards, \n"
                  "then forwards %i time step(s) to initialize past level information\n", AMRD_id_pl_steps);
      PAMR_get_lambda(&lambda);
      lambda=-lambda;
      PAMR_set_lambda(lambda);
      AMRD_is_t0=0; evolve(AMRD_id_pl_steps,1); 
      flip_dt();
      lambda=-lambda;
      PAMR_set_lambda(lambda);
      evolve(AMRD_id_pl_steps,1);
      AMRD_is_t0=1;
      IFL0 printf("\ngenerate id: resetting initial data, and re-solving elliptics\n");
      for (L=1; L<=Lf; L++) 
      {
         PAMR_set_time(L,AMRD_t0);
         call_app(app_free_data,L,PAMR_AMRH);
         PAMR_sync(L,2,PAMR_AMRH,0);
         zero_f_tre(L);
         AMRD_lsteps[L-1]=0;
         AMRD_tre_valid[L-1]=0;
         sync_free_data(L);
         if (L<Lf) interp_free_data(L);
      }
      for (L=Lf; L>1; L--) inject_free_data(L);

      solve_elliptics(1,2,0);
      if (AMRD_calc_global_var_norms) calc_global_norms();
      update_mg_extrap_vars(1,Lf);
   }
   else if (AMRD_id_pl_method==2)
   {
      if (AMRD_num_elliptic_vars>0) AMRD_stop("AMRD_id_pl_method==2 not yet tested with evolved elliptics\n","");
      if (AMRD_num_evo_tl>3) AMRD_stop("AMRD_id_pl_method==2 currently only works with AMRD_num_evo_tl 2 or 3\n","");
      PAMR_get_dxdt(Lf,dx,&dt_fl);
      PAMR_get_dxdt(1,dx,&dt_cl);
      IFL0 printf("\ngenerate id: initial hierarchy determined ... evolving backwards, \n"
                  "%i global time step(s) with dt=%lf (finest level dt=%lf) "
                  "to initialize past level information\n", 
                  AMRD_id_pl_steps,AMRD_id_pl_lambda*dt_fl,dt_fl);
      PAMR_get_lambda(&lambda);
      PAMR_set_lambda(-AMRD_id_pl_lambda*lambda*dt_fl/dt_cl);
      PAMR_get_rho(rho_sp,rho_tm,PAMR_MAX_LEVS);
      for (i=0;i<PAMR_MAX_LEVS;i++) rho_tm0[i]=1;
      PAMR_set_rho(rho_sp,rho_tm0,PAMR_MAX_LEVS);

      AMRD_is_t0=0; evolve(AMRD_id_pl_steps,1); 

      tx=PAMR_get_time(1);
      PAMR_set_rho(rho_sp,rho_tm,PAMR_MAX_LEVS);
      PAMR_set_lambda(lambda);
      AMRD_is_t0=1;
      id_pl_2_save();
      IFL0 printf("\ngenerate id: resetting initial data, and re-solving elliptics\n");
      for (L=1; L<=Lf; L++) 
      {
         PAMR_set_time(L,AMRD_t0);
         call_app(app_free_data,L,PAMR_AMRH);
         PAMR_sync(L,2,PAMR_AMRH,0);
         zero_f_tre(L);
         AMRD_lsteps[L-1]=0;
         AMRD_tre_valid[L-1]=0;
         sync_free_data(L);
         if (L<Lf) interp_free_data(L);
      }
      for (L=Lf; L>1; L--) inject_free_data(L);

      solve_elliptics(1,2,0);
      if (AMRD_calc_global_var_norms) calc_global_norms();
      id_pl_2_init(tx);
   }
   else
   {
       AMRD_stop("invalid AMRD_id_pl_method\n","");
   }

   if (AMRD_ID_DV_trace) 
   { 
      i=0; while(i<AMRD_num_hyperbolic_vars) PAMR_save_gfn(AMRD_hyperbolic_vars[i++],PAMR_AMRH,2,-1,-1.0,"ID_","");
      i=0; while(i<AMRD_num_elliptic_vars) PAMR_save_gfn(AMRD_elliptic_vars[i++],PAMR_AMRH,2,-1,-1.0,"ID_","");
   }

   if (AMRD_regrid_script==AMRD_WRITE_REGRID_SCRIPT) 
   {
      if (Lf<2) AMRD_stop("Error ... cannot write regrid script with < 2 levels\n","");
      if (Lf==2) 
      rgs_t=AMRD_t0; rgs_Lf=Lf; rgs_L1=3;
      if (!(rgs_write_next(rgs_t,rgs_L1,rgs_Lf,rgs_gnum,rgs_glev,rgs_gbbox)))
      {
         printf("generate_id: error writing info to regridding script ... turning off script\n");
         AMRD_regrid_script=AMRD_NO_REGRID_SCRIPT;
      }
   }

   disable_t0_vars();

   //-----------------------------------------------------------------
   // set the flux correction mask
   //-----------------------------------------------------------------
   for (L=2; L<=Lf; L++) set_fc_mask(L);

   if (AMRD_evo_trace && my_rank==0) printf("\nInitial data calculated\n");
   gh_stats();
   if (AMRD_evo_trace && my_rank==0)  printf("%s\n",line_break);
}

//---------------------------------------------------------------------
// initializes non-tl=2 fields (including extrapolation info)
// by copying data from tl=2
//---------------------------------------------------------------------
void init_rest(void)
{
   int L,i,j,tl,Lf,valid;

   Lf=PAMR_get_max_lev(PAMR_AMRH);

   if (AMRD_id_pl_method==3)
   {
      for (L=1; L<=Lf; L++)
         for (tl=2; tl<AMRD_num_evo_tl; tl++)
            PAMR_sync(L,tl+1,PAMR_AMRH,0); 
      return;
   }

   for (L=1; L<=Lf; L++)
   {
      valid=PAMR_init_s_iter(L,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
           for (tl=0; tl<AMRD_num_evo_tl; tl++) {
             if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
               for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_f[tl][i])[j]=(AMRD_AMR_f[1][i])[j];
             } else {
               for (j=0; j<AMRD_g_size_c; j++) (AMRD_AMR_f[tl][i])[j]=(AMRD_AMR_f[1][i])[j];
             }
           }
         }
         for (i=0; i<AMRD_num_elliptic_vars; i++)
         {
            if (!AMRD_id_user_mg_pl)
               for (tl=0; tl<AMRD_num_evo_tl; tl++)
                  for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf[tl][i])[j]=(AMRD_AMR_mgf[1][i])[j];
            for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf_extrap_tm1[i])[j]=(AMRD_AMR_mgf[1][i])[j];
            for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf_extrap_tm2[i])[j]=(AMRD_AMR_mgf[1][i])[j];
         }
         valid=PAMR_next_g();
      }
   }
}

//---------------------------------------------------------------------
// saves tn=2 vars (current) to tn=3 for evolution equations,
// and tn=2 elliptic variables to extrap_tm1 ... for use with 
// id_pl_method=2
//---------------------------------------------------------------------
void id_pl_2_save(void)
{
   int L,i,j,tl,Lf,valid;

   Lf=PAMR_get_max_lev(PAMR_AMRH);
   for (L=1; L<=Lf; L++)
   {
      valid=PAMR_init_s_iter(L,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         if (AMRD_num_evo_tl>2)
         {
           for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
             if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
               for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_f[2][i])[j]=(AMRD_AMR_f[1][i])[j];
             } else {
               for (j=0; j<AMRD_g_size_c; j++) (AMRD_AMR_f[2][i])[j]=(AMRD_AMR_f[1][i])[j];
             }
           }
         }
         if (AMRD_num_evo_tl>1)
         {
            for (i=0; i<AMRD_num_elliptic_vars; i++)
            {
               for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf_extrap_tm1[i])[j]=(AMRD_AMR_mgf[1][i])[j];
            }
         }
         valid=PAMR_next_g();
      }
   }
}

//---------------------------------------------------------------------
// initializes past level (tl=3) and mg extrap info for id_pl_method=2
//
// tx is the past time that was evolved to
//
// prior to call, tn=2 contains t=AMRD_t0 data, and tn=3 (or extrap_m1) 
// t=tx data
//---------------------------------------------------------------------
void id_pl_2_init(real tx)
{
   int lev,i,j,rho_tm[PAMR_MAX_LEVS],rho_sp[PAMR_MAX_LEVS],rho;
   int valid,Lf;
   real f,dt,dx[PAMR_MAX_DIM];

   Lf=PAMR_get_max_lev(PAMR_AMRH);

   PAMR_get_rho(rho_sp,rho_tm,AMRD_max_lev);

   for (lev=1; lev<=Lf; lev++)
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      PAMR_get_dxdt(lev,dx,&dt);
      while(valid)
      {
         ev_ldptr();
         if (AMRD_num_evo_tl==3)
         {
            f=(AMRD_t0-tx)/dt;
            for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
              if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
                for (j=0; j<AMRD_g_size; j++) {
                  (AMRD_AMR_f[2][i])[j]=((AMRD_AMR_f[2][i])[j]-(AMRD_AMR_f[1][i])[j]*(1-f))/f;
                }
              } else {
                for (j=0; j<AMRD_g_size_c; j++) {
                  (AMRD_AMR_f[2][i])[j]=((AMRD_AMR_f[2][i])[j]-(AMRD_AMR_f[1][i])[j]*(1-f))/f;
                }
              }
            }
         }

         for (i=0; i<AMRD_num_elliptic_vars; i++)
         {
            if (lev==1) rho=rho_tm[0]; else rho=rho_tm[lev-2];
            f=(AMRD_t0-tx)/rho/dt;
            for (j=0; j<AMRD_g_size; j++) 
            {
               (AMRD_AMR_mgf_extrap_tm2[i])[j]=((AMRD_AMR_mgf_extrap_tm1[i])[j]-(AMRD_AMR_mgf[1][i])[j]*(1-f))/f;
               (AMRD_AMR_mgf_extrap_tm1[i])[j]=(AMRD_AMR_mgf[1][i])[j];
            }
         }
         valid=PAMR_next_g();
      }
   }
}

//---------------------------------------------------------------------
// 'flips' times levels for reversing the direction of integration
// assumes time sequence is tl=n,1,2,...,n-1
//---------------------------------------------------------------------
#define SECOND_ORDER_ONLY 1
void flip_dt()
{
   int L,Lf,i,j,tl,valid;
   real f1;

   if (AMRD_num_evo_tl>3) AMRD_stop("flip_dt ... not yet updated for num_tl>3\n","");

   Lf=PAMR_get_max_lev(PAMR_AMRH);
   for (L=1; L<=Lf; L++)
   {
      valid=PAMR_init_s_iter(L,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         if (AMRD_num_evo_tl==2)
         {
           for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
             if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
               for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_f[0][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[0][i])[j];
             } else {
               for (j=0; j<AMRD_g_size_c; j++) (AMRD_AMR_f[0][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[0][i])[j];
             }
           }
           for (i=0; i<AMRD_num_elliptic_vars; i++) {
             for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf[0][i])[j]=2*(AMRD_AMR_mgf[1][i])[j]-(AMRD_AMR_mgf[0][i])[j];
           }
         }
         else if (AMRD_num_evo_tl==3)
         {
           for (i=0; i<AMRD_num_hyperbolic_vars; i++) {
             if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
               for (j=0; j<AMRD_g_size; j++) 
                 {
                   if (SECOND_ORDER_ONLY) 
                     {
                       (AMRD_AMR_f[2][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[2][i])[j];
                       (AMRD_AMR_f[0][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[0][i])[j];
                     }
                   else
                     {
                       f1=3*((AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[2][i])[j])+(AMRD_AMR_f[0][i])[j]; 
                       (AMRD_AMR_f[0][i])[j]=6*(AMRD_AMR_f[1][i])[j]-8*(AMRD_AMR_f[2][i])[j]+3*(AMRD_AMR_f[0][i])[j];
                       (AMRD_AMR_f[2][i])[j]=f1;
                     }
                 }
             } else {
               for (j=0; j<AMRD_g_size_c; j++) 
                 {
                   if (SECOND_ORDER_ONLY) 
                     {
                       (AMRD_AMR_f[2][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[2][i])[j];
                       (AMRD_AMR_f[0][i])[j]=2*(AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[0][i])[j];
                     }
                   else
                     {
                       f1=3*((AMRD_AMR_f[1][i])[j]-(AMRD_AMR_f[2][i])[j])+(AMRD_AMR_f[0][i])[j]; 
                       (AMRD_AMR_f[0][i])[j]=6*(AMRD_AMR_f[1][i])[j]-8*(AMRD_AMR_f[2][i])[j]+3*(AMRD_AMR_f[0][i])[j];
                       (AMRD_AMR_f[2][i])[j]=f1;
                     }
                 }
             }
           }
            for (i=0; i<AMRD_num_elliptic_vars; i++)
               for (j=0; j<AMRD_g_size; j++) 
               {
                  if (SECOND_ORDER_ONLY) 
                  {
                     (AMRD_AMR_mgf[2][i])[j]=2*(AMRD_AMR_mgf[1][i])[j]-(AMRD_AMR_mgf[2][i])[j];
                     (AMRD_AMR_mgf[0][i])[j]=2*(AMRD_AMR_mgf[1][i])[j]-(AMRD_AMR_mgf[0][i])[j];
                  }
                  else
                  {
                     f1=3*((AMRD_AMR_mgf[1][i])[j]-(AMRD_AMR_mgf[2][i])[j])+(AMRD_AMR_mgf[0][i])[j]; 
                     (AMRD_AMR_mgf[0][i])[j]=6*(AMRD_AMR_mgf[1][i])[j]-8*(AMRD_AMR_mgf[2][i])[j]+3*(AMRD_AMR_mgf[0][i])[j];
                     (AMRD_AMR_mgf[2][i])[j]=f1;
                  }
               }
         }
         for (i=0; i<AMRD_num_elliptic_vars; i++)
         {
            for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_mgf_extrap_tm2[i])[j]=2*(AMRD_AMR_mgf_extrap_tm1[i])[j]-
                                                                          (AMRD_AMR_mgf_extrap_tm2[i])[j];
         }
         valid=PAMR_next_g();
      }
   }
}

//=============================================================================
// fills the data pointers for current grid (io.c initialized gfn's)
//=============================================================================
void AMRD_ev_ldptr(void)
{
   ev_ldptr();
}

void ev_ldptr(void)
{
  int ngfs,i,j;
   real *gfs[PAMR_MAX_GFNS],t;

   AMRD_g_shape[1]=AMRD_g_shape[2]=1;
   AMRD_g_shape_c[0]=AMRD_g_shape_c[1]=AMRD_g_shape_c[2]=1;
   if (!(PAMR_get_g_attribs(&AMRD_g_rank,&AMRD_g_dim,AMRD_g_shape,AMRD_g_shape_c,AMRD_g_bbox,AMRD_g_ghost_width,&t,&ngfs,AMRD_g_x,AMRD_g_x_c,gfs)))
      AMRD_stop("mg_ldptr: PAMR_get_g_attribs failed\n","");
   AMRD_g_size=1; AMRD_g_Nx=AMRD_g_shape[0]; AMRD_g_Ny=AMRD_g_shape[1]; AMRD_g_Nz=AMRD_g_shape[2];
   for (i=0; i<AMRD_g_dim; i++) { AMRD_g_size*=AMRD_g_shape[i]; AMRD_g_dx[i]=(AMRD_g_x[i])[2]-(AMRD_g_x[i])[1]; }
   AMRD_g_size_c = 1;
   AMRD_g_Nx_c = AMRD_g_shape_c[0];
   AMRD_g_Ny_c = AMRD_g_shape_c[1];
   AMRD_g_Nz_c = AMRD_g_shape_c[2];
   if (AMRD_g_Nx_c < 1) AMRD_g_Nx_c = 1;
   if (AMRD_g_Ny_c < 1) AMRD_g_Ny_c = 1;
   if (AMRD_g_Nz_c < 1) AMRD_g_Nz_c = 1;

   for (i=0; i<AMRD_g_dim; i++) { AMRD_g_size_c*=AMRD_g_shape_c[i]; }
   for (i=0; i<AMRD_num_f_tre_vars; i++) AMRD_f_tre[i]=gfs[AMRD_f_tre_gfn[i]-1];
   for (i=0; i<AMRD_num_tnp1_diss_vars; i++) AMRD_tnp1_diss[i]=gfs[AMRD_tnp1_diss_gfn[i]-1];
   for (i=0; i<AMRD_num_tn_diss_vars; i++) AMRD_tn_diss[i]=gfs[AMRD_tn_diss_gfn[i]-1];
   for (i=0; i<AMRD_num_tnp1_liipb_vars; i++) AMRD_tnp1_liipb[i]=gfs[AMRD_tnp1_liipb_gfn[i]-1];
   for (i=0; i<AMRD_num_tnp1_liiab_vars; i++) AMRD_tnp1_liiab[i]=gfs[AMRD_tnp1_liiab_gfn[i]-1];
   for (i=0; i<AMRD_num_tnp1_liibb_vars; i++) AMRD_tnp1_liibb[i]=gfs[AMRD_tnp1_liibb_gfn[i]-1];
   for (i=0; i<AMRD_tnum_rg_diss_vars; i++) AMRD_rg_diss[i]=gfs[AMRD_rg_diss_gfn[i]-1];
   for (i=0; i<AMRD_num_interp_AMR_bdy_vars; i++) AMRD_interp_AMR_bdy_f[i]=gfs[AMRD_interp_AMR_bdy_f_gfn[i]-1];
   for (i=0; i<AMRD_num_work_repop_vars; i++) AMRD_work_repop[i]=gfs[AMRD_work_repop_gfn[i]-1];
   for (j=0; j<AMRD_num_evo_tl; j++) 
   {
      for (i=0; i<AMRD_num_hyperbolic_vars; i++) AMRD_AMR_f[j][i]=gfs[AMRD_AMR_f_gfn[j][i]-1];
      for (i=0; i<AMRD_num_elliptic_vars; i++) 
      {
         AMRD_AMR_mgf[j][i]=gfs[AMRD_AMR_mgf_gfn[j][i]-1];
         AMRD_AMR_mgf_extrap_tm1[i]=gfs[AMRD_AMR_mgf_extrap_tm1_gfn[i]-1];
         AMRD_AMR_mgf_extrap_tm2[i]=gfs[AMRD_AMR_mgf_extrap_tm2_gfn[i]-1];
         AMRD_MG_brs[i]=gfs[AMRD_MG_brs_gfn[i]-1];
      }
   }
   if ((AMRD_g_bbox[0]-AMRD_base_bbox[0])<AMRD_g_dx[0]/2) AMRD_g_phys_bdy[0]=1; else AMRD_g_phys_bdy[0]=0;
   if ((AMRD_base_bbox[1]-AMRD_g_bbox[1])<AMRD_g_dx[0]/2) AMRD_g_phys_bdy[1]=1; else AMRD_g_phys_bdy[1]=0;
   if (AMRD_g_dim>1)
   {
      if ((AMRD_g_bbox[2]-AMRD_base_bbox[2])<AMRD_g_dx[1]/2) AMRD_g_phys_bdy[2]=1; else AMRD_g_phys_bdy[2]=0;
      if ((AMRD_base_bbox[3]-AMRD_g_bbox[3])<AMRD_g_dx[1]/2) AMRD_g_phys_bdy[3]=1; else AMRD_g_phys_bdy[3]=0;
      if (AMRD_g_dim>2)
      { 
         if ((AMRD_g_bbox[4]-AMRD_base_bbox[4])<AMRD_g_dx[2]/2) AMRD_g_phys_bdy[4]=1; else AMRD_g_phys_bdy[4]=0;
         if ((AMRD_base_bbox[5]-AMRD_g_bbox[5])<AMRD_g_dx[2]/2) AMRD_g_phys_bdy[5]=1; else AMRD_g_phys_bdy[5]=0;
      }   
   }

   if (AMRD_cmask_gfn) AMRD_cmask=gfs[AMRD_cmask_gfn-1]; else AMRD_cmask=0;
   if (AMRD_cmask_c_gfn) AMRD_cmask_c=gfs[AMRD_cmask_c_gfn-1]; else AMRD_cmask_c=0;
   if (AMRD_fc_mask_gfn) AMRD_fc_mask=gfs[AMRD_fc_mask_gfn-1]; else AMRD_fc_mask = 0;
   if (AMRD_tre_gfn) AMRD_tre=gfs[AMRD_tre_gfn-1]; else AMRD_tre = 0;
   if (AMRD_tre_c_gfn) AMRD_tre_c=gfs[AMRD_tre_c_gfn-1]; else AMRD_tre_c_gfn = 0;
   AMRD_w1=gfs[AMRD_w1_gfn-1];
   AMRD_w1_c=gfs[AMRD_w1_c_gfn-1];

   if (AMRD_do_ex) 
   {
      if (AMRD_chr_gfn) AMRD_chr=gfs[AMRD_chr_gfn-1]; else AMRD_chr=AMRD_cmask; 
      if (AMRD_chr_c_gfn) AMRD_chr_c=gfs[AMRD_chr_c_gfn-1]; else AMRD_chr_c=AMRD_cmask_c; 
   }
}

//=============================================================================
// the following sets cmask to ON, for the current grid (i.e. call ev_ldptr()
// before!), in the region specified by the bbox list
//=============================================================================
void set_cmask_oldgs(real *bbox, int num)
{
   int i,j,k,n,is,ie,js,je,ks,ke;

   for (i=0; i<AMRD_g_size; i++)   AMRD_cmask[i]=AMRD_CMASK_OFF;
   for (i=0; i<AMRD_g_size_c; i++) AMRD_cmask_c[i]=AMRD_CMASK_OFF;

   is=ie=js=je=ks=ke=1;

   for (n=0; n<num; n++)
   {
      js=je=ks=ke=0;
      is=max((bbox[n*2*AMRD_dim]-AMRD_g_bbox[0])/AMRD_g_dx[0]+0.5,0);
      ie=min((bbox[n*2*AMRD_dim+1]-AMRD_g_bbox[0])/AMRD_g_dx[0]+0.5,AMRD_g_Nx-1);
      if (AMRD_dim>1)
      {
         js=max((bbox[n*2*AMRD_dim+2]-AMRD_g_bbox[2])/AMRD_g_dx[1]+0.5,0);
         je=min((bbox[n*2*AMRD_dim+3]-AMRD_g_bbox[2])/AMRD_g_dx[1]+0.5,AMRD_g_Ny-1);
         if (AMRD_g_dim>2)
         {
            ks=max((bbox[n*2*AMRD_dim+4]-AMRD_g_bbox[4])/AMRD_g_dx[2]+0.5,0);
            ke=min((bbox[n*2*AMRD_dim+5]-AMRD_g_bbox[4])/AMRD_g_dx[2]+0.5,AMRD_g_Nz-1);
         }
      }

      // The following assumes that the cell centered cmask
      // covers exactly 1 fewer cells than vertices in each
      // direction.
      if (is<=ie && js<=je && ks<=ke)
      {
         for (i=is; i<=ie; i++)
         {
            for (j=js; j<=je; j++)
            {
               for (k=ks; k<=ke; k++)
               {
                  AMRD_cmask[i+j*AMRD_g_Nx+k*AMRD_g_Ny*AMRD_g_Nx]=AMRD_CMASK_ON; 
               } 
            } 
         }
         if (AMRD_g_dim<3) ke++;
         if (AMRD_g_dim<2) je++;
         for (i=is; i<ie; i++)
         {
            for (j=js; j<je; j++)
            {
               for (k=ks; k<ke; k++)
               {
                 AMRD_cmask_c[i+j*AMRD_g_Nx_c+k*AMRD_g_Ny_c*AMRD_g_Nx_c]=AMRD_CMASK_ON; 
               } 
            } 
         }
      }
   }
}

//=============================================================================
// copies the MG vars (at AMR time level 2) to brs, from level L1 to L2
//=============================================================================
void set_mg_brs_vars(int L1, int L2)
{
   int lev,i,j;
   int valid;

   for (lev=L1; lev<=L2; lev++)
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         for (i=0; i<AMRD_num_elliptic_vars; i++)
            for (j=0; j<AMRD_g_size; j++) (AMRD_MG_brs[i])[j]=(AMRD_AMR_mgf[1][i])[j];
         valid=PAMR_next_g();
      }
   }
}

//=============================================================================
// This function updates the extrapolation variables by:
//
// For L>L1 (implying L is in-sync with its parent)
//
//    o f_extrap_tm1 -> f_extrap_tm2
//    o f(tl=2) -> f_extrap_tm1
//    o if MG_eps_c>0 'correct' f_extrap_tm2 via 
//
//       f_e_tm2 -> f_e_tm1 - (f_brs - f_e_tm2) - AMRD_MG_eps_c(f_e_tm1 - f_brs)/rho_tm^n
//
//               =  f_e_tm2 + (f_e_tm1-f_brs)*(1-AMRD_MG_eps_c/rho_tm^n) 
//
//       where n is L-L1
//
//       (see thesis 2.70 --- MG_eps_c=1)
//
// If L1=1 then we only do the first two steps above
//=============================================================================
void update_mg_extrap_vars(int L1, int Lf)
{
   int lev,i,j,rho_tm[PAMR_MAX_LEVS],rho_sp[PAMR_MAX_LEVS],net_rho;
   int valid,ltrace=0;
   real f;

   IFL0 printf("update_mg_extrap_vars: L1=%i, Lf=%i\n",L1,Lf);

   PAMR_get_rho(rho_sp,rho_tm,AMRD_max_lev);

   if (L1>1) 
   {
      net_rho=rho_tm[L1-1];
      L1++;
   }
   else net_rho=1;

   for (lev=L1; lev<=Lf; lev++)
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         for (i=0; i<AMRD_num_elliptic_vars; i++)
         {
            for (j=0; j<AMRD_g_size; j++) 
            {
               (AMRD_AMR_mgf_extrap_tm2[i])[j]=(AMRD_AMR_mgf_extrap_tm1[i])[j];
               (AMRD_AMR_mgf_extrap_tm1[i])[j]=(AMRD_AMR_mgf[1][i])[j];
            }
            if (lev>1 && (AMRD_MG_eps_c !=0))
            {
               f=1.0-AMRD_MG_eps_c/((real)net_rho);
               for (j=0; j<AMRD_g_size; j++) 
                  (AMRD_AMR_mgf_extrap_tm2[i])[j]+=((AMRD_AMR_mgf_extrap_tm1[i])[j]-(AMRD_MG_brs[i])[j])*f;
            }
         }
         valid=PAMR_next_g();
      }
      net_rho*=rho_tm[L1-1];
   }
}

//=============================================================================
// The following interpolates the 'i-1' boundary, i.e. 1 point in from grid
// boundary, for desired variables
//=============================================================================
void ibnd_interp(int L)
{
   int valid,i;
   int phys_only=1;
   int amr_only=2;
   int both=3;

   valid=PAMR_init_s_iter(L,PAMR_AMRH,0);
   while(valid)
   {
      ev_ldptr();
      for (i=0; i<AMRD_num_tnp1_liipb_vars; i++) 
         linbnd3d_(AMRD_tnp1_liipb[i],AMRD_g_phys_bdy,&phys_only,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                   AMRD_chr,&AMRD_ex,&AMRD_do_ex);
      for (i=0; i<AMRD_num_tnp1_liiab_vars; i++) 
         linbnd3d_(AMRD_tnp1_liiab[i],AMRD_g_phys_bdy,&amr_only,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                   AMRD_chr,&AMRD_ex,&AMRD_do_ex);
      for (i=0; i<AMRD_num_tnp1_liibb_vars; i++) 
         linbnd3d_(AMRD_tnp1_liibb[i],AMRD_g_phys_bdy,&both,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                   AMRD_chr,&AMRD_ex,&AMRD_do_ex);
      valid=PAMR_next_g();
   }
}

//=============================================================================
// The following re-initializes the given grid function's AMR boundaries,
// at points that have no parent point, via interpolation from points that do.
//
// NOTE: assumes ev_ldptr() has been called ... and need to sync afterwards.
//       also, in fine grid boundaries are not align with coarse grid,
//       then the 'remainder' points are not interpolated (i.e., we do not
//       extrapolate)
//=============================================================================
void interp_AMR_bdy(real* f, real *work, int rhosp)
{
   int i,j,k,Nxc,Nyc,Nzc,one=1,h;
   int is,ie,js,je,ks,ke;

   if (AMRD_g_dim<2) return;

   if (!(AMRD_interp_AMR_bdy_order==2 || AMRD_interp_AMR_bdy_order==4))
      AMRD_stop("interp_AMR_bdy: only interp_AMR_bdy_order=2 or 4 currently supported\n","");

   //--------------------------------------------------------------------------
   // find largest bounding box aligned with coarse level
   //--------------------------------------------------------------------------
   h=((AMRD_g_x[0])[0]-AMRD_base_bbox[0])/AMRD_g_dx[0]+0.5;
   if (h%rhosp) is=(rhosp-(h%rhosp))+1; else is=1;
   h=((AMRD_g_x[0])[AMRD_g_Nx-1]-AMRD_base_bbox[0])/AMRD_g_dx[0]+0.5;
   ie=AMRD_g_Nx-(h%rhosp); 

   h=((AMRD_g_x[1])[0]-AMRD_base_bbox[2])/AMRD_g_dx[1]+0.5;
   if (h%rhosp) js=(rhosp-(h%rhosp))+1; else js=1;
   h=((AMRD_g_x[1])[AMRD_g_Ny-1]-AMRD_base_bbox[2])/AMRD_g_dx[1]+0.5;
   je=AMRD_g_Ny-(h%rhosp); 

   if (AMRD_g_dim>2)
   {
      h=((AMRD_g_x[2])[0]-AMRD_base_bbox[4])/AMRD_g_dx[2]+0.5;
      if (h%rhosp) ks=(rhosp-(h%rhosp))+1; else ks=1;
      h=((AMRD_g_x[2])[AMRD_g_Nz-1]-AMRD_base_bbox[4])/AMRD_g_dx[2]+0.5;
      ke=AMRD_g_Nz-(h%rhosp); 
   }
   else {ks=ke=1;}

   Nxc=(ie-is)/rhosp+1;
   Nyc=(je-js)/rhosp+1;
   Nzc=(ke-ks)/rhosp+1;

   dmcopy3d_(f,work,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,&Nxc,&Nyc,&Nzc,&is,&js,&ks,&one,&one,&one,
             &Nxc,&Nyc,&Nzc,&rhosp,&rhosp,&rhosp,&one,&one,&one);

   //-----------------------------------------------------------------------
   // xmin xmax ymin ymax zmin zmax faces:
   //-----------------------------------------------------------------------
   if (((AMRD_g_x[0])[0]-AMRD_base_bbox[0])>AMRD_g_dx[0]/2 || AMRD_periodic[0])
      dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                  &one,&one,&one,&is,&js,&ks,&one,&Nyc,&Nzc,
                  &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                  AMRD_chr,&AMRD_ex,&AMRD_do_ex);

   if ((AMRD_base_bbox[1]-(AMRD_g_x[0])[AMRD_g_Nx-1])>AMRD_g_dx[0]/2 || AMRD_periodic[0])
      dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                  &Nxc,&one,&one,&ie,&js,&ks,&one,&Nyc,&Nzc,
                  &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                  AMRD_chr,&AMRD_ex,&AMRD_do_ex);

   if (AMRD_g_dim>1)
   {
      if (((AMRD_g_x[1])[0]-AMRD_base_bbox[2])>AMRD_g_dx[1]/2 || AMRD_periodic[1])
         dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                     &one,&one,&one,&is,&js,&ks,&Nxc,&one,&Nzc,
                     &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                     AMRD_chr,&AMRD_ex,&AMRD_do_ex);

      if ((AMRD_base_bbox[3]-(AMRD_g_x[1])[AMRD_g_Ny-1])>AMRD_g_dx[1]/2 || AMRD_periodic[1])
         dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                     &one,&Nyc,&one,&is,&je,&ks,&Nxc,&one,&Nzc,
                     &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                     AMRD_chr,&AMRD_ex,&AMRD_do_ex);
   }
   
   if (AMRD_g_dim>2)
   {
      if (((AMRD_g_x[2])[0]-AMRD_base_bbox[4])>AMRD_g_dx[2]/2 || AMRD_periodic[2])
         dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                     &one,&one,&one,&is,&js,&ks,&Nxc,&Nyc,&one,
                     &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                     AMRD_chr,&AMRD_ex,&AMRD_do_ex);

      if ((AMRD_base_bbox[5]-(AMRD_g_x[2])[AMRD_g_Nz-1])>AMRD_g_dx[2]/2 || AMRD_periodic[2])
         dminterp3d_(work,f,&Nxc,&Nyc,&Nzc,&AMRD_g_Nx,&AMRD_g_Ny,&AMRD_g_Nz,
                     &one,&one,&Nzc,&is,&js,&ke,&Nxc,&Nyc,&one,
                     &one,&one,&one,&rhosp,&rhosp,&rhosp,&AMRD_interp_AMR_bdy_order,
                     AMRD_chr,&AMRD_ex,&AMRD_do_ex);
   }

   if (AMRD_do_ex) for (i=0; i<AMRD_g_size; i++) if (AMRD_chr[i]==AMRD_ex) f[i]=0;
}

//=============================================================================
// The following calculates a global norm of all hyperbolic and 
// elliptic variables at tn=1, and saves the result in the corresponding 
// location (and at tn=2,..., MG) in global_var_norms[]
//=============================================================================
real *AMRD_get_global_norms(void)
{
   return AMRD_global_var_norms;
}

void calc_global_norms()
{
   int lev,Lf,j,i,tl;
   int valid;

   real local_var_norms[PAMR_MAX_GFNS],cmax;

   if (AMRD_global_var_norm_type!=AMRD_GLOBAL_VAR_INF_NORM)
      AMRD_stop("global_var_norm_type: only the infinity norm is currently supported","");

   tl=1; if (AMRD_num_evo_tl<2) tl=0;

   Lf=PAMR_get_max_lev(PAMR_AMRH);

   for (i=0; i<PAMR_MAX_GFNS; i++) local_var_norms[i]=AMRD_global_var_norm_floor;

   for (lev=1; lev<=Lf; lev++)
   {
      valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
      while(valid)
      {
         ev_ldptr();
         for (i=0; i<AMRD_num_hyperbolic_vars; i++)
         {
            cmax=0;
            if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
              for (j=0; j<AMRD_g_size; j++) cmax=max(cmax,fabs((AMRD_AMR_f[tl][i])[j]));
            } else {
              for (j=0; j<AMRD_g_size_c; j++) cmax=max(cmax,fabs((AMRD_AMR_f[tl][i])[j]));
            }
            for (j=0; j<AMRD_num_evo_tl; j++) local_var_norms[AMRD_AMR_f_gfn[j][i]]=
                                              max(cmax,local_var_norms[AMRD_AMR_f_gfn[j][i]]);
            // MG level:
            local_var_norms[AMRD_AMR_f_gfn[AMRD_num_evo_tl-1][i]+1]=local_var_norms[AMRD_AMR_f_gfn[AMRD_num_evo_tl-1][i]];
         }
         for (i=0; i<AMRD_num_elliptic_vars; i++)
         {
            cmax=0;
            for (j=0; j<AMRD_g_size; j++) cmax=max(cmax,fabs((AMRD_AMR_mgf[tl][i])[j]));
            for (j=0; j<AMRD_num_evo_tl; j++) local_var_norms[AMRD_AMR_mgf_gfn[j][i]-1]=
                                              max(cmax,local_var_norms[AMRD_AMR_mgf_gfn[j][i]-1]);
            // MG level:
            local_var_norms[AMRD_AMR_mgf_gfn[AMRD_num_evo_tl-1][i]]=local_var_norms[AMRD_AMR_mgf_gfn[AMRD_num_evo_tl-1][i]-1];
         }
         valid=PAMR_next_g();
      }
   }

   MPI_Allreduce(local_var_norms,AMRD_global_var_norms,PAMR_MAX_GFNS,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD);
}
        
//=============================================================================
// for 'non-standard' communication (remember to PAMR_thaw... afterwards)
//=============================================================================
void set_gfn_sync(int gfn)
{
   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();
   PAMR_set_tf_bit(gfn,PAMR_SYNC);
}

void set_regrid_sync()
{
   int i,j;

   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();
   for (j=0; j<AMRD_num_evo_tl; j++)
   {
      for (i=0; i<AMRD_num_hyperbolic_vars; i++) PAMR_set_tf_bit(AMRD_AMR_f_gfn[j][i],PAMR_SYNC);
      for (i=0; i<AMRD_num_elliptic_vars; i++) 
      {
         if (j==0) PAMR_set_tf_bit(AMRD_AMR_mgf_extrap_tm1_gfn[i],PAMR_SYNC);
         if (j==0) PAMR_set_tf_bit(AMRD_AMR_mgf_extrap_tm2_gfn[i],PAMR_SYNC);
         PAMR_set_tf_bit(AMRD_AMR_mgf_gfn[j][i],PAMR_SYNC);
      }
   }
}

void set_gfn_in(int gfn, int in)
{
   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();
   PAMR_set_tf_bit(gfn,in);
}

//=============================================================================
// debug utility
//
// which = 0 : all
//       = 1 : elliptic only
//       = 2 : hyperbolic only
//       = 3 : elliptic & hyperbolic
//=============================================================================
void evo_dump(int L1, int L2, char *tag, int iter, int which)
{
   real t;
   int i,L;
   char buffer[512];
   real dt,dx[PAMR_MAX_DIM];

   if (!AMRD_evo_DV_trace) return;

   for (L=L1; L<=L2; L++)
   {
      PAMR_get_dxdt(L,dx,&dt);
      t=PAMR_get_time(L)+dt*iter/((real)AMRD_evo_max_iter+1)/10;
      if (AMRD_evo_DV_trace_t_on>t || AMRD_evo_DV_trace_t_off<t) return;

      if (!which || which==1 || which==3)
         for (i=0; i<AMRD_num_elliptic_vars; i++) 
         {
            PAMR_save_gfn(AMRD_elliptic_vars[i],PAMR_AMRH,1,L,t,tag,"");
            if (!which)
            {
               strcpy(buffer,AMRD_elliptic_vars[i]);
              AMRD_append_tag(buffer,"_extrap_tm1",AMRD_v_tag,AMRD_c_tag);
              PAMR_save_gfn(buffer,PAMR_AMRH,1,L,t,tag,"");

               strcpy(buffer,AMRD_elliptic_vars[i]); 
              AMRD_append_tag(buffer,"_extrap_tm2",AMRD_v_tag,AMRD_c_tag);
              PAMR_save_gfn(buffer,PAMR_AMRH,1,L,t,tag,"");

               strcpy(buffer,AMRD_elliptic_vars[i]); 
              AMRD_append_tag(buffer,"_brs",AMRD_v_tag,AMRD_c_tag);
              PAMR_save_gfn(buffer,PAMR_AMRH,1,L,t,tag,"");
            }
         }
      if (!which || which==2 || which==3)
         for (i=0; i<AMRD_num_hyperbolic_vars; i++) PAMR_save_gfn(AMRD_hyperbolic_vars[i],PAMR_AMRH,1,L,t,tag,"");
      if (!which)
      {
         for (i=0; i<AMRD_num_AMRH_work_vars; i++) PAMR_save_gfn(AMRD_AMRH_work_vars[i],PAMR_AMRH,1,L,t,tag,"");
         for (i=0; i<AMRD_num_f_tre_vars; i++) PAMR_save_gfn(AMRD_f_tre_vars[i],PAMR_AMRH,1,L,t,tag,"");
         PAMR_save_gfn("cmask",PAMR_AMRH,1,L,t,tag,"");
         PAMR_save_gfn("chr",PAMR_AMRH,1,L,t,tag,"");
      }

      if (!which || which==1 || which==3)
         for (i=0; i<AMRD_num_elliptic_vars; i++) PAMR_save_gfn(AMRD_elliptic_vars[i],PAMR_AMRH,2,L,t,tag,"");
      if (!which || which==2 || which==3)
         for (i=0; i<AMRD_num_hyperbolic_vars; i++) PAMR_save_gfn(AMRD_hyperbolic_vars[i],PAMR_AMRH,2,L,t,tag,"");
   }
}

//=============================================================================
// the following is experimental ... must be called just after compute_ssh,
// but before injections
//
// only applies to hyperbolic vars, needs at least two time levels
// (uses most-retarted time level as temporary storage, and so
//  presently is only compatable with AMRD_np1_initial_guess=1)
//=============================================================================
void richardson_extrap(int lev)
{
   int valid,i,j,k;
   int rho_sp[PAMR_MAX_LEVS],rho_tm[PAMR_MAX_LEVS];
   int rho_sp0,rho_tm0;
   int loc_size;
   real f_l,f_lm1,f_c;

   if (lev<=1) return;

   if (AMRD_np1_initial_guess!=1) AMRD_stop("AMRD error : AMRD_np1_initial_guess must equal 1 with magic cookie set\n","");
   if (AMRD_num_evo_tl<2) AMRD_stop("AMRD error : AMRD_num_evo_tl must be >= 2 with magic cookie set\n","");

   PAMR_get_rho(rho_sp,rho_tm,PAMR_MAX_LEVS);
   rho_sp0=rho_sp[lev-2]; rho_tm0=rho_tm[lev-2];

   for(i=0; i<AMRD_num_interp_AMR_bdy_vars; i++) PAMR_set_tf_bit(AMRD_interp_AMR_bdy_f_gfn[i],PAMR_SYNC);
   PAMR_sync(lev,1,PAMR_AMRH,0); PAMR_thaw_tf_bits();

   //=============================================================================
   // first --- save copy of current tl=2 (current) to tl 1 (most retarded),
   // mark for communication, and interpolate coarser level info to tl=2
   //=============================================================================
   PAMR_freeze_tf_bits(); PAMR_clear_tf_bits();
   valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
   while(valid)
   {
      ev_ldptr();
      for (i=0; i<AMRD_num_hyperbolic_vars; i++)
      {
        if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
          for (j=0; j<AMRD_g_size; j++) (AMRD_AMR_f[0][i])[j]=(AMRD_AMR_f[1][i])[j];
        } else {
          for (j=0; j<AMRD_g_size_c; j++) (AMRD_AMR_f[0][i])[j]=(AMRD_AMR_f[1][i])[j];
        }
        PAMR_set_tf_bit(AMRD_AMR_f_gfn[1][i],PAMR_SECOND_ORDER);
      }
      valid=PAMR_next_g();
   }
   PAMR_interp(lev-1,2,PAMR_AMRH);
   PAMR_thaw_tf_bits();

   //=============================================================================
   // now apply correction, assuming a second order accurate scheme.
   // If past levels exists, distribute corrections to them as well
   // (no need to sync, as this is purely local after the interpolation)
   //=============================================================================

   valid=PAMR_init_s_iter(lev,PAMR_AMRH,0);
   while(valid)
   {
      ev_ldptr();
      for (i=0; i<AMRD_num_hyperbolic_vars; i++)
      {
        if (PAMR_var_type(AMRD_hyperbolic_vars[i])==PAMR_VERTEX_CENTERED) {
          loc_size=AMRD_g_size;
        } else {
          loc_size=AMRD_g_size_c;
        }
        
         for (j=0; j<loc_size; j++)
         {
            f_l=(AMRD_AMR_f[0][i])[j];
            f_lm1=(AMRD_AMR_f[1][i])[j];
            f_c=(f_l-f_lm1)/(1.0e0-rho_sp0*rho_sp0);
            (AMRD_AMR_f[1][i])[j]=f_l-f_c;
            for (k=2; k<AMRD_num_evo_tl; k++) (AMRD_AMR_f[k][i])[j]=(AMRD_AMR_f[k][i])[j]-f_c*(rho_tm0-(k-1.0e0))/rho_tm0;
         }
      }
      valid=PAMR_next_g();
   }
}


//===================================================================================
// Some functions for dealing with the flux correction mask
// make sure ev_ldptr has been called before zeroing the mask
//===================================================================================

void zero_fc_mask(void) {
  int i;
  if (!AMRD_fc_mask) return;
  for (i=0; i<AMRD_g_size_c; i++) AMRD_fc_mask[i]=0;
}

void set_fc_mask(int L) {

   int valid,is,js,ks,ie,je,ke,i,j,k,l;
   real cbbox[2*PAMR_MAX_DIM];  // bounding box of potential child.
   real cbbox_trans[2*PAMR_MAX_DIM];  // for use in transverse directions.  (see below)
   real lbbox[2*PAMR_MAX_DIM];  // bbox for the grid where fc_mask is being set
   real lbbox_trans[2*PAMR_MAX_DIM];  
   int Lf, Lc;
   int cghost_width[2*PAMR_MAX_DIM];
   real c_dx[PAMR_MAX_DIM];
   real lfc_mask;  
   real sign_flag;
   real pxflag, mxflag, pyflag, myflag, pzflag, mzflag;
   real cdt;
   int added_sign_flag;
   int in_range_t1, in_range_t2;
   int set_pxflag, set_mxflag, set_pyflag;
   int set_myflag, set_pzflag, set_mzflag;

   sign_flag = AMRD_SIGN_FLAG;
   pxflag = AMRD_PXFLAG;
   mxflag = AMRD_MXFLAG;
   pyflag = AMRD_PYFLAG;
   myflag = AMRD_MYFLAG;
   pzflag = AMRD_PZFLAG;
   mzflag = AMRD_MZFLAG;

   Lf=PAMR_get_max_lev(PAMR_AMRH);
   Lc=PAMR_get_min_lev(PAMR_AMRH);

   valid=PAMR_init_s_iter(L,PAMR_AMRH,0);

   while(valid) {
     ev_ldptr();
     if (!AMRD_fc_mask) return;
     for (j=0; j<AMRD_g_size_c; j++) AMRD_fc_mask[j]=0.0;

     // The search for type A cells should not include ghost zones.
     // We will restrict the limits of the loop accordingly.
     is=0; ie=AMRD_g_Nx_c;
     if (AMRD_g_ghost_width[0]>0) is += AMRD_g_ghost_width[0];
     if (AMRD_g_ghost_width[1]>0) ie -= AMRD_g_ghost_width[1];
     js=0; je=AMRD_g_Ny_c;
     if (AMRD_g_dim > 1) {
       if (AMRD_g_ghost_width[2]>0) js += AMRD_g_ghost_width[2];
       if (AMRD_g_ghost_width[3]>0) je -= AMRD_g_ghost_width[3];
     }
     ks=0; ke=AMRD_g_Nz_c;
     if (AMRD_g_dim > 2) {
       if (AMRD_g_ghost_width[4]>0) ks += AMRD_g_ghost_width[4];
       if (AMRD_g_ghost_width[5]>0) ke -= AMRD_g_ghost_width[5];
     }

     if (L < Lf && L > Lc) {
       // Now look for type A cells.  
       for (i=is; i<ie; i++) {
         for (j=js; j<je; j++) {
           for (k=ks; k<ke; k++) {

             // For each point on this level L grid, 
             // loop over possible child grids.
             set_pxflag = 0;
             set_mxflag = 0;
             set_pyflag = 0;
             set_myflag = 0;
             set_pzflag = 0;
             set_mzflag = 0;

             PAMR_push_iter();
             valid=PAMR_init_s_iter(L+1,PAMR_AMRH,1);
             while(valid) {
               PAMR_get_g_bbox(cbbox);
               PAMR_get_g_bbox(cbbox_trans);
               PAMR_get_g_ghost_width(cghost_width);
               PAMR_get_dxdt(L+1,c_dx,&cdt);

               // Reduce the bbox by the AMR boundary width (but not near a physical boundary)
               for (l=0; l<AMRD_g_dim; l++) {
                 if (cghost_width[2*l]==0 && ((cbbox[2*l]-AMRD_base_bbox[2*l]) > c_dx[l]/2)) {
                   cbbox[2*l] += AMRD_AMR_bdy_width_c*c_dx[l];
                   cbbox_trans[2*l] += AMRD_AMR_bdy_width_c*c_dx[l];
                 }
                 // If you are at a ghost boundary, send the boundary far away where 
                 // it won't bother us.  But don't do this to the transverse box, which
                 // will be used to check the range in the transverse direction to the 
                 // flux correction under consideration.
                 if (cghost_width[2*l]>0) {
                   cbbox[2*l] = AMRD_base_bbox[2*l] - AMRD_g_dx[l];
                 }
                 
                 if (cghost_width[2*l+1]==0 && ((AMRD_base_bbox[2*l+1]-cbbox[2*l+1])>c_dx[l]/2)) {
                   cbbox[2*l+1] -= AMRD_AMR_bdy_width_c*c_dx[l];
                   cbbox_trans[2*l+1] -= AMRD_AMR_bdy_width_c*c_dx[l];
                 }
                 
                 if (cghost_width[2*l+1]>0) {
                   cbbox[2*l+1] = AMRD_base_bbox[2*l+1] + AMRD_g_dx[l];
                 }
               }

               lfc_mask = 0.0;  // Don't add the sign flag for type A cells.
                                // You want their flux contributions to enter
                                // with a negative sign.
               
               // Check for flux corrections in the x-direction.
               
               // First, are we in the appropriate range in the *transverse*
               // directions to be doing the flux correction?  
               in_range_t1 = 1;
               in_range_t2 = 1;

               if ( AMRD_g_dim > 1 &&
                    ((AMRD_g_x_c[1])[j] < cbbox_trans[2] || 
                     (AMRD_g_x_c[1])[j] > cbbox_trans[3] )) in_range_t1 = 0;
               if ( AMRD_g_dim > 2 && 
                    ((AMRD_g_x_c[2])[k] < cbbox_trans[4] || 
                     (AMRD_g_x_c[2])[k] > cbbox_trans[5] )) in_range_t2 = 0;
               
               if ((AMRD_g_x_c[0])[i] > (cbbox[0] - AMRD_g_dx[0]) &&
                   (AMRD_g_x_c[0])[i] < cbbox[0] &&
                   (AMRD_g_x_c[0])[i] > (AMRD_base_bbox[0] + AMRD_g_dx[0]/2.0)
                   && in_range_t1 && in_range_t2) {
                 if (!set_pxflag) {
                   lfc_mask += pxflag;
                   set_pxflag = 1;
/*                    if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                      printf("L = %d:  type A pxflag at i=%d j=%d, k=%d\n",L,i,j,k); */
/*                    } */
                 }
               }
               
               if ((AMRD_g_x_c[0])[i] < (cbbox[1] + AMRD_g_dx[0]) &&
                   (AMRD_g_x_c[0])[i] > cbbox[1] &&
                   (AMRD_g_x_c[0])[i] < (AMRD_base_bbox[1] - AMRD_g_dx[0]/2.0)
                   && in_range_t1 && in_range_t2) {
                 if (!set_mxflag) {
                   lfc_mask += mxflag;
                   set_mxflag = 1;
/*                    if (my_rank==0  && AMRD_evo_trace > 2 && k==0) { */
/*                      printf("L = %d:  type A mxflag at i=%d j=%d, k=%d\n",L,i,j,k); */
/*                    } */
                 }
               }
               
               // Now check for flux corrections in the y-direction.
               if (AMRD_g_dim > 1) {

                 // Are we in the correct transverse range?
                 in_range_t1 = 1;
                 in_range_t2 = 1;
               
                 if ( AMRD_g_dim > 2 &&
                     ((AMRD_g_x_c[2])[k] < cbbox_trans[4] || 
                      (AMRD_g_x_c[2])[k] > cbbox_trans[5] )) in_range_t1 = 0;
                 if ( (AMRD_g_x_c[0])[i] < cbbox_trans[0] || 
                      (AMRD_g_x_c[0])[i] > cbbox_trans[1] )  in_range_t2 = 0;

                 if ((AMRD_g_x_c[1])[j] > (cbbox[2] - AMRD_g_dx[1]) &&
                     (AMRD_g_x_c[1])[j] < cbbox[2] &&
                     (AMRD_g_x_c[1])[j] > (AMRD_base_bbox[2] + AMRD_g_dx[1]/2.0) 
                     && in_range_t1 && in_range_t2) {
                   if (!set_pyflag) {
                     lfc_mask += pyflag;
                     set_pyflag = 1;
/*                      if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                        printf("L = %d:  type A pyflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                      } */
                   }
                 }
                 
                 if ((AMRD_g_x_c[1])[j] < (cbbox[3] + AMRD_g_dx[1]) &&
                     (AMRD_g_x_c[1])[j] > cbbox[3] &&
                     (AMRD_g_x_c[1])[j] < (AMRD_base_bbox[3] - AMRD_g_dx[1]/2.0) 
                     && in_range_t1 && in_range_t2) {
                   if (!set_myflag) {
                     lfc_mask += myflag;
                     set_myflag = 1;
/*                      if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                        printf("L = %d:  type A myflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                      } */
                   }
                 }
               }
               
               // Now check for flux corrections in the z-direction.
               if (AMRD_g_dim > 2) {
                 
                 // Are we in the correct transverse range?
                 in_range_t1 = 1;
                 in_range_t2 = 1;
               
                 if ( (AMRD_g_x_c[0])[i] < cbbox_trans[0] || 
                      (AMRD_g_x_c[0])[i] > cbbox_trans[1] )  in_range_t2 = 0;
                 if ( AMRD_g_dim > 1 &&
                     ((AMRD_g_x_c[1])[j] < cbbox_trans[2] || 
                      (AMRD_g_x_c[1])[j] > cbbox_trans[3] )) in_range_t1 = 0;

                 if ((AMRD_g_x_c[2])[k] > (cbbox[4] - AMRD_g_dx[2]) &&
                     (AMRD_g_x_c[2])[k] < cbbox[4] &&
                     (AMRD_g_x_c[2])[k] > (AMRD_base_bbox[4] + AMRD_g_dx[2]/2.0) 
                     && in_range_t1 && in_range_t2) {
                   if (!set_pzflag) {
                     lfc_mask += pzflag;
                     set_pzflag = 1;
/*                      if (my_rank==0 && AMRD_evo_trace > 2 && k==0) { */
/*                        printf("L = %d:  type A pzflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                      } */
                   }
                 }
                 
                 if ((AMRD_g_x_c[2])[k] < (cbbox[5] + AMRD_g_dx[2]) &&
                     (AMRD_g_x_c[2])[k] > cbbox[5] &&
                     (AMRD_g_x_c[2])[k] < (AMRD_base_bbox[5] - AMRD_g_dx[2]/2.0) 
                     && in_range_t1 && in_range_t2) {
                   if (!set_mzflag) {
                     lfc_mask += mzflag;
                     set_mzflag = 1;
/*                      if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                        printf("L = %d:  type A mzflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                      } */
                   }
                 }
               }
               
               AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c]+=lfc_mask;
         
               valid=PAMR_next_g();
             }
             PAMR_pop_iter(); 

           }
         }
       }
     } // end if (L < Lf)

     // Now look for type B cells.  Note that no information about the
     // parent grid is necessary.  All I need to know is that I've got a 
     // parent grid underneath me.  I require L>Lc+1 because it is assumed
     // that Lc is a shadow of the base level.  
     if (L > Lc + 1) {
       // Reduce the level L bbox by the AMR boundary width.
       for (i=0; i<AMRD_g_dim; i++) {
         lbbox_trans[2*i] = lbbox[2*i] = AMRD_g_bbox[2*i];
         lbbox_trans[2*i+1] = lbbox[2*i+1] = AMRD_g_bbox[2*i+1];
         if (AMRD_g_ghost_width[2*i]==0 && 
             ((lbbox[2*i]-AMRD_base_bbox[2*i]) > AMRD_g_dx[i]/2)) {
           lbbox[2*i]   += AMRD_AMR_bdy_width_c*AMRD_g_dx[i];
           lbbox_trans[2*i]   += AMRD_AMR_bdy_width_c*AMRD_g_dx[i];
         }
         if (AMRD_g_ghost_width[2*i]>0) {
           lbbox[2*i]    = AMRD_base_bbox[2*i] - AMRD_g_dx[i];
         }
         if (AMRD_g_ghost_width[2*i+1]==0 && 
             ((AMRD_base_bbox[2*i+1]-lbbox[2*i+1]) > AMRD_g_dx[i]/2)) {
           lbbox[2*i+1] -= AMRD_AMR_bdy_width_c*AMRD_g_dx[i];
           lbbox_trans[2*i+1] -= AMRD_AMR_bdy_width_c*AMRD_g_dx[i];
         }
         if (AMRD_g_ghost_width[2*i+1]>0) {
           lbbox[2*i+1]  = AMRD_base_bbox[2*i+1] + AMRD_g_dx[i];
         }
       }

       for (i=0; i<AMRD_g_Nx_c; i++) {
         for (j=0; j<AMRD_g_Ny_c; j++) {
           for (k=0; k<AMRD_g_Nz_c; k++) {

             added_sign_flag = 0;
             lfc_mask=AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c];
             
             // Check for flux corrections of type B in the x direction.             

             // First, are we in the appropriate range in the *transverse*
             // directions to be doing the flux correction?  
             in_range_t1 = 1;
             in_range_t2 = 1;
             
             if ( AMRD_g_dim > 1 &&
                  ((AMRD_g_x_c[1])[j] < lbbox_trans[2] || 
                   (AMRD_g_x_c[1])[j] > lbbox_trans[3] )) in_range_t1 = 0;
             if ( AMRD_g_dim > 2 && 
                  ((AMRD_g_x_c[2])[k] < lbbox_trans[4] || 
                   (AMRD_g_x_c[2])[k] > lbbox_trans[5] )) in_range_t2 = 0;

             if ((AMRD_g_x_c[0])[i] > (lbbox[0] - AMRD_g_dx[0]) &&
                 (AMRD_g_x_c[0])[i] < lbbox[0] &&
                 (AMRD_g_x_c[0])[i] > (AMRD_base_bbox[0] + AMRD_g_dx[0]/2.0) 
                 && in_range_t1 && in_range_t2) {
               if (!added_sign_flag) {
                 lfc_mask += sign_flag;
                 added_sign_flag=1;
               }
               lfc_mask += pxflag;

/*                if (my_rank==0 && AMRD_evo_trace > 2 && k==0) { */
/*                  printf("L = %d:  type B pxflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                } */

               if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                 AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
               }
             }
             
             if ((AMRD_g_x_c[0])[i] < (lbbox[1] + AMRD_g_dx[0]) &&
                 (AMRD_g_x_c[0])[i] > lbbox[1] &&
                 (AMRD_g_x_c[0])[i] < (AMRD_base_bbox[1] - AMRD_g_dx[0]/2.0) 
                 && in_range_t1 && in_range_t2) {
               if (!added_sign_flag) {
                 lfc_mask += sign_flag;
                 added_sign_flag=1;
               }
               lfc_mask += mxflag;

/*                if (my_rank==0 && AMRD_evo_trace > 2 && k==0) { */
/*                  printf("L = %d:  type B mxflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                } */

               if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                 AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
               }
             }
             
             // Now check for flux corrections of type B in the y-direction.
             // Are we in the correct transverse range?
             if (AMRD_g_dim > 1) {
               in_range_t1 = 1;
               in_range_t2 = 1;
               
               if ( AMRD_g_dim > 2 &&
                   ((AMRD_g_x_c[2])[k] < lbbox_trans[4] || 
                    (AMRD_g_x_c[2])[k] > lbbox_trans[5] )) in_range_t1 = 0;
               if ( (AMRD_g_x_c[0])[i] < lbbox_trans[0] || 
                    (AMRD_g_x_c[0])[i] > lbbox_trans[1] )  in_range_t2 = 0;
                              
               if ((AMRD_g_x_c[1])[j] > (lbbox[2] - AMRD_g_dx[1]) &&
                   (AMRD_g_x_c[1])[j] < lbbox[2] &&
                   (AMRD_g_x_c[1])[j] > (AMRD_base_bbox[2] + AMRD_g_dx[1]/2.0) 
                   && in_range_t1 && in_range_t2) {
                 if (!added_sign_flag) {
                   lfc_mask += sign_flag;
                   added_sign_flag=1;
                 }
                 lfc_mask += pyflag;

/*                  if (my_rank==0 && AMRD_evo_trace > 2 && k==0) { */
/*                    printf("L = %d:  type B pyflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                  } */

                 if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                   AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
                 }
               }
               
               if ((AMRD_g_x_c[1])[j] < (lbbox[3] + AMRD_g_dx[1]) &&
                   (AMRD_g_x_c[1])[j] > lbbox[3] &&
                   (AMRD_g_x_c[1])[j] < (AMRD_base_bbox[3] - AMRD_g_dx[1]/2.0) 
                   && in_range_t1 && in_range_t2) {
                 if (!added_sign_flag) {
                   lfc_mask += sign_flag;
                   added_sign_flag=1;
                 }
                 lfc_mask += myflag;

/*                  if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                    printf("L = %d:  type B myflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                  } */

                 if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                   AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
                 }
               }
             }
             
             // Finally, check for corrections of type B in the z-direction.
             if (AMRD_g_dim > 2) {

               // Are we in the correct transverse range?
               in_range_t1 = 1;
               in_range_t2 = 1;
               
               if ( (AMRD_g_x_c[0])[i] < lbbox_trans[0] || 
                    (AMRD_g_x_c[0])[i] > lbbox_trans[1] )  in_range_t2 = 0;
               if ( AMRD_g_dim > 1 &&
                   ((AMRD_g_x_c[1])[j] < lbbox_trans[2] || 
                    (AMRD_g_x_c[1])[j] > lbbox_trans[3] )) in_range_t1 = 0;

               if ((AMRD_g_x_c[2])[k] > (lbbox[4] - AMRD_g_dx[2]) &&
                   (AMRD_g_x_c[2])[k] < lbbox[4] &&
                   (AMRD_g_x_c[2])[k] > (AMRD_base_bbox[4] + AMRD_g_dx[2]/2.0) 
                   && in_range_t1 && in_range_t2) {
                 if (!added_sign_flag) {
                   lfc_mask += sign_flag;
                   added_sign_flag=1;
                 }
                 lfc_mask += pzflag;

/*                  if (my_rank==0 &&  AMRD_evo_trace > 2 && k==0) { */
/*                    printf("L = %d:  type B pzflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                  } */

                 if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                   AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
                 }
               }
               
               if ((AMRD_g_x_c[2])[k] < (lbbox[5] + AMRD_g_dx[2]) &&
                   (AMRD_g_x_c[2])[k] > lbbox[5] &&
                   (AMRD_g_x_c[2])[k] < (AMRD_base_bbox[5] - AMRD_g_dx[2]/2.0) 
                   && in_range_t1 && in_range_t2) {
                 if (!added_sign_flag) {
                   lfc_mask += sign_flag;
                   added_sign_flag=1;
                 }
                 lfc_mask += mzflag;

/*                  if (my_rank==0 && AMRD_evo_trace > 2 && k==0) { */
/*                      printf("L = %d:  type B mzflag at i=%d, j=%d, k=%d\n",L,i,j,k); */
/*                  } */

                 if (AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c] > 0) {
                   AMRD_stop("set_fc_mask ... the same cell cannot be both type A and B","");
                 }
               }
             }
             
             AMRD_fc_mask[i+j*AMRD_g_Nx_c+k*AMRD_g_Nx_c*AMRD_g_Ny_c]=lfc_mask;
           }
         }
       }

     } // end if L > Lc+1
     
     valid=PAMR_next_g();
   }
}

// returns an integer version of fc_mask

int *get_ifc_mask(real *fc_mask, int size)
{
   static int *l_ifc_mask=0;
   static int l_max_size=0;
   int *p,i;
   real *q;

   if (size>l_max_size)
   {
      if (l_ifc_mask) free(l_ifc_mask);
      if (!(l_ifc_mask=(int *)malloc(sizeof(int)*size)))
         AMRD_stop("out of memory trying to allocate ifc_mask","");
      l_max_size=size;
   }

   p=l_ifc_mask; q=fc_mask;
   for (i=0;i<size;i++) (*p++)=(*q++)+0.5;

   return l_ifc_mask;
}

/*
After free data hook function is called, sync/inject/interp AMRD_free_data_gfn grid functions.
*/
void sync_free_data(int L) {
   if (AMRD_num_free_data_vars==0) return;
   int i;

   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();

   for (i=0; i<AMRD_num_free_data_vars; i++) PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_SYNC);
   PAMR_sync(L,0,PAMR_AMRH,0);

   PAMR_thaw_tf_bits();
}

void inject_free_data(int Lf) {
   if (AMRD_num_free_data_vars==0) return;

   int i;

   //--------------------------------------------------------
   // Before injecting, volume weight the hydro variables.
   //--------------------------------------------------------

   if(AMRD_num_inject_wavg_vars){ 
	apply_wavg(0, Lf-1, PAMR_AMRH);
   	apply_wavg(0, Lf, PAMR_AMRH);
   }
   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();

   for (i=0; i<AMRD_num_free_data_vars; i++) {
     if (PAMR_gfn_var_type(AMRD_free_data_gfn[i])==PAMR_VERTEX_CENTERED) {
       PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_STRAIGHT_INJECT);
     } else {
   	if(AMRD_num_inject_wavg_vars) PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_NN_ADD);
   	else PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_FIRST_ORDER_CONS);
     }
   }
   PAMR_inject(Lf,0,PAMR_AMRH);
   PAMR_thaw_tf_bits();

   if(AMRD_num_inject_wavg_vars){ 
	apply_wavg(1, Lf-1, PAMR_AMRH);
   	apply_wavg(1, Lf, PAMR_AMRH);
   }	
}
void interp_free_data(int Lc) {
   if (AMRD_num_free_data_vars==0) return;

   PAMR_freeze_tf_bits();
   PAMR_clear_tf_bits();

   int i;
   for (i=0; i<AMRD_num_free_data_vars; i++) {
     if (PAMR_gfn_var_type(AMRD_free_data_gfn[i])==PAMR_VERTEX_CENTERED) {
       PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_FOURTH_ORDER);
     } else {
       PAMR_set_tf_bit(AMRD_free_data_gfn[i],PAMR_FIRST_ORDER_CONS);
     }
   }
   PAMR_interp(Lc,0,PAMR_AMRH);

   PAMR_thaw_tf_bits();
}
