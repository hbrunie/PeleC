subroutine amrex_probinit (init,name,namlen,problo,probhi) bind(c)

  implicit none

  integer          :: init, namlen
  integer          :: name(namlen)
  double precision :: problo(3), probhi(3)

end subroutine amrex_probinit


! ::: -----------------------------------------------------------
! ::: This routine is called at problem setup time and is used
! ::: to initialize data on each grid.  
! ::: 
! ::: NOTE:  all arrays have one cell of ghost zones surrounding
! :::        the grid interior.  Values in these cells need not
! :::        be set here.
! ::: 
! ::: INPUTS/OUTPUTS:
! ::: 
! ::: level     => amr level of grid
! ::: time      => time at which to init data             
! ::: lo,hi     => index limits of grid interior (cell centered)
! ::: nvar      => number of state components.
! ::: state     <= scalar array
! ::: dx        => cell size
! ::: xlo, xhi  => physical locations of lower left and upper
! :::              right hand corner of grid.  (does not include
! :::		   ghost region).
! ::: -----------------------------------------------------------

subroutine pc_initdata(level,time,lo,hi,nvar, &
                       state,state_lo,state_hi, &
                       dx,xlo,xhi) &
                       bind(C, name="pc_initdata")

  use amrex_fort_module

  implicit none

  integer :: level, nvar
  integer :: lo(3), hi(3)
  integer :: state_lo(3), state_hi(3)
  double precision :: xlo(3), xhi(3), time, dx(3)
  double precision :: state(state_lo(1):state_hi(1), &
                            state_lo(2):state_hi(2), &
                            state_lo(3):state_hi(3),nvar)

  ! Remove this call if you're defining your own problem; it is here to 
  ! ensure that you cannot run PeleC if you haven't got your own copy of this function.

  call bl_error("pc_initdata has not been defined for this problem!")

end subroutine pc_initdata

subroutine pc_prob_close() &
     bind(C, name="pc_prob_close")

  ! Do nothing, potentially overridden by something more useful in user replace of this file

end subroutine pc_prob_close
