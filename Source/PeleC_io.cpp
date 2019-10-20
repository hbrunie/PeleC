
#ifndef WIN32
#include <unistd.h>
#endif

#include <iomanip>
#include <iostream>
#include <string>
#include <ctime>

#include <AMReX_Utility.H>
#include "PeleC.H"
#include "PeleC_F.H"
#include "PeleC_io.H"
#include <AMReX_ParmParse.H>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef AMREX_USE_EB
#include <AMReX_EBMultiFabUtil.H>
#endif

#include "AMReX_buildInfo.H"
#ifdef REACTIONS
#include "chemistry_file.H"
#endif

using std::string;
using namespace amrex;

const std::string ascii_spray_particle_file("Spray");

// PeleC maintains an internal checkpoint version numbering system.
// This allows us to maintain backwards compatibility with checkpoints
// generated by old versions of the code, so that new versions can
// restart from them. The version number is stored in the PeleCHeader
// file inside a checkpoint. The following were the changes that were made
// in updating version numbers:
// 0: all checkpoints as of 11/21/16
// 1: add body state

namespace
{
    int input_version = -1;
    int current_version = 1;
    std::string body_state_filename = "body_state.fab";
    Real vfraceps = 0.000001;
}

// I/O routines for PeleC

void
PeleC::restart (Amr&     papa,
		istream& is,
		bool     bReadSpecial)
{
    // Let's check PeleC checkpoint version first;
    // trying to read from checkpoint; if nonexisting, set it to 0.
    if (input_version == -1) {
   	if (ParallelDescriptor::IOProcessor()) {
   	    std::ifstream PeleCHeaderFile;
   	    std::string FullPathPeleCHeaderFile = papa.theRestartFile();
   	    FullPathPeleCHeaderFile += "/PeleCHeader";
   	    PeleCHeaderFile.open(FullPathPeleCHeaderFile.c_str(), std::ios::in);
   	    if (PeleCHeaderFile.good()) {
		char foo[256];
		// first line: Checkpoint version: ?
		PeleCHeaderFile.getline(foo, 256, ':');  
		PeleCHeaderFile >> input_version;
   		PeleCHeaderFile.close();
  	    } else {
   		input_version = 0;
   	    }
   	}
  	ParallelDescriptor::Bcast(&input_version, 1, ParallelDescriptor::IOProcessorNumber());
    }
 
    BL_ASSERT(input_version >= 0);

    // also need to mod checkPoint function to store the new version in a text file
    AmrLevel::restart(papa,is,bReadSpecial);

    /*
      Deal here with new state descriptor types added, with corresponding input_version > 0,
      if applicable
     */
    Vector<int> state_in_checkpoint(desc_lst.size() , 1);
    set_state_in_checkpoint(state_in_checkpoint);
    for (int i = 0; i < desc_lst.size(); ++i)
    {
      if (state_in_checkpoint[i] == 0)
      {
        const Real ctime = state[i-1].curTime();
        state[i].define(geom.Domain(),
                        grids,
                        dmap,
                        desc_lst[i],
                        ctime,
                        parent->dtLevel(level),
                        *m_factory);
	state[i] = state[i-1];
      }
    }
    buildMetrics();

#ifdef PELE_USE_EB
    init_eb(geom, grids, dmap);
#endif

    MultiFab& S_new = get_new_data(State_Type);

    for (int n = 0; n < src_list.size(); ++n)
    {
      old_sources[src_list[n]] = std::unique_ptr<MultiFab>(new MultiFab(grids, dmap, NUM_STATE, NUM_GROW,
                                                                        MFInfo(), Factory()));
      new_sources[src_list[n]] = std::unique_ptr<MultiFab>(new MultiFab(grids, dmap, NUM_STATE, S_new.nGrow(),
                                                                        MFInfo(), Factory()));
    }

    if (do_hydro)
    {
      Sborder.define(grids,dmap,NUM_STATE,NUM_GROW,MFInfo(),Factory());
    }
    else if (do_diffuse)
    {
      Sborder.define(grids,dmap,NUM_STATE,nGrowTr,MFInfo(),Factory());
    }

    if (!do_mol_AD)
    {
      if (do_hydro)
      {
        hydro_source.define(grids,dmap, NUM_STATE,0,MFInfo(),Factory());

        // This array holds the sum of all source terms that affect the hydrodynamics.
        // If we are doing the source term predictor, we'll also use this after the
        // hydro update to store the sum of the new-time sources, so that we can
        // compute the time derivative of the source terms.
        sources_for_hydro.define(grids,dmap,NUM_STATE,NUM_GROW,MFInfo(),Factory());
      }
    }
    else
    {
      Sborder.define(grids,dmap,NUM_STATE,nGrowTr,MFInfo(),Factory());
    }

    // get the elapsed CPU time to now;
    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      // get elapsed CPU time
      std::ifstream CPUFile;
      std::string FullPathCPUFile = parent->theRestartFile();
      FullPathCPUFile += "/CPUtime";
	CPUFile.open(FullPathCPUFile.c_str(), std::ios::in);

	CPUFile >> previousCPUTimeUsed;
	CPUFile.close();

	std::cout << "read CPU time: " << previousCPUTimeUsed << "\n";

    }

    if (track_grid_losses && level == 0)
    {

	// get the current value of the diagnostic quantities
	std::ifstream DiagFile;
	std::string FullPathDiagFile = parent->theRestartFile();
	FullPathDiagFile += "/Diagnostics";
	DiagFile.open(FullPathDiagFile.c_str(), std::ios::in);

	for (int i = 0; i < n_lost; i++)
	    DiagFile >> material_lost_through_boundary_cumulative[i];

	DiagFile.close();

    }


    if (level == 0)
    {
	// get problem-specific stuff -- note all processors do this,
	// eliminating the need for a broadcast
	std::string dir = parent->theRestartFile();

	char * dir_for_pass = new char[dir.size() + 1];
	std::copy(dir.begin(), dir.end(), dir_for_pass);
	dir_for_pass[dir.size()] = '\0';

	int len = dir.size();

	Vector<int> int_dir_name(len);
	for (int j = 0; j < len; j++)
	    int_dir_name[j] = (int) dir_for_pass[j];

	BL_FORT_PROC_CALL(PROBLEM_RESTART,problem_restart)(int_dir_name.dataPtr(), &len);      

	delete [] dir_for_pass;

    }

    if (level > 0 && do_reflux) {
        flux_reg.define(grids, papa.boxArray(level-1),
                        dmap, papa.DistributionMap(level-1),
                        geom, papa.Geom(level-1),
                        papa.refRatio(level-1), level, NUM_STATE);

	if (!DefaultGeometry().IsCartesian()) {
            pres_reg.define(grids, papa.boxArray(level-1),
                            dmap, papa.DistributionMap(level-1),
                            geom, papa.Geom(level-1),
                            papa.refRatio(level-1), level, 1);
	}
    }

#ifdef AMREX_USE_EB
    if (input_version > 0 && level==0 && !no_eb_in_domain) {
        body_state.resize(NUM_STATE);
        if (ParallelDescriptor::IOProcessor()) {
	    std::ifstream BodyFile;
	    std::string FullPathBodyFile = parent->theRestartFile();
	    FullPathBodyFile += "/" + body_state_filename;
	    BodyFile.open(FullPathBodyFile.c_str(), std::ios::in);
            FArrayBox bstate_fab;
            bstate_fab.readFrom(BodyFile);
            BodyFile.close();
            if (bstate_fab.nComp() != NUM_STATE) amrex::Abort("Body state incompatible with checkpointed version");
            IntVect iv(bstate_fab.box().smallEnd());
            for (int n=0; n<NUM_STATE; ++n) {
                body_state[n] = bstate_fab(iv,n);
            }
        }
        ParallelDescriptor::Bcast(&(body_state[0]), body_state.size(),ParallelDescriptor::IOProcessorNumber());
        body_state_set = true;
    }
#endif

  // initialize the Godunov state array used in hydro
  if (do_hydro)
  {
    init_godunov_indices();
  }

}

void
PeleC::set_state_in_checkpoint (Vector<int>& state_in_checkpoint)
{
  for (int i=0; i<num_state_type; ++i){
	state_in_checkpoint[i] = 1;

    if( i == Work_Estimate_Type )
    {
      state_in_checkpoint[i] = 0;
    }
  }

}

void
PeleC::checkPoint(const std::string& dir,
		  std::ostream&  os,
		  VisMF::How     how,
		  bool dump_old_default)
{
    AmrLevel::checkPoint(dir, os, how, dump_old);

#ifdef AMREX_PARTICLES
   bool is_checkpoint = true;

   Vector<std::string> real_comp_names;
   Vector<std::string>  int_comp_names;
   real_comp_names.push_back("xvel");
   real_comp_names.push_back("yvel");
#if (BL_SPACEDIM > 2)
   real_comp_names.push_back("zvel");
#endif
   real_comp_names.push_back("temperature");
   real_comp_names.push_back("diam");
   real_comp_names.push_back("density");
// real_comp_names.push_back("mass_frac");
   AMREX_ASSERT(real_comp_names.size()==NSR_SPR);

  if (PeleC::theSprayPC()) 
  {
       PeleC::theSprayPC()->Checkpoint(dir,"Spray",
                                       is_checkpoint,real_comp_names,int_comp_names);

       // Here we write ascii information every time we write a checkpoint file
       if (level == 0)
       {
         if (do_spray_particles==1) {
           theSprayPC()->Checkpoint(dir, ascii_spray_particle_file);
           std::string fname = "spray" + dir.substr (3,6) + ".p3d";
           theSprayPC()->WriteAsciiFile(fname);
         }
       }
  }
#endif

    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
	{
	    std::ofstream PeleCHeaderFile;
	    std::string FullPathPeleCHeaderFile = dir;
	    FullPathPeleCHeaderFile += "/PeleCHeader";
	    PeleCHeaderFile.open(FullPathPeleCHeaderFile.c_str(), std::ios::out);

	    PeleCHeaderFile << "Checkpoint version: " << current_version << std::endl;
	    PeleCHeaderFile.close();
	}

	{
	    // store elapsed CPU time
	    std::ofstream CPUFile;
	    std::string FullPathCPUFile = dir;
	    FullPathCPUFile += "/CPUtime";
	    CPUFile.open(FullPathCPUFile.c_str(), std::ios::out);

	    CPUFile << std::setprecision(15) << getCPUTime();
	    CPUFile.close();
	}

	if (track_grid_losses) {

	    // store diagnostic quantities
            std::ofstream DiagFile;
	    std::string FullPathDiagFile = dir;
	    FullPathDiagFile += "/Diagnostics";
	    DiagFile.open(FullPathDiagFile.c_str(), std::ios::out);

	    for (int i = 0; i < n_lost; i++)
		DiagFile << std::setprecision(15) << material_lost_through_boundary_cumulative[i] << std::endl;

	    DiagFile.close();

	}

	{
	    // store any problem-specific stuff
	    char * dir_for_pass = new char[dir.size() + 1];
	    std::copy(dir.begin(), dir.end(), dir_for_pass);
	    dir_for_pass[dir.size()] = '\0';

	    int len = dir.size();

	    Vector<int> int_dir_name(len);
	    for (int j = 0; j < len; j++)
		int_dir_name[j] = (int) dir_for_pass[j];

	    BL_FORT_PROC_CALL(PROBLEM_CHECKPOINT,problem_checkpoint)(int_dir_name.dataPtr(), &len);      

	    delete [] dir_for_pass;
	}
    }

#ifdef AMREX_USE_EB
    if (current_version > 0) {
        if (ParallelDescriptor::IOProcessor() && !no_eb_in_domain) {
            IntVect iv(D_DECL(0,0,0));
            FArrayBox bstate_fab(Box(iv,iv),NUM_STATE);
            for (int n=0; n<NUM_STATE; ++n) {
                bstate_fab(iv,n) = body_state[n];
            }

	    std::ofstream BodyFile;
	    std::string FullPathBodyFile = dir;
	    FullPathBodyFile += "/" + body_state_filename;
	    BodyFile.open(FullPathBodyFile.c_str(), std::ios::out);
            bstate_fab.writeOn(BodyFile);
            BodyFile.close();
        }
    }
#endif
}

std::string
PeleC::thePlotFileType () const
{
    //
    // Increment this whenever the writePlotFile() format changes.
    //
#ifdef AMREX_USE_EB
    static const std::string the_plot_file_type = no_eb_in_domain ? "HyperCLaw-V1.1" : "CartGrid-V2.0";
#else
    static const std::string the_plot_file_type = "HyperCLaw-V1.1";
#endif

    return the_plot_file_type;
}

void
PeleC::setPlotVariables ()
{
    AmrLevel::setPlotVariables();

    ParmParse pp("pelec");

#ifdef AMREX_USE_EB
    bool plot_vfrac = !no_eb_in_domain;
    pp.query("plot_vfrac",plot_vfrac);
    if (plot_vfrac) {
        parent->addDerivePlotVar("vfrac");
    }
    else if (parent->isDerivePlotVar("vfrac")) {
        parent->deleteDerivePlotVar("vfrac");
    }
#endif
    bool plot_cost=true;
    pp.query("plot_cost",plot_cost);
    if (plot_cost) {
        parent->addDerivePlotVar("WorkEstimate");
    }

    bool plot_rhoy = true;
    pp.query("plot_rhoy",plot_rhoy);
    if (!plot_rhoy) {
	for (int i = 0; i < NumSpec; i++) {
	    parent->deleteStatePlotVar(desc_lst[State_Type].name(FirstSpec+i));
	}
    }

    bool plot_massfrac = false;
    pp.query("plot_massfrac",plot_massfrac);
//    if (plot_massfrac)
//    {
//	if (plot_massfrac)
//	{
//	    //
//	    // Get the number of species from the network model.
//	    //
//	    get_num_spec(&NumSpec);
//	    //
//	    // Get the species names from the network model.
//	    //
//	    for (int i = 0; i < NumSpec; i++)
//	    {
//		int len = 20;
//		Vector<int> int_spec_names(len);
//		//
//		// This call return the actual length of each string in "len"
//		//
//		get_spec_names(int_spec_names.dataPtr(),&i,&len);
//		char* spec_name = new char[len+1];
//		for (int j = 0; j < len; j++)
//		    spec_name[j] = int_spec_names[j];
//		spec_name[len] = '\0';
//		string spec_string = "Y(";
//		spec_string += spec_name;
//		spec_string += ')';
//		parent->addDerivePlotVar(spec_string);
//		delete [] spec_name;
//	    }
//	}
//    }
 
    if (plot_massfrac) {
        parent->addDerivePlotVar("massfrac");
    } else {
        parent->deleteDerivePlotVar("massfrac");
    }
    
    bool plot_moleFrac = false;
    pp.query("plot_molefrac",plot_moleFrac);
    if (plot_moleFrac) {
        parent->addDerivePlotVar("molefrac");
    } else {
        parent->deleteDerivePlotVar("molefrac");
    }
    
}



void
PeleC::writeJobInfo (const std::string& dir)

{

    // job_info file with details about the run
    std::ofstream jobInfoFile;
    std::string FullPathJobInfoFile = dir;
    FullPathJobInfoFile += "/job_info";
    jobInfoFile.open(FullPathJobInfoFile.c_str(), std::ios::out);

    std::string PrettyLine = "===============================================================================\n";
    std::string OtherLine = "--------------------------------------------------------------------------------\n";
    std::string SkipSpace = "        ";


    // job information
    jobInfoFile << PrettyLine;
    jobInfoFile << " PeleC Job Information\n";
    jobInfoFile << PrettyLine;

    jobInfoFile << "job name: " << job_name << "\n\n";
    jobInfoFile << "inputs file: " << inputs_name << "\n\n";

    jobInfoFile << "number of MPI processes: " << ParallelDescriptor::NProcs() << "\n";
#ifdef _OPENMP
    jobInfoFile << "number of threads:       " << omp_get_max_threads() << "\n";
#endif

    jobInfoFile << "\n";
    jobInfoFile << "CPU time used since start of simulation (CPU-hours): " <<
	getCPUTime()/3600.0;

    jobInfoFile << "\n\n";

    // plotfile information
    jobInfoFile << PrettyLine;
    jobInfoFile << " Plotfile Information\n";
    jobInfoFile << PrettyLine;

    time_t now = time(0);

    // Convert now to tm struct for local timezone
    tm* localtm = localtime(&now);
    jobInfoFile   << "output data / time: " << asctime(localtm);

    char currentDir[FILENAME_MAX];
    if (getcwd(currentDir, FILENAME_MAX)) {
	jobInfoFile << "output dir:         " << currentDir << "\n";
    }

    jobInfoFile << "\n\n";


    // build information
    jobInfoFile << PrettyLine;
    jobInfoFile << " Build Information\n";
    jobInfoFile << PrettyLine;

    jobInfoFile << "build date:    " << buildInfoGetBuildDate() << "\n";
    jobInfoFile << "build machine: " << buildInfoGetBuildMachine() << "\n";
    jobInfoFile << "build dir:     " << buildInfoGetBuildDir() << "\n";
    jobInfoFile << "AMReX dir:     " << buildInfoGetAMReXDir() << "\n";

    jobInfoFile << "\n";

    jobInfoFile << "COMP:          " << buildInfoGetComp() << "\n";
    jobInfoFile << "COMP version:  " << buildInfoGetCompVersion() << "\n";
    jobInfoFile << "FCOMP:         " << buildInfoGetFcomp() << "\n";
    jobInfoFile << "FCOMP version: " << buildInfoGetFcompVersion() << "\n";

    jobInfoFile << "\n";

    for (int n = 1; n <= buildInfoGetNumModules(); n++) {
	jobInfoFile << buildInfoGetModuleName(n) << ": " << buildInfoGetModuleVal(n) << "\n";
    }

    jobInfoFile << "\n";

    const char* githash1 = buildInfoGetGitHash(1);
    const char* githash2 = buildInfoGetGitHash(2);
    const char* githash3 = buildInfoGetGitHash(3);
    if (strlen(githash1) > 0) {
	jobInfoFile << "PeleC       git hash: " << githash1 << "\n";
    }
    if (strlen(githash2) > 0) {
	jobInfoFile << "AMReX       git hash: " << githash2 << "\n";
    }
    if (strlen(githash3) > 0) {	
	jobInfoFile << "PelePhysics git hash: " << githash3 << "\n";
    }

    const char* buildgithash = buildInfoGetBuildGitHash();
    const char* buildgitname = buildInfoGetBuildGitName();
    if (strlen(buildgithash) > 0){
	jobInfoFile << buildgitname << " git hash: " << buildgithash << "\n";
    }

    jobInfoFile << "\n\n";


    // grid information
    jobInfoFile << PrettyLine;
    jobInfoFile << " Grid Information\n";
    jobInfoFile << PrettyLine;

    int f_lev = parent->finestLevel();

    for (int i = 0; i <= f_lev; i++)
    {
	jobInfoFile << " level: " << i << "\n";
	jobInfoFile << "   number of boxes = " << parent->numGrids(i) << "\n";
	jobInfoFile << "   maximum zones   = ";
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	    jobInfoFile << parent->Geom(i).Domain().length(n) << " ";
	    //jobInfoFile << parent->Geom(i).ProbHi(n) << " ";
	}
	jobInfoFile << "\n\n";
    }

    jobInfoFile << " Boundary conditions\n";
    Vector<string> lo_bc_out(BL_SPACEDIM);
    Vector<string> hi_bc_out(BL_SPACEDIM);
    ParmParse pp("pelec");
    pp.getarr("lo_bc",lo_bc_out,0,BL_SPACEDIM);
    pp.getarr("hi_bc",hi_bc_out,0,BL_SPACEDIM);


    // these names correspond to the integer flags setup in the
    // PeleC_setup.cpp

    jobInfoFile << "   -x: " << lo_bc_out[0] << "\n";
    jobInfoFile << "   +x: " << hi_bc_out[0] << "\n";
    if (BL_SPACEDIM >= 2) {
	jobInfoFile << "   -y: " << lo_bc_out[1] << "\n";
	jobInfoFile << "   +y: " << hi_bc_out[1] << "\n";
    }
    if (BL_SPACEDIM == 3) {
	jobInfoFile << "   -z: " << lo_bc_out[2] << "\n";
	jobInfoFile << "   +z: " << hi_bc_out[2] << "\n";
    }

    jobInfoFile << "\n\n";


    int mlen = 20;

    jobInfoFile << PrettyLine;
    jobInfoFile << " Species Information\n";
    jobInfoFile << PrettyLine;

    jobInfoFile <<
	std::setw(6) << "index" << SkipSpace <<
	std::setw(mlen+1) << "name" << SkipSpace <<
	std::setw(7) << "A" << SkipSpace <<
	std::setw(7) << "Z" << "\n";
    jobInfoFile << OtherLine;

    for (int i = 0; i < NumSpec; i++)
    {

	int len = mlen;
	Vector<int> int_spec_names(len);
	//
	// This call return the actual length of each string in "len"
	//
	get_spec_names(int_spec_names.dataPtr(),&i,&len);
	char* spec_name = new char[len+1];
	for (int j = 0; j < len; j++) 
	    spec_name[j] = int_spec_names[j];
	spec_name[len] = '\0';

	delete [] spec_name;
    }
    jobInfoFile << "\n\n";


    // runtime parameters
    jobInfoFile << PrettyLine;
    jobInfoFile << " Inputs File Parameters\n";
    jobInfoFile << PrettyLine;

    ParmParse::dumpTable(jobInfoFile, true);
    jobInfoFile.close();

}

/*
 * PeleC::writeBuildInfo
 * Similar to writeJobInfo, but the subset of information that makes sense without
 * an input file to enable --describe in format similar to CASTRO
 *
 */

void
PeleC::writeBuildInfo (std::ostream& os)

{
    std::string PrettyLine = std::string(78, '=') + "\n";
    std::string OtherLine = std::string(78, '-') + "\n";
    std::string SkipSpace = std::string(8, ' ');

 // build information
    os << PrettyLine;
    os << " PeleC Build Information\n";
    os << PrettyLine;

    os << "build date:    " << buildInfoGetBuildDate() << "\n";
    os << "build machine: " << buildInfoGetBuildMachine() << "\n";
    os << "build dir:     " << buildInfoGetBuildDir() << "\n";
    os << "AMReX dir:     " << buildInfoGetAMReXDir() << "\n";

    os << "\n";

    os << "COMP:          " << buildInfoGetComp() << "\n";
    os << "COMP version:  " << buildInfoGetCompVersion() << "\n";


    std::cout << "C++ compiler:  " << buildInfoGetCXXName() << "\n";
    std::cout << "C++ flags:     " << buildInfoGetCXXFlags() << "\n";

    os << "\n";

    os << "FCOMP:         " << buildInfoGetFcomp() << "\n";
    os << "FCOMP version: " << buildInfoGetFcompVersion() << "\n";

    os << "\n";


    std::cout << "Link flags:    " << buildInfoGetLinkFlags() << "\n";
    std::cout << "Libraries:     " << buildInfoGetLibraries() << "\n";

    os << "\n";

    for (int n = 1; n <= buildInfoGetNumModules(); n++) {
	os << buildInfoGetModuleName(n) << ": " << buildInfoGetModuleVal(n) << "\n";
    }

    os << "\n";
    const char* githash1 = buildInfoGetGitHash(1);
    const char* githash2 = buildInfoGetGitHash(2);
    const char* githash3 = buildInfoGetGitHash(3);
    if (strlen(githash1) > 0) {
	os << "PeleC       git hash: " << githash1 << "\n";
    }
    if (strlen(githash2) > 0) {
	os << "AMReX       git hash: " << githash2 << "\n";
    }
    if (strlen(githash3) > 0) {
	os << "PelePhysics git hash: " << githash3 << "\n";
    }

    const char* buildgithash = buildInfoGetBuildGitHash();
    const char* buildgitname = buildInfoGetBuildGitName();
    if (strlen(buildgithash) > 0){
	os << buildgitname << " git hash: " << buildgithash << "\n";
    }

    os << "\n";
    os << " PeleC Compile time variables: \n";

#ifdef REACTIONS
    int mm, kk, ii, nfit;
    CKINDX(&mm, &kk, &ii, &nfit);
    os << std::setw(40) << std::left << "Number elements from chem cpp : " << mm << std::endl;
    os << std::setw(40) << std::left << "Number species from chem cpp : " << kk << std::endl;
    os << std::setw(40) << std::left << "Number reactions from chem cpp : " << ii << std::endl;
#endif



    os << "\n";
    os << " PeleC Defines: \n";
#ifdef _OPENMP
    os << std::setw(35) << std::left << "_OPENMP " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "_OPENMP " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef MPI_VERSION
    os << std::setw(35) << std::left << "MPI_VERSION " << std::setw(6) << MPI_VERSION << std::endl;
#else
    os << std::setw(35) << std::left << "MPI_VERSION " << std::setw(6) << "UNDEFINED" << std::endl;
#endif

#ifdef MPI_SUBVERSION
    os << std::setw(35) << std::left << "MPI_SUBVERSION " << std::setw(6) << MPI_SUBVERSION << std::endl;
#else
    os << std::setw(35) << std::left << "MPI_SUBVERSION " << std::setw(6) << "UNDEFINED" << std::endl;
#endif

#ifdef REACTIONS
    os << std::setw(35) << std::left << "REACTIONS " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "REACTIONS " << std::setw(6) << "OFF" << std::endl;
#endif
#ifdef NUM_ADV
    os << std::setw(35) << std::left << "NUM_ADV=" << NUM_ADV << std::endl;
#else
    os << std::setw(35) << std::left << "NUM_ADV" << "is undefined (0)" << std::endl;
#endif


#ifdef DO_HIT_FORCE
    os << std::setw(35) << std::left << "DO_HIT_FORCE " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "DO_HIT_FORCE " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef PELEC_USE_EB
    os << std::setw(35) << std::left << "PELEC_USE_EB " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "PELEC_USE_EB " << std::setw(6) << "OFF" << std::endl;
#endif


#ifdef USE_MASA
    os << std::setw(35) << std::left << "USE_MASA " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "USE_MASA " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef PELE_USE_EB
    os << std::setw(35) << std::left << "PELE_USE_EB " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "PELE_USE_EB " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef AMREX_USE_EB
    os << std::setw(35) << std::left << "AMREX_USE_EB " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "AMREX_USE_EB " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef AMREX_PARTICLES
    os << std::setw(35) << std::left << "AMREX_PARTICLES " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "AMREX_PARTICLES " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef PELE_UNIT_TEST_DN
    os << std::setw(35) << std::left << "PELE_UNIT_TEST_DN " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "PELE_UNIT_TEST_DN " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef PELEC_USE_MOL
    os << std::setw(35) << std::left << "PELEC_USE_MOL " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "PELEC_USE_MOL " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef DO_PROBLEM_POST_TIMESTEP
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_TIMESTEP " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_TIMESTEP " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef DO_PROBLEM_POST_RESTART
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_RESTART " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_RESTART " << std::setw(6) << "OFF" << std::endl;
#endif

#ifdef DO_PROBLEM_POST_INIT
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_INIT " << std::setw(6) << "ON" << std::endl;
#else
    os << std::setw(35) << std::left << "DO_PROBLEM_POST_INIT " << std::setw(6) << "OFF" << std::endl;
#endif


    os << "\n\n";


}


void
PeleC::writePlotFile (const std::string& dir,
		      ostream&       os,
		      VisMF::How     how)
{
#ifdef USE_ASCENT
    writeAscentPlotFile( parent->levelSteps(0),parent->stepOfLastPlotFile());
#endif
    int i, n;
    //
    // The list of indices of State to write to plotfile.
    // first component of pair is state_type,
    // second component of pair is component # within the state_type
    //
    std::vector<std::pair<int,int> > plot_var_map;
    for (int typ = 0; typ < desc_lst.size(); typ++)
        for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
            if (parent->isStatePlotVar(desc_lst[typ].name(comp)) &&
                desc_lst[typ].getType() == IndexType::TheCellType())
                plot_var_map.push_back(std::pair<int,int>(typ,comp));

    int num_derive = 0;
    std::list<std::string> derive_names;
    const std::list<DeriveRec>& dlist = derive_lst.dlist();

    for (std::list<DeriveRec>::const_iterator it = dlist.begin(), end = dlist.end();
       it != end;
       ++it)    
    {
        if (parent->isDerivePlotVar(it->name()))
        {
#ifdef AMREX_PARTICLES
            if (it->name() == "particle_count" ||
                it->name() == "total_particle_count" ||
                it->name() == "particle_density")
            {
                if (PeleC::theSprayPC())
                {
                    derive_names.push_back(it->name());
                    num_derive++;
                }
            } else
#endif
	    {
		derive_names.push_back(it->name());
                num_derive += it->numDerive();
	    }
	}
    }

    int n_data_items = plot_var_map.size() + num_derive;

    Real cur_time = state[State_Type].curTime();

    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
        //
        // The first thing we write out is the plotfile type.
        //
        os << thePlotFileType() << '\n';

        if (n_data_items == 0)
            amrex::Error("Must specify at least one valid data item to plot");

        os << n_data_items << '\n';

	//
	// Names of variables -- first state, then derived
	//
	for (i =0; i < plot_var_map.size(); i++)
        {
	    int typ = plot_var_map[i].first;
	    int comp = plot_var_map[i].second;
	    os << desc_lst[typ].name(comp) << '\n';
        }

  for (std::list<std::string>::const_iterator it = derive_names.begin(), end = derive_names.end();
         it != end;
         ++it)
    {
      const DeriveRec* rec = derive_lst.get(*it);
      for (i = 0; i < rec->numDerive(); i++)
        os << rec->variableName(i) << '\n';
    }
      
        os << BL_SPACEDIM << '\n';
        os << parent->cumTime() << '\n';
        int f_lev = parent->finestLevel();
        os << f_lev << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << DefaultGeometry().ProbLo(i) << ' ';
        os << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << DefaultGeometry().ProbHi(i) << ' ';
        os << '\n';
        for (i = 0; i < f_lev; i++)
            os << parent->refRatio(i)[0] << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->Geom(i).Domain() << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->levelSteps(i) << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
        {
            for (int k = 0; k < BL_SPACEDIM; k++)
                os << parent->Geom(i).CellSize()[k] << ' ';
            os << '\n';
        }
        os << (int) DefaultGeometry().Coord() << '\n';
        os << "0\n"; // Write bndry data.

	writeJobInfo(dir);

    }
    // Build the directory to hold the MultiFab at this level.
    // The name is relative to the directory containing the Header file.
    //
    static const std::string BaseName = "/Cell";
    char buf[64];
    sprintf(buf, "Level_%d", level);
    std::string LevelStr = buf;
    //
    // Now for the full pathname of that directory.
    //
    std::string FullPath = dir;
    if (!FullPath.empty() && FullPath[FullPath.size()-1] != '/')
        FullPath += '/';
    FullPath += LevelStr;
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!amrex::UtilCreateDirectory(FullPath, 0755))
            amrex::CreateDirectoryFailed(FullPath);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    if (ParallelDescriptor::IOProcessor())
    {
        os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
        os << parent->levelSteps(level) << '\n';

        for (i = 0; i < grids.size(); ++i)
        {
            RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
            for (n = 0; n < BL_SPACEDIM; n++)
                os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
        //
        // The full relative pathname of the MultiFabs at this level.
        // The name is relative to the Header file containing this name.
        // It's the name that gets written into the Header.
        //
        if (n_data_items > 0)
        {
            std::string PathNameInHeader = LevelStr;
            PathNameInHeader += BaseName;
            os << PathNameInHeader << '\n';
        }

#ifdef PELE_USE_EB
        if (!no_eb_in_domain && level==parent->finestLevel())  {
          os << vfraceps << '\n';
        }
#endif
    }
    //
    // We combine all of the multifabs -- state, derived, etc -- into one
    // multifab -- plotMF.
    // NOTE: we are assuming that each state variable has one component,
    // but a derived variable is allowed to have multiple components.
    int       cnt   = 0;
    int       ncomp = 1;
    const int nGrow = 0;
    MultiFab  plotMF(grids,dmap,n_data_items,nGrow, MFInfo(), Factory());
    MultiFab* this_dat = 0;
    //
    // Cull data from state variables -- use no ghost cells.
    //
    for (i = 0; i < plot_var_map.size(); i++)
    {
	int typ  = plot_var_map[i].first;
	int comp = plot_var_map[i].second;
	this_dat = &state[typ].newData();
	MultiFab::Copy(plotMF,*this_dat,comp,cnt,1,nGrow);
	cnt++;
    }
    //
    // Cull data from derived variables.
    //
    if (derive_names.size() > 0)
    {

    for (std::list<std::string>::const_iterator it = derive_names.begin(), end = derive_names.end();
         it != end;
         ++it)
	{
      const DeriveRec* rec = derive_lst.get(*it);
      ncomp = rec->numDerive();

	    auto derive_dat = derive(*it,cur_time,nGrow);
	    MultiFab::Copy(plotMF,*derive_dat,0,cnt,ncomp,nGrow);
      cnt += ncomp;
	}
    }

#ifdef AMREX_USE_EB
    // Prefer app-specific one
    //amrex::EB_set_covered(plotMF);
#endif

    //
    // Use the Full pathname when naming the MultiFab.
    //
    std::string TheFullPath = FullPath;
    TheFullPath += BaseName;
    VisMF::Write(plotMF,TheFullPath,how,true);
#ifdef AMREX_PARTICLES
    bool is_checkpoint = false;

    if (PeleC::theSprayPC())
    {
       Vector<std::string> real_comp_names;
       Vector<std::string>  int_comp_names;
       real_comp_names.push_back("xvel");
       real_comp_names.push_back("yvel");
#if (BL_SPACEDIM > 2)
   real_comp_names.push_back("zvel");
#endif
       real_comp_names.push_back("temp");
       real_comp_names.push_back("diam");
       real_comp_names.push_back("density");
//     real_comp_names.push_back("mass_frac");

       PeleC::theSprayPC()->Checkpoint(dir,"PC",is_checkpoint,real_comp_names,int_comp_names);
    }
#endif
}

void
PeleC::writeSmallPlotFile (const std::string& dir,
			   ostream&       os,
			   VisMF::How     how)
{
    int i, n;
    //
    // The list of indices of State to write to plotfile.
    // first component of pair is state_type,
    // second component of pair is component # within the state_type
    //
    std::vector<std::pair<int,int> > plot_var_map;
    for (int typ = 0; typ < desc_lst.size(); typ++)
        for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
            if (parent->isStateSmallPlotVar(desc_lst[typ].name(comp)) &&
                desc_lst[typ].getType() == IndexType::TheCellType())
                plot_var_map.push_back(std::pair<int,int>(typ,comp));

    int n_data_items = plot_var_map.size();

    Real cur_time = state[State_Type].curTime();

    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
        //
        // The first thing we write out is the plotfile type.
        //
        os << thePlotFileType() << '\n';

        if (n_data_items == 0)
            amrex::Error("Must specify at least one valid data item to plot");

        os << n_data_items << '\n';

	//
	// Names of variables -- first state, then derived
	//
	for (i =0; i < plot_var_map.size(); i++)
        {
	    int typ = plot_var_map[i].first;
	    int comp = plot_var_map[i].second;
	    os << desc_lst[typ].name(comp) << '\n';
        }

        os << BL_SPACEDIM << '\n';
        os << parent->cumTime() << '\n';
        int f_lev = parent->finestLevel();
        os << f_lev << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << DefaultGeometry().ProbLo(i) << ' ';
        os << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << DefaultGeometry().ProbHi(i) << ' ';
        os << '\n';
        for (i = 0; i < f_lev; i++)
            os << parent->refRatio(i)[0] << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->Geom(i).Domain() << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->levelSteps(i) << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
        {
            for (int k = 0; k < BL_SPACEDIM; k++)
                os << parent->Geom(i).CellSize()[k] << ' ';
            os << '\n';
        }
        os << (int) DefaultGeometry().Coord() << '\n';
        os << "0\n"; // Write bndry data.

        // job_info file with details about the run
	writeJobInfo(dir);

    }
    // Build the directory to hold the MultiFab at this level.
    // The name is relative to the directory containing the Header file.
    //
    static const std::string BaseName = "/Cell";
    char buf[64];
    sprintf(buf, "Level_%d", level);
    std::string LevelStr = buf;
    //
    // Now for the full pathname of that directory.
    //
    std::string FullPath = dir;
    if (!FullPath.empty() && FullPath[FullPath.size()-1] != '/')
        FullPath += '/';
    FullPath += LevelStr;
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!amrex::UtilCreateDirectory(FullPath, 0755))
            amrex::CreateDirectoryFailed(FullPath);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    if (ParallelDescriptor::IOProcessor())
    {
        os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
        os << parent->levelSteps(level) << '\n';

        for (i = 0; i < grids.size(); ++i)
        {
            RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
            for (n = 0; n < BL_SPACEDIM; n++)
                os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
        //
        // The full relative pathname of the MultiFabs at this level.
        // The name is relative to the Header file containing this name.
        // It's the name that gets written into the Header.
        //
        if (n_data_items > 0)
        {
            std::string PathNameInHeader = LevelStr;
            PathNameInHeader += BaseName;
            os << PathNameInHeader << '\n';
        }
        os << vfraceps << '\n';
    }
    //
    // We combine all of the multifabs -- state, derived, etc -- into one
    // multifab -- plotMF.
    // NOTE: we are assuming that each state variable has one component,
    // but a derived variable is allowed to have multiple components.
    int       cnt   = 0;
    const int nGrow = 0;
    MultiFab  plotMF(grids,dmap,n_data_items,nGrow,MFInfo(),Factory());
    MultiFab* this_dat = 0;
    //
    // Cull data from state variables -- use no ghost cells.
    //
    for (i = 0; i < plot_var_map.size(); i++)
    {
	int typ  = plot_var_map[i].first;
	int comp = plot_var_map[i].second;
	this_dat = &state[typ].newData();
	MultiFab::Copy(plotMF,*this_dat,comp,cnt,1,nGrow);
	cnt++;
    }

    //
    // Use the Full pathname when naming the MultiFab.
    //
    std::string TheFullPath = FullPath;
    TheFullPath += BaseName;
    VisMF::Write(plotMF,TheFullPath,how,true);

}
