subroutine impose_NSCBC(lo, hi, domlo, domhi, &
                        uin, uin_l1, uin_h1, &
                        q, q_l1, q_h1, &
                        qaux, qa_l1, qa_h1, &
                        bcMask, bcMask_l1, bcMask_h1, &
                        time,delta,dt) bind(C, name="impose_NSCBC")
    
 
    use bl_error_module
    use chemistry_module, only : nspecies
    use eos_module
    use fundamental_constants_module, only: k_B, n_A

    use bl_constants_module
    use prob_params_module, only : physbc_lo, physbc_hi, problo, probhi, &
                                   Interior, Inflow, Outflow, Symmetry, SlipWall, NoSlipWall
    
    use meth_params_module, only : NVAR, URHO, UMX, UMY, UMZ, UEDEN, UEINT, UTEMP,&
                                   UFA, UFS, UFX, NQAUX, QC, QGAMC, QRSPEC, &
                                   QC, QDPDE, QDPDR, QCSML, QGAMC, &
                                   QVAR, QRHO, QU, QV, QREINT, QPRES, QTEMP, &
                                   QFS, QFX, QGAME, NHYP
    use meth_params_module, only: nb_nscbc_params
 
    implicit none
   

  integer, intent(in) :: lo(1), hi(1)
  integer, intent(in) :: domlo(1), domhi(1)
  integer, intent(in) :: q_l1, q_h1
  integer, intent(in) :: qa_l1, qa_h1
  integer, intent(in) :: uin_l1, uin_h1
  integer, intent(in) :: bcMask_l1, bcMask_h1
  
  double precision, intent(inout) :: q(q_l1:q_h1,QVAR)
  double precision, intent(inout) :: qaux(qa_l1:qa_h1,NQAUX)
  double precision, intent(inout) :: uin(uin_l1:uin_h1,NVAR)
  integer, intent(inout) :: bcMask(bcMask_l1:bcMask_h1,2)
  double precision, intent(in) :: delta(1), dt, time

  ! Local
  double precision dx, dy
  double precision x, y
  
  double precision :: drhodx, dudx, dvdx, dpdx, dx_vec
  double precision :: dpdy, dudy, dvdy, drhody
  double precision :: L1, L2, L3, L4
  double precision :: S1, S2
  double precision :: Kout, sigma_out
  double precision :: relax_U, relax_T, magvel
  double precision :: mach_max_hi_x_tmp, mach_local_hi_x
  double precision :: mach_max_lo_x_tmp, mach_local_lo_x
  double precision :: INLET_PRESSURE, INLET_VX, INLET_VY, INLET_TEMPERATURE
  
  double precision :: U_dummy(NVAR)
  double precision :: U_ext(NVAR)
  double precision, parameter :: small = 1.d-8
  
  integer          :: q_lo(1), q_hi(1)
  integer          ::  uin_lo(1),  uin_hi(1)
  integer          :: i, j, n, hop
  integer          :: bc_type
  double precision :: bc_params(nb_nscbc_params)
  double precision :: wall_sign
  
  type (eos_t) :: eos_state
  call build(eos_state)

  q_lo = q_l1
  q_hi = q_h1
  
  uin_lo  = uin_l1
  uin_hi  = uin_h1
 
  dx = delta(1)
  
  bcMask(:,:) = 0

 !--------------------------------------------------------------------------   
 ! lower X
 !--------------------------------------------------------------------------
 
 if ((q_lo(1) < domlo(1)) .and. (physbc_lo(1) /= Interior)) then
   i = domlo(1)
 
      x   = (dble(i)+HALF)*dx

     !2nd order
     dpdx = ((-3.0d0/2.0d0)*q(i,QPRES)+2.0d0*q(i+1,QPRES)-0.5d0*q(i+2,QPRES))/dx
     dudx = ((-3.0d0/2.0d0)*q(i,QU)+2.0d0*q(i+1,QU)-0.5d0*q(i+2,QU))/dx
     dvdx = ((-3.0d0/2.0d0)*q(i,QV)+2.0d0*q(i+1,QV)-0.5d0*q(i+2,QV))/dx
     drhodx = ((-3.0d0/2.0d0)*q(i,QRHO)+2.0d0*q(i+1,QRHO)-0.5d0*q(i+2,QRHO))/dx
 
     ! Calling user target BC values 
     call bcnormal_nscbc([x,y,0.0d0],U_dummy,U_ext,1,1,bc_type,bc_params,.false.)
     bcMask(i-1,1) = bc_type

     eos_state %  T = U_ext(UTEMP)
     eos_state %  rho = U_ext(URHO)
     eos_state % massfrac(1:nspecies) = u_ext(UFS:UFS+nspecies-1) / U_ext(URHO)
     call eos_rt(eos_state)
     INLET_VX = U_ext(UMX)/U_ext(URHO)
     INLET_VY = U_ext(UMY)/U_ext(URHO)
     INLET_TEMPERATURE = U_ext(UTEMP)
     INLET_PRESSURE = eos_state%p

     mach_local_lo_x = q(i,QU)/qaux(i,QC)

     ! Compute LODI equations
     if (bc_type == Inflow) then

       relax_U = bc_params(1)
       relax_T = bc_params(3)
 
       L1 = (q(i,QU)-qaux(i,QC))* (dpdx - (q(i,QRHO)*qaux(i,QC))*dudx)
       L2 = relax_T * (q(i,QRHO)*qaux(i,QC)*qaux(i,QRSPEC)/probhi(1)) * (q(i,QTEMP) - INLET_TEMPERATURE) 
         
       L3 = 0.0d0
       L4 = relax_U * ((q(i,QRHO)*qaux(i,QC)**2.0d0)*(1.0d0-mach_local_lo_x*mach_local_lo_x)/probhi(1)) * &
            (q(i,QU) - INLET_VX)  
            
            
     elseif ((bc_type == SlipWall).or.(bc_type == NoSlipWall)) then
     
       ! Values long Y will be computed by mirror functions below
       
     elseif (bc_type == Outflow) then
       
     ! We find that using a local Mach number gives better results for high Mach nb.
     ! This is in contradiction with Granet AIAA 2010
     ! However for low Mach number a surface averaged Mach number is much more better
     ! as reported in the paper of Granet
       sigma_out = bc_params(1)
       Kout = sigma_out*(1.0d0 - (mach_local_lo_x**2.0d0))*qaux(i,QC)/(probhi(1))
 
       L1 = (q(i,QU)-qaux(i,QC))* (dpdx - (q(i,QRHO)*qaux(i,QC))*dudx)
       L2 = q(i,QU) * ( ((qaux(i,QC)**2.0d0)*drhodx) - dpdx)
       L3 = 0.0d0
       L4 = (Kout*(q(i,QPRES) - INLET_PRESSURE)) 
       
     else
       call bl_error("Error:: This BC is not yet implemented for lo_x in characteristic form")
     endif
 
     if (q(i,QU) == 0.0d0) then
       L1 = L1 / (q(i,QU)-qaux(i,QC))
       L2 = 0.0d0
       L3 = 0.0d0
       L4 = L4 / (q(i,QU)+qaux(i,QC))
     else       
       L1 = L1 / (q(i,QU)-qaux(i,QC))
       L2 = L2 / q(i,QU)
       L3 = 0.0d0
       L4 = L4 / (q(i,QU)+qaux(i,QC))
     endif
       
     ! Compute new spatial derivative
     drhodx = (L2 + 0.5d0*(L1 + L4))/(qaux(i,QC)**2.0d0)  
     dudx   = (L4-L1)/(2.0d0*qaux(i,QC)*q(i,QRHO))
     dpdx   = 0.5d0*(L1+L4)
       
     ! Update ghost cells
     ! 2nd order
     q(i-1,QU)    = q(i+1,QU) - 2.0d0*dx*dudx
     q(i-1,QRHO)  = q(i+1,QRHO)  - 2.0d0*dx*drhodx
     q(i-1,QPRES) = q(i+1,QPRES) - 2.0d0*dx*dpdx
   
     !---------------- 
     q(i-2,QU)    = -2.0d0*q(i+1,QU) - 3.0d0*q(i,QU) + 6.0d0*q(i-1,QU) + 6.0d0*dx*dudx
     q(i-2,QRHO)    = -2.0d0*q(i+1,QRHO) - 3.0d0*q(i,QRHO) + 6.0d0*q(i-1,QRHO) + 6.0d0*dx*drhodx
     q(i-2,QPRES)    = -2.0d0*q(i+1,QPRES) - 3.0d0*q(i,QPRES) + 6.0d0*q(i-1,QPRES) + 6.0d0*dx*dpdx
 
     q(i-3,QU)    = 3.0d0*q(i+1,QU) +10.0d0*q(i,QU) - 18.0d0*q(i-1,QU) + 6.0d0*q(i-2,QU) - 12.0d0*dx*dudx
     q(i-3,QRHO)    = 3.0d0*q(i+1,QRHO) +10.0d0*q(i,QRHO) - 18.0d0*q(i-1,QRHO) + 6.0d0*q(i-2,QRHO) - 12.0d0*dx*drhodx
     q(i-3,QPRES)    = 3.0d0*q(i+1,QPRES) +10.0d0*q(i,QPRES) - 18.0d0*q(i-1,QPRES) + 6.0d0*q(i-2,QPRES) - 12.0d0*dx*dpdx
 
     q(i-4,QU)    = -2.0d0*q(i+1,QU) - 13.0d0*q(i,QU) + 24.0d0*q(i-1,QU) - 12.0d0*q(i-2,QU)  &
                     + 4.0d0*q(i-3,QU) + 12.0d0*dx*dudx
     q(i-4,QRHO)  = -2.0d0*q(i+1,QRHO) - 13.0d0*q(i,QRHO) + 24.0d0*q(i-1,QRHO) - 12.0d0*q(i-2,QRHO) &
                     + 4.0d0*q(i-3,QRHO) + 12.0d0*dx*drhodx
     q(i-4,QPRES) = -2.0d0*q(i+1,QPRES) - 13.0d0*q(i,QPRES) + 24.0d0*q(i-1,QPRES) - 12.0d0*q(i-2,QPRES) &
                     + 4.0d0*q(i-3,QPRES) + 12.0d0*dx*dpdx
 
     if ((bc_type .eq. NoSlipWall).or.(bc_type .eq. SlipWall)) then
           
       q(i-1,QU)    = -q(i,QU)
       q(i-2,QU)    = -q(i+1,QU)
       q(i-3,QU)    = -q(i+2,QU)
       q(i-4,QU)    = -q(i+3,QU)
       
       q(i-1,QRHO)  = q(i,QRHO)
       q(i-2,QRHO)  = q(i+1,QRHO)
       q(i-3,QRHO)  = q(i+2,QRHO)
       q(i-4,QRHO)  = q(i+3,QRHO)
       
       q(i-1,QPRES)  = q(i,QPRES)
       q(i-2,QPRES)  = q(i+1,QPRES)
       q(i-3,QPRES)  = q(i+2,QPRES)
       q(i-4,QPRES)  = q(i+3,QPRES)
     
     end if
 
 
     ! Recompute missing values thanks to EOS
     do hop=domlo(1)-1,-4,-1
     
       eos_state % p        = q(hop,QPRES )
       eos_state % rho      = q(hop,QRHO  )
       ! Here we do 0th-order extrapolation for species mass fraction
       !eos_state % massfrac = q(domlo(1),QFS:QFS+nspecies-1)
       eos_state % aux      = q(domlo(1),QFX:QFX+naux-1)

       call eos_rp(eos_state)
       q(hop,QTEMP)  = eos_state % T
       q(hop,QREINT) = eos_state % e * q(hop,QRHO)
       q(hop,QGAME)  = q(hop,QPRES) / q(hop,QREINT) + ONE
       
       qaux(hop,QDPDR)  = eos_state % dpdr_e
       qaux(hop,QDPDE)  = eos_state % dpde
       qaux(hop,QGAMC)  = eos_state % gam1
       qaux(hop,QC   )  = eos_state % cs
       qaux(hop,QCSML)  = max(small, small * qaux(hop,QC))
 
       uin(hop,URHO )  = eos_state % rho 
       uin(hop,UMX  )  = q(hop,QU ) * eos_state % rho 
       uin(hop,UMY  )  = 0.0d0
       uin(hop,UMZ  ) = 0.0d0
       uin(hop,UEINT) = eos_state % rho   *  eos_state % e
       uin(hop,UEDEN) = eos_state % rho  &
          * (eos_state % e + 0.5d0 * (uin(hop,UMX)**2 + uin(hop,UMY)**2))
       uin(hop,UTEMP) = eos_state % T
       do n=1, nspecies
          uin(hop,UFS+n-1) = eos_state % rho  *  eos_state % massfrac(n)
       end do   
       
     enddo
 
 end if
 

 
 !--------------------------------------------------------------------------   
 ! upper X
 !--------------------------------------------------------------------------
 
 if ((q_hi(1) > domhi(1)) .and. (physbc_hi(1) /= Interior)) then
   i = domhi(1)
   
     x   = (dble(i)+HALF)*dx
     
     !2nd order
     dpdx = ((3.0d0/2.0d0)*q(i,QPRES)-2.0d0*q(i-1,QPRES)+0.5d0*q(i-2,QPRES))/dx
     dudx = ((3.0d0/2.0d0)*q(i,QU)-2.0d0*q(i-1,QU)+0.5d0*q(i-2,QU))/dx
     dvdx = ((3.0d0/2.0d0)*q(i,QV)-2.0d0*q(i-1,QV)+0.5d0*q(i-2,QV))/dx
     drhodx = ((3.0d0/2.0d0)*q(i,QRHO)-2.0d0*q(i-1,QRHO)+0.5d0*q(i-2,QRHO))/dx
       
     ! Calling user target BC values 
     call bcnormal_nscbc([x,y,0.0d0],U_dummy,U_ext,1,-1,bc_type,bc_params,.false.)
     bcMask(i+1,1) = bc_type
     
     eos_state %  T = U_ext(UTEMP)
     eos_state %  rho = U_ext(URHO)
     eos_state % massfrac(1:nspecies) = u_ext(UFS:UFS+nspecies-1) / U_ext(URHO)
     call eos_rt(eos_state)
     INLET_VX = U_ext(UMX)/U_ext(URHO)
     INLET_VY = U_ext(UMY)/U_ext(URHO)
     INLET_TEMPERATURE = U_ext(UTEMP)
     INLET_PRESSURE = eos_state%p

     mach_local_hi_x = q(i,QU) /qaux(i,QC)

     ! Compute LODI equations                        
     if (bc_type .eq. Inflow) then
     
       relax_U = bc_params(1)
       relax_T = bc_params(3)

       L1 = relax_U * ((q(i,QRHO)*qaux(i,QC)**2.0d0)*(1.0d0-mach_local_hi_x*mach_local_hi_x)/probhi(1)) * &
            (q(i,QU) - INLET_VX) 
              
       L2 = relax_T * (q(i,QRHO)*qaux(i,QC)*qaux(i,QRSPEC)/probhi(1)) * (q(i,QTEMP) - INLET_TEMPERATURE) 
         
       L3 = 0.d0
       L4 = (q(i,QU)+qaux(i,QC))* (dpdx + (q(i,QRHO)*qaux(i,QC))*dudx)  
         
     elseif (bc_type .eq. Outflow) then
     
       ! We find that using a local Mach number gives better results for high Mach nb.
       ! This is in contradiction with Granet AIAA 2010
       ! However for low Mach number a surface averaged Mach number is much more better
       ! as reported in the paper of Granet
       sigma_out = bc_params(1)
       Kout = sigma_out*(1.0d0 - (mach_local_hi_x**2.0d0))*qaux(i,QC)/(probhi(1))

       L1 = (Kout*(q(i,QPRES) - INLET_PRESSURE))
       L2 = q(i,QU) * ( ((qaux(i,QC)**2.0d0)*drhodx) - dpdx) 
       L3 = 0.0d0
       L4 = (q(i,QU)+qaux(i,QC))* (dpdx + (q(i,QRHO)*qaux(i,QC))*dudx) 
   
     elseif ((bc_type == SlipWall).or.(bc_type == NoSlipWall)) then
     
       ! Values long Y will be computed by mirror functions below
       
     else
       call bl_error("Error:: This BC is not yet implemented for hi_x in characteristic form")
     endif
   
     if (q(i,QU) == 0.0d0) then
       L1 = L1 / (q(i,QU)-qaux(i,QC))
       L2 = 0.0d0
       L3 = 0.0d0
       L4 = L4 / (q(i,QU)+qaux(i,QC))
     else
       L1 = L1 / (q(i,QU)-qaux(i,QC))
       L2 = L2 / q(i,QU)
       L3 = 0.0d0
       L4 = L4 / (q(i,QU)+qaux(i,QC))
     endif
     
       
     ! Compute new spatial derivative
     drhodx = (L2 + 0.5d0*(L1 + L4))/(qaux(i,QC)**2.0d0)  
     dudx   = (L4-L1)/(2.0d0*qaux(i,QC)*q(i,QRHO))
     dpdx   = 0.5d0*(L1+L4)

     ! 2nd order
     q(i+1,QU)    = q(i-1,QU) + 2.0d0*dx*dudx
     q(i+1,QRHO)  = q(i-1,QRHO)  + 2.0d0*dx*drhodx
     q(i+1,QPRES) = q(i-1,QPRES) + 2.0d0*dx*dpdx

     !----------------
     q(i+2,QU)    = -2.0d0*q(i-1,QU) - 3.0d0*q(i,QU) + 6.0d0*q(i+1,QU) - 6.0d0*dx*dudx
     q(i+2,QRHO)  = -2.0d0*q(i-1,QRHO) - 3.0d0*q(i,QRHO) + 6.0d0*q(i+1,QRHO) - 6.0d0*dx*drhodx
     q(i+2,QPRES) = -2.0d0*q(i-1,QPRES) - 3.0d0*q(i,QPRES) + 6.0d0*q(i+1,QPRES) - 6.0d0*dx*dpdx

     q(i+3,QU)    = 3.0d0*q(i-1,QU) +10.0d0*q(i,QU) - 18.0d0*q(i+1,QU) + 6.0d0*q(i+2,QU) + 12.0d0*dx*dudx
     q(i+3,QRHO)  = 3.0d0*q(i-1,QRHO) +10.0d0*q(i,QRHO) - 18.0d0*q(i+1,QRHO) + 6.0d0*q(i+2,QRHO) + 12.0d0*dx*drhodx
     q(i+3,QPRES)  = 3.0d0*q(i-1,QPRES) +10.0d0*q(i,QPRES) - 18.0d0*q(i+1,QPRES) + 6.0d0*q(i+2,QPRES) + 12.0d0*dx*dpdx

     q(i+4,QU)    = -2.0d0*q(i-1,QU) - 13.0d0*q(i,QU) + 24.0d0*q(i+1,QU) - 12.0d0*q(i+2,QU)  & 
                     + 4.0d0*q(i+3,QU) - 12.0d0*dx*dudx 
     q(i+4,QRHO)  = -2.0d0*q(i-1,QRHO) - 13.0d0*q(i,QRHO) + 24.0d0*q(i+1,QRHO) - 12.0d0*q(i+2,QRHO) &
                     + 4.0d0*q(i+3,QRHO) - 12.0d0*dx*drhodx
     q(i+4,QPRES) = -2.0d0*q(i-1,QPRES) - 13.0d0*q(i,QPRES) + 24.0d0*q(i+1,QPRES) - 12.0d0*q(i+2,QPRES) &
                     + 4.0d0*q(i+3,QPRES) - 12.0d0*dx*dpdx

     if ((bc_type .eq. NoSlipWall).or.(bc_type .eq. SlipWall)) then

       q(i+1,QU)    = -q(i,QU)
       q(i+2,QU)    = -q(i-1,QU)
       q(i+3,QU)    = -q(i-2,QU)
       q(i+4,QU)    = -q(i-3,QU)

       q(i+1,QRHO)  = q(i,QRHO)
       q(i+2,QRHO)  = q(i-1,QRHO)
       q(i+3,QRHO)  = q(i-2,QRHO)
       q(i+4,QRHO)  = q(i-3,QRHO)

       q(i+1,QPRES)  = q(i,QPRES)
       q(i+2,QPRES)  = q(i-1,QPRES)
       q(i+3,QPRES)  = q(i-2,QPRES)
       q(i+4,QPRES)  = q(i-3,QPRES)
     
     end if

     ! Recompute missing values thanks to EOS
     do hop= domhi(1)+1,domhi(1)+4,1
     
       eos_state % p        = q(hop,QPRES )
       eos_state % rho      = q(hop,QRHO  )
       !eos_state % massfrac = q(hop,QFS:QFS+nspecies-1)
       eos_state % aux      = q(hop,QFX:QFX+naux-1)

       call eos_rp(eos_state)
       q(hop,QTEMP)  = eos_state % T
       q(hop,QREINT) = eos_state % e * q(hop,QRHO)
       q(hop,QGAME)  = q(hop,QPRES) / q(hop,QREINT) + ONE
       
       qaux(hop,QDPDR)  = eos_state % dpdr_e
       qaux(hop,QDPDE)  = eos_state % dpde
       qaux(hop,QGAMC)  = eos_state % gam1
       qaux(hop,QC   )  = eos_state % cs
       qaux(hop,QCSML)  = max(small, small * qaux(hop,QC))

       uin(hop,URHO )  = eos_state % rho 
       uin(hop,UMX  )  = q(hop,QU ) * eos_state % rho 
       uin(hop,UMY  )  = 0.0d0
       uin(hop,UMZ  ) = 0.0d0
       uin(hop,UEINT) = eos_state % rho   *  eos_state % e
       uin(hop,UEDEN) = eos_state % rho  &
          * (eos_state % e + 0.5d0 * (uin(hop,UMX)**2 + uin(hop,UMY)**2))
       uin(hop,UTEMP) = eos_state % T
       do n=1, nspecies
          uin(hop,UFS+n-1) = eos_state % rho  *  eos_state % massfrac(n)
       end do 
     enddo

endif


call destroy(eos_state)

end subroutine impose_NSCBC
