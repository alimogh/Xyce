//-------------------------------------------------------------------------
//   Copyright 2002-2022 National Technology & Engineering Solutions of
//   Sandia, LLC (NTESS).  Under the terms of Contract DE-NA0003525 with
//   NTESS, the U.S. Government retains certain rights in this software.
//
//   This file is part of the Xyce(TM) Parallel Electrical Simulator.
//
//   Xyce(TM) is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   Xyce(TM) is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with Xyce(TM).
//   If not, see <http://www.gnu.org/licenses/>.
//-------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose       : .ROL class analysis functions.
// Special Notes :
// Creator       : 
// Creation Date : 
//
//-----------------------------------------------------------------------------
#include <Xyce_config.h>
 
#include <N_ANP_AnalysisManager.h>
#include <N_ANP_ROL.h>

#include <N_ANP_OutputMgrAdapter.h>
#include <N_ANP_DCSweep.h>
#include <N_ANP_SweepParam.h>
#include <N_ANP_SweepParamFreeFunctions.h>
#include <N_IO_CircuitBlock.h>
#include <N_IO_CmdParse.h>
#include <N_IO_InitialConditions.h>
#include <N_IO_OptionBlock.h>
#include <N_IO_PkgOptionsMgr.h>
#include <N_LOA_Loader.h>
#include <N_NLS_Manager.h> // TT: was not included
#include <N_NLS_ObjectiveFunctions.h>
#include <N_TIA_DataStore.h>
#include <N_TIA_StepErrorControl.h>
#include <N_TIA_WorkingIntegrationMethod.h> // TT: was not included

#include <N_UTL_Diagnostic.h>
#include <N_UTL_ExtendedString.h>
#include <N_UTL_Factory.h>
#include <N_UTL_FeatureTest.h>
#include <N_UTL_OptionBlock.h>
#include <N_UTL_Expression.h>
#include <N_UTL_ExpressionData.h>
#include <N_ERH_Message.h>

#include <N_TOP_Topology.h>
#include <N_LAS_System.h>
#include <N_LAS_Builder.h>

#include <N_ANP_StepEvent.h>
#include <N_PDS_Manager.h>
#include <N_UTL_Timer.h>

#include <N_LOA_NonlinearEquationLoader.h>

#ifdef Xyce_ROL

#include "ROL_BoundConstraint.hpp"
#include "ROL_Constraint.hpp"
#include "ROL_Constraint_SimOpt.hpp"
#include "ROL_Objective_SimOpt.hpp"
#include "ROL_ParameterList.hpp"
#include "ROL_Problem.hpp"
#include "ROL_Ptr.hpp"
#include "ROL_Reduced_Objective_SimOpt.hpp"
#include "ROL_Solver.hpp"
#include "ROL_StdVector.hpp"
#include "ROL_Vector.hpp"

#include "Teuchos_oblackholestream.hpp"
#include "Teuchos_GlobalMPISession.hpp"
#include "Teuchos_RCP.hpp"

#include "ROL_XyceVector.hpp"
#include <N_ANP_ROL_DC_Optimization.h>

#endif

// std includes
#include <fstream>

namespace Xyce {
namespace Analysis {
    
//-----------------------------------------------------------------------------
// Function      : ROL::ROL
// Purpose       :
//-----------------------------------------------------------------------------
ROL::ROL(
   AnalysisManager &analysis_manager, 
   Nonlinear::Manager &nonlinear_manager, // TT
   Loader::Loader &loader, 
   Linear::System & linear_system,
   Topo::Topology & topology,
   IO::InitialConditionsManager & initial_conditions_manager)
  : AnalysisBase(analysis_manager, "ROL"),
    analysisManager_(analysis_manager),
    nonlinearManager_(nonlinear_manager), // TT
    loader_(loader),
    topology_(topology),
    initialConditionsManager_(initial_conditions_manager),
    linearSystem_(linear_system),
    outputManagerAdapter_(analysis_manager.getOutputManagerAdapter()),
    stepLoopSize_(0),
    sensFlag_(analysis_manager.getSensFlag()),
    numParams_(0),
    numSensParams_(0),
    paramFile_("parameters.txt"),
    rolParamFile_("input.xml"),
    outputFile_("rol_output.txt"),
    currentAnalysisObject_(0)
{}

//-----------------------------------------------------------------------------
// Function      : ROL::~ROL
// Purpose       :
//-----------------------------------------------------------------------------
ROL::~ROL()
{}
  
//-----------------------------------------------------------------------------
// Function      : ROL::setROLOptions
// Purpose       :
// Special Notes : These are from '.rol'
// Scope         : public
// Creator       : 
// Creation Date : 2/4/2022 
//-----------------------------------------------------------------------------
bool ROL::setROLOptions( const Util::OptionBlock & option_block)
{ 
  for (Util::ParamList::const_iterator it = option_block.begin(), end = option_block.end(); it != end; ++it)
  { 
    ExtendedString tag = it->tag();
    tag.toUpper();

    if ( tag == "PARAM_FILENAME" )
    {
      ExtendedString stringVal ( it->stringValue() );
      paramFile_ = stringVal;
    }
    else if ( tag == "ROL_FILENAME" )
    {
      ExtendedString stringVal ( it->stringValue() );
      rolParamFile_ = stringVal;
    }
    else
    { 
      Report::UserError0() << tag << " is not a recognized ROL option.";
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setOptParams
// Purpose       :
// Special Notes : These are from a file
// Scope         : public
// Creator       :
// Creation Date : 2/4/2022
//-----------------------------------------------------------------------------
bool ROL::setOptParams()
{
  // Read in z's param names, initial guess and upper and lower bounds.
  std::ifstream param_file;
  std::string deviceName,paramName,dummy;
  RealT temp;
  numParams_ = 0;
  paramNameVec_.clear();
  param_file.open(paramFile_.c_str(), std::ifstream::in);
  if ( param_file.fail() )
  {
    Report::UserError0() << "Cannot open the ROL parameter file: " << paramFile_;
    return false;
  }
  else
  {
    getline(param_file,dummy); // skip the first line
    while (true)
    {
      if(!(param_file >> deviceName)) break;
      if(!(param_file >> paramName)) break;
      paramName = deviceName+":"+paramName;
      paramNameVec_.push_back(paramName);
      if(!(param_file >> temp)) break; // init guess
      zInitValue_.push_back(temp);
      if(!(param_file >> temp)) break; // lo bound
      zLowerBoundVector_.push_back(temp);
      if(!(param_file >> temp)) break; // up bound
      zUpperBoundVector_.push_back(temp);

      numParams_ += 1;
    }
    param_file.close();
  }

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::run()
// Purpose       : This is the main controlling loop for ROL analysis.
//
// Special Notes :
// Scope         : public
// Creator       : Eric Keiter, SNL
// Creation Date : 10/04/00
//-----------------------------------------------------------------------------
bool ROL::doRun()
{
  // The doLoopProcess() will execute the ROL optimization algorithm on necessary
  // subordinate analysis objects, like AC / DC / Transient.
  return doInit() && doLoopProcess() && doFinish();
}

//-----------------------------------------------------------------------------
// Function      : ROL::init()
// Purpose       :
// Special Notes :
// Scope         : public
// Creator       : 
// Creation Date : 06/02/2015
//-----------------------------------------------------------------------------
bool ROL::doInit()
{
  bool status;

  if (sensFlag_)
  {
    std::cout << "Sens flag has been enabled!" << std::endl;
    Stats::StatTop _sensitivityStat("Sensitivity");

    nonlinearManager_.enableSensitivity(
        *analysisManager_.getDataStore(),
        analysisManager_.getStepErrorControl(),
        *analysisManager_.getPDSManager(),
        topology_,
        outputManagerAdapter_.getOutputManager(),
        numSensParams_);
  }

  if (DEBUG_ANALYSIS && isActive(Diag::TIME_PARAMETERS))
  {
    Xyce::dout() << std::endl << std::endl;
    Xyce::dout() << section_divider << std::endl;
    Xyce::dout() << "ROL Sweep::init" << std::endl;
  }

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::getDCOPFlag
// Purpose       :
// Special Notes :
// Scope         : public
// Creator       : Eric Keiter
// Creation Date : 3/24/2014
//-----------------------------------------------------------------------------
bool ROL::getDCOPFlag() const
{
  if (currentAnalysisObject_)
    return currentAnalysisObject_->getDCOPFlag();
  else
    return false;
}

//-----------------------------------------------------------------------------
// Function      : ROL::twoLevelStep
//
// Purpose       : Used by 2-level Newton solves to execute a single DC sweep
//                 step.
//
// Special Notes : This is mostly what happens on the inner loop of
//                 dcSweepLoop, except that DC parameters are not updated,
//                 and success/failure of the step is not determined.
//
// Scope         : public
// Creator       : Eric Keiter, SNL
// Creation Date : 03/10/06
//-----------------------------------------------------------------------------
bool ROL::twoLevelStep()
{
  if (currentAnalysisObject_)
    return currentAnalysisObject_->twoLevelStep();
  else
    return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::doLoopProcess
// Purpose       : Where the ROL analysis is performed
// Special Notes :
// Scope         : public
// Creator       : Timur Takhtaganov
// Creation Date : 06/02/2015
//-----------------------------------------------------------------------------
bool ROL::doLoopProcess()
{
  std::string msg;
  bool status = true;
  int errorFlag = 0;

#ifdef Xyce_ROL
  try
  { 
    // Create ROL_DC analysis object.
    ROL_DC dc_sweep(analysisManager_, nonlinearManager_, loader_, linearSystem_, topology_, initialConditionsManager_);
    currentAnalysisObject_ = &dc_sweep;
    analysisManager_.pushActiveAnalysis(&dc_sweep);

    dc_sweep.setAnalysisParams(saved_sweepOB_);
    dc_sweep.setTimeIntegratorOptions(saved_timeIntOB_);
    for (std::vector<Util::OptionBlock>::const_iterator it = saved_dataOB_.begin(), end = saved_dataOB_.end(); it != end; ++it)
    {
      dc_sweep.setDataStatements(*it);
    }
    dc_sweep.doInit();
    stepLoopSize_ = dc_sweep.getLoopSize();

    // Number of simulation variables
    int nu = analysisManager_.getDataStore()->nextSolutionPtr->globalLength();
    int nc = stepLoopSize_;  // Number of constraints.

    RealT tol = 1.e-12;
  
    // STEP 1A: Initialize vectors.  //////////////////////////////////////////

    // Set the parameter bounds.
    setOptParams();

    // Configure the optimization vector.
    int nz = numParams_;     // Number of optimization variables                                           
    auto p = ::ROL::makePtr<  std::vector   <RealT>>(zInitValue_);
    auto z = ::ROL::makePtr<::ROL::StdVector<RealT>>(p);
    // p := pointer to a standard vector representing z

    // Configure the state and constraint vectors.
    status = dc_sweep.doAllocations(nc, nz);
    auto u = ::ROL::makePtr<Linear::ROL_XyceVector<RealT>>(dc_sweep.statePtrVector_);
    auto l = ::ROL::makePtr<Linear::ROL_XyceVector<RealT>>(dc_sweep.constraintPtrVector_);

    // STEP 1B: Initialize parameters.  ///////////////////////////////////////

    // Load parameters.
    ::ROL::Ptr<::ROL::ParameterList> parlist 
      = ::ROL::getParametersFromXmlFile(rolParamFile_);
    std::string outName     = parlist->get("Output File", outputFile_);
    bool useBoundConstraint = parlist->get("Use Bound Constraints", true);
    bool useScale           = parlist->get("Use Scaling For Epsilon-Active Sets", true);
    // TODO (asjavee): Remove hardwired code. Eventually we shouldn't need the 
    //                 subsequent lines in this code block.
    // bool doChecks           = parlist->get("Do Checks",true);
    // bool useSQP             = parlist->get("Use SQP", true);
    // bool useLineSearch      = parlist->get("Use Line Search", true);    
    // bool useTrustRegion     = parlist->get("Use Trust Region", true);
    // bool useBundleMethod    = parlist->get("Use Bundle Method", true);
    // Parameters for the amplifier problem. 
    int ptype               = parlist->get("Penalty Type", 1);
    RealT alpha             = parlist->get("Penalty Parameter", 1.0e-4);
    RealT ampl              = parlist->get("Amplifier Gain", 4.0);

    // Configure the output stream.
    std::ofstream out(outName.c_str());
    // TODO (asjavee): I don't think we need the line below; ofstream inherits 
    //                 from ostream.
    // Teuchos::RCP<std::ostream> outStream = Teuchos::rcp(&out,false);

    // STEP 2: Create the SimOpt equality constraint.  ////////////////////////

    auto con = ::ROL::makePtr<EqualityConstraint_ROL_DC<RealT>>
               (
                 nu, nc, nz, 
                 analysisManager_, 
                 nonlinearManager_.getNonlinearSolver(),
                 linearSystem_,
                 paramNameVec_,
                 dc_sweep
               );
    // Here, con is the constraint c(u, z) = 0. Its solve member function 
    // specifies u as a function of z (u = S(z)); see slide 26 of
    //   <https://trilinos.github.io/pdfs/ROL.pdf>.

    con->solve(*l, *u, *z, tol);

    // STEP 3: Build our objective.  /////////////////////////////////////////

    dc_sweep.createObjectives( rolDCObjVec_ );
    
    // Get the objective from dc_sweep object
    Teuchos::RCP<::ROL::Objective_SimOpt<RealT> > obj, pen;
    obj = dc_sweep.obj_;

    // Create a reduced objective (and penalty) from the full space counterpart.
    auto robj = ::ROL::makePtr<::ROL::Reduced_Objective_SimOpt<RealT>>(obj, con, u, z, l, false);

    // TODO (asjavee): Once ROL is modified, remove "false" in the 
    //   initialization of robj above.

    // // Create the (possibly penalized) objective that we wish to optimize.
    // std::vector<Teuchos::RCP<::ROL::Objective<RealT>>> objVec(1, robj);
    // if (objType_ == 1)
    // {
    //   pen = ::ROL::makePtr<Penalty_DC_AMP  <RealT>>(ptype, alpha, ampl, nc, nz);
    //   auto rpen = ::ROL::makePtr<::ROL::Reduced_Objective_SimOpt<RealT>>(pen, con, u, z, l);
    //   objVec.push_back(rpen);
    // }
    // std::vector<bool> types(objVec.size(), true);
    // auto pobj = ::ROL::makePtr<SumObjective<RealT>>(objVec, types);

    // STEP 4: Create z's BoundConstraint.  ///////////////////////////////////
    
    RealT scale = 1.0;
    if(useScale)
    {
      // Evaluate the reduced objective gradient at the initial value of z.
      auto g0P = ::ROL::makePtr<  std::vector   <RealT>>(nz, 0.0);
      auto g0  = ::ROL::makePtr<::ROL::StdVector<RealT>>(g0P);
      robj->gradient(*g0, *z, tol);

      // std::cout << std::scientific << "Norm of initial gradient = " << g0->norm() << "\n";
    
      scale = 1.0e-2/(g0->norm());
    }

    // std::cout << std::scientific << "Scaling: " << scale << "\n";
    
    auto bnd = ::ROL::makePtr<BoundConstraint_ROL_DC<RealT>>(scale, zLowerBoundVector_, zUpperBoundVector_);
    
    // STEP 5A: Define the optimization problem.  /////////////////////////////

    auto problem = ::ROL::makePtr<::ROL::Problem<RealT>>(robj, z);
    if (useBoundConstraint)
      problem->addBoundConstraint(bnd);
    // problem->check(true);

    // STEP 5B: Define the optimization solver.  //////////////////////////////
    
    parlist->sublist("General").set("Output Level", 1);    
    parlist->sublist("Status Test").set("Gradient Tolerance", 1.e-10);
    parlist->sublist("Status Test").set("Step Tolerance", 1.e-30); 
 
    ::ROL::Solver<RealT> solver(problem, *parlist);

    // STEP 6: Solve.  ////////////////////////////////////////////////////////
    
    // Print initial guess.
    out << "\nInitial guess " << std::endl;
    for (int i = 0; i < nz; i++)
      out << paramNameVec_[i] << " = " << (*p)[i] << std::endl;

    // TODO (asjavee): Manage the default below by setting them in parlist.
    // int maxit  = parlist->get("Maximum Number of Iterations", 100);
    
    std::clock_t timer_bm = std::clock();
   
    solver.solve(std::cout);
    con->solve(*l, *u, *z, tol);

    // Print results.
    out << "\nSolution " << std::endl;
    for (int i = 0; i < nz; i++)
    {
      out << paramNameVec_[i] << " = " << (*p)[i] << std::endl;
    }
    out << "Solve required " << (std::clock()-timer_bm)/(RealT)CLOCKS_PER_SEC
         << " seconds.\n";


    // Remove DC analysis object from active analysis stack.
    stats_ += currentAnalysisObject_->stats_;
    analysisManager_.popActiveAnalysis();
    currentAnalysisObject_ = 0;
  } 
  catch (std::logic_error err) 
  {
    //out << err.what() << "\n";
    errorFlag = -1000;
  }; // end try       
  
#else

  Report::UserError0() << "ROL was not enabled in the Xyce build";

#endif
  
  return status;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setROLDCSweep
// Purpose       : this is needed for ROL
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter
// Creation Date : 7/12/2013
//-----------------------------------------------------------------------------
bool ROL::setROLDCSweep(const std::vector<Util::OptionBlock>& OB)
{
  saved_sweepOB_ = OB;

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setROLDataOptionBlock
// Purpose       : this is needed for ROL
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter
// Creation Date : 7/12/2013
//-----------------------------------------------------------------------------
bool ROL::setROLDataOptionBlock(const std::vector<Util::OptionBlock>& OB)
{
  saved_dataOB_ = OB;

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setROLObjectives
// Purpose       : this is needed for ROL
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter
// Creation Date : 7/12/2013
//-----------------------------------------------------------------------------
bool ROL::setROLObjectives(const std::vector<Util::OptionBlock>& OB)
{
  saved_rolObjOB_ = OB;

  if (OB.size() == 0)
    Report::UserError0() << "No objectives have been defined for optimization, please include .ROL_OBJ lines to define.";

  std::vector<Util::OptionBlock>::const_iterator it_OB = OB.begin(), end_OB = OB.end();

  for ( ; it_OB != end_OB; ++it_OB )
  {
    ROL_Objective new_obj;
    ExtendedString analysisType( "" );

    Util::ParamList::const_iterator it = (*it_OB).begin(), it_end = (*it_OB).end(); 
    for ( ; it != it_end; ++it )
    {
      ExtendedString tag = it->tag();
      tag.toUpper();

      if ( tag == "ANALYSIS" )
      {
        analysisType = ExtendedString( it->stringValue() );
      }   
      else if ( tag == "OBJ_TYPE" )
      {
        new_obj.objType_ = ExtendedString( it->stringValue() );
      }
      else if ( tag == "OBJ_TAG" )
      {
        new_obj.objTag_ = ExtendedString( it->stringValue() );
      }
      else if ( tag == "OBJ_ARGS" )
      {
        new_obj.objArgs_ = it->getValue<std::vector<std::string> >();
      }
      else
      { 
        Report::UserError0() << tag << " is not a recognized ROL option.";
      }
    }

    // Associate objective with respective analysis
    if (analysisType == "DC")
      rolDCObjVec_.push_back( new_obj );
    else
      Report::UserError0() << "ROL does not recognize objectives for " << analysisType << " analysis.";
  }

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setLinSol
// Purpose       : this is needed for ROL
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter
// Creation Date : 7/12/2013
//-----------------------------------------------------------------------------
bool ROL::setLinSol(const Util::OptionBlock & OB)
{
  // Save the linear solver option block
  saved_lsOB_ = OB;

  return true;
}

//-----------------------------------------------------------------------------
// Function      : ROL::setTimeInt
// Purpose       : this is needed for ROL
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter
// Creation Date : 7/12/2013
//-----------------------------------------------------------------------------
bool ROL::setTimeInt(const Util::OptionBlock & OB)
{
  // Save the time integrator option block
  saved_timeIntOB_ = OB;

  return true;
}

namespace {

//-----------------------------------------------------------------------------
// Class         : ROLFactory
// Purpose       :
// Special Notes :
// Scope         : public
// Creator       : David G. Baur  Raytheon  Sandia National Laboratories 1355
// Creation Date : Thu Jan 29 12:53:02 2015
//-----------------------------------------------------------------------------
///
/// Factory for parsing ROL parameters from the netlist and creating ROL analysis.
///
class ROLFactory : public Util::Factory<AnalysisBase, ROL>
{
public:
  //-----------------------------------------------------------------------------
  // Function      : ROLFactory
  // Purpose       :
  // Special Notes :
  // Scope         : public
  // Creator       : David G. Baur  Raytheon  Sandia National Laboratories 1355
  // Creation Date : Thu Jan 29 12:54:09 2015
  //-----------------------------------------------------------------------------
  ///
  /// Constructs the ROL analysis factory
  ///
  /// @invariant Stores the results of parsing.  Multiple ROL analysis options may be
  /// applied and each generates and additional step.
  ///
  /// @invariant The existence of the parameters specified in the constructor cannot
  /// change.
  ///
  /// @param analysis_manager 
  /// @param linear_system 
  /// @param nonlinear_manager 
  ///
  ROLFactory(
     Analysis::AnalysisManager &         analysis_manager,
     Linear::System &                    linear_system,
     Nonlinear::Manager &                nonlinear_manager,
     Loader::Loader &                    loader,
     Topo::Topology &                    topology,
     IO::InitialConditionsManager &      initial_conditions_manager)
    : Util::Factory<AnalysisBase, ROL>(),
    analysisManager_(analysis_manager),
    linearSystem_(linear_system),
    nonlinearManager_(nonlinear_manager),
    loader_(loader),
    topology_(topology),
    initialConditionsManager_(initial_conditions_manager)
  {}

  virtual ~ROLFactory()
  {}

  //-----------------------------------------------------------------------------
  // Function      : create
  // Purpose       :
  // Special Notes :
  // Scope         : public
  // Creator       : David G. Baur  Raytheon  Sandia National Laboratories 1355
  // Creation Date : Thu Jan 29 12:59:00 2015
  //-----------------------------------------------------------------------------
  ///
  /// Create a new ROL analysis and applies the analysis and time integrator option blocks.
  ///
  /// @return new ROL analysis object
  ///
  ROL *create() const
  {
    ROL *rol = new ROL(analysisManager_, nonlinearManager_, loader_, linearSystem_, topology_, initialConditionsManager_);

    rol->setTimeInt(timeIntegratorOptionBlock_);
    rol->setLinSol(linSolOptionBlock_);

    rol->setROLOptions(rolOptionBlock_);
    rol->setROLDCSweep(rolDCSweepBlock_);
    rol->setROLObjectives(rolObjBlock_);
    rol->setROLDataOptionBlock(dataOptionBlockVec_);

    return rol;
  }

  bool setROLDCBlock(const Util::OptionBlock &option_block)
  {
    bool found = false; 
    std::vector<Util::OptionBlock>::iterator it = rolDCSweepBlock_.begin();
    std::vector<Util::OptionBlock>::iterator end = rolDCSweepBlock_.end();
    for ( ; it != end; ++it )
    { 
      if (Util::compareParamLists(option_block, *it))
        found = true;
    }
    
    // save the new one.
    if (!found)
      rolDCSweepBlock_.push_back( option_block );
    
    return true;
  }

  bool setROLObjBlock(const Util::OptionBlock &option_block)
  {
    bool found = false; 
    std::vector<Util::OptionBlock>::iterator it = rolObjBlock_.begin();
    std::vector<Util::OptionBlock>::iterator end = rolObjBlock_.end();
    for ( ; it != end; ++it )
    { 
      if (Util::compareParamLists(option_block, *it))
        found = true;
    }
    
    // save the new one.
    if (!found)
      rolObjBlock_.push_back( option_block );
    
    return true;
  }

  bool setTimeIntegratorOptionBlock(const Util::OptionBlock &option_block)
  {
    timeIntegratorOptionBlock_ = option_block;
    return true;
  }
  
  bool setLinSolOptionBlock(const Util::OptionBlock &option_block)
  {
    linSolOptionBlock_ = option_block;
    return true;
  }

  bool setROLOptionBlock(const Util::OptionBlock &option_block)
  {
    rolOptionBlock_ = option_block;
    return true;
  }

  bool setDotDataBlock(const Util::OptionBlock &option_block)
  {
    dataOptionBlockVec_.push_back(option_block);
    return true;
  }
 
public:
  AnalysisManager &                     analysisManager_;
  Linear::System &                      linearSystem_;
  Nonlinear::Manager &                  nonlinearManager_;
  Loader::Loader &                      loader_;
  Topo::Topology &                      topology_;
  IO::InitialConditionsManager &        initialConditionsManager_;

private:
  std::vector<Util::OptionBlock>        stepSweepAnalysisOptionBlock_;
  std::vector<Util::OptionBlock>        rolDCSweepBlock_;
  std::vector<Util::OptionBlock>        rolObjBlock_;
  std::vector<Util::OptionBlock>        dataOptionBlockVec_;
  Util::OptionBlock                     timeIntegratorOptionBlock_; 
  Util::OptionBlock                     linSolOptionBlock_;
  Util::OptionBlock                     rolOptionBlock_; 
};
  
// .ROL
struct ROLAnalysisReg : public IO::PkgOptionsReg
{
  ROLAnalysisReg(
     ROLFactory &             factory)
    : factory_(factory)
  {}

  bool operator()(const Util::OptionBlock &option_block)
  {
    factory_.setROLOptionBlock(option_block);

    factory_.analysisManager_.addAnalysis(&factory_);

    return true;
  }

  ROLFactory &               factory_;
};

//-----------------------------------------------------------------------------
// Function      : extractROLData
// Purpose       : Extract the parameters from a netlist .ROL line held in
//                 parsedLine.
// Special Notes :
// Scope         : public
// Creator       : Eric R. Keiter, SNL
// Creation Date : 10/30/2003
//-----------------------------------------------------------------------------
bool extractROLData(
   IO::PkgOptionsMgr &           options_manager,
   IO::CircuitBlock &            circuit_block,
   const std::string &           netlist_filename,
   const IO::TokenVector &       parsed_line)
{
  // length of the original .ROL line
  int numFields = parsed_line.size();

  // start of parameters (skip over the ".ROL")
  int linePosition = 1;

  Util::OptionBlock option_block("ROL", Util::OptionBlock::ALLOW_EXPRESSIONS, netlist_filename, parsed_line[linePosition].lineNumber_);

  while( linePosition < numFields )
  {
    std::string curr = parsed_line[linePosition].string_;
    Util::toUpper(curr);

    if (curr == "PARAM_FILENAME")
    {
      if (parsed_line[linePosition + 1].string_ == "=")
        linePosition ++;
      option_block.addParam(Util::Param(curr, parsed_line[++linePosition].string_));
    }
    else if (curr == "ROL_FILENAME")
    {
      if (parsed_line[linePosition + 1].string_ == "=")
        linePosition ++;
      option_block.addParam(Util::Param(curr, parsed_line[++linePosition].string_));
    }
    else
    {
      Report::UserError0().at(netlist_filename, parsed_line[linePosition].lineNumber_) << "Unrecognized value '" << curr << "' on .ROL line";
      break;
    }

    linePosition++;
  } 

  circuit_block.addOptions(option_block);

  return true;
}

//-----------------------------------------------------------------------------
// Function      : extractROLDCData
// Purpose       : Extract the parameters from a netlist .ROL_DC line held in
//                 parsedLine.
// Special Notes : This calls the extractDCData method to avoid duplication of
//                 line parsing for .DC lines
// Scope         : public
// Creator       : Eric R. Keiter, SNL
// Creation Date : 10/30/2003
//-----------------------------------------------------------------------------
bool extractROLDCData(
   IO::PkgOptionsMgr &           options_manager,
   IO::CircuitBlock &            circuit_block,
   const std::string &           netlist_filename,
   const IO::TokenVector &       parsed_line)
{
  std::vector<Util::OptionBlock> option_block_vec = 
    Xyce::Analysis::extractDCDataInternals("ROL_DC", options_manager, netlist_filename, parsed_line);

  if (option_block_vec.size())
  {
    std::vector<Util::OptionBlock>::iterator it = option_block_vec.begin();
    std::vector<Util::OptionBlock>::const_iterator it_end = option_block_vec.end();
    for ( ; it != it_end; ++it )
      circuit_block.addOptions( *it );
  }
  else
   return false;

  return true;
}

//-----------------------------------------------------------------------------
// Function      : extractROLObjData
// Purpose       : Extract the objective parameters from a .ROL_OBJ line held in
//                 parsedLine.
// Special Notes : This calls the extractObjData method 
// Scope         : public
// Creator       : Eric R. Keiter, SNL
// Creation Date : 10/30/2003
//-----------------------------------------------------------------------------
bool extractROLObjData(
   IO::PkgOptionsMgr &           options_manager,
   IO::CircuitBlock &            circuit_block,
   const std::string &           netlist_filename,
   const IO::TokenVector &       parsed_line)
{
  std::vector< std::string > validAnalysisTypes = {"DC", "AC", "TRAN"};

  // length of the original .ROL line
  int numFields = parsed_line.size();

  // start of parameters (skip over the ".ROL_OBJ")
  int linePosition = 1;

  Util::OptionBlock option_block("ROL_OBJ", Util::OptionBlock::ALLOW_EXPRESSIONS, netlist_filename, parsed_line[linePosition].lineNumber_);

  // collect analysis type for objective
  std::string curr = parsed_line[linePosition].string_;
  Util::toUpper(curr);
  if (std::find(validAnalysisTypes.begin(), validAnalysisTypes.end(), curr) != validAnalysisTypes.end())
    option_block.addParam(Util::Param("ANALYSIS", curr )); 
  else  
    Report::UserError0().at(netlist_filename, parsed_line[linePosition].lineNumber_) << "Unrecognized analysis type '" << curr << "' on .ROL_OBJ line";

  // Get OBJ_TAG
  curr = parsed_line[++linePosition].string_;
  Util::toUpper(curr);
  option_block.addParam(Util::Param("OBJ_TAG", curr));

  // Get OBJ_TYPE
  curr = parsed_line[++linePosition].string_;
  Util::toUpper(curr);
  option_block.addParam(Util::Param("OBJ_TYPE", curr));

  std::vector< std::string > argVec;

  linePosition++;

  while( linePosition < numFields )
  {
    curr = parsed_line[linePosition++].string_;
    Util::toUpper(curr);
    argVec.push_back( curr );
  }
  option_block.addParam(Util::Param("OBJ_ARGS", argVec)); 

  circuit_block.addOptions(option_block);

  return true;
}

} // namespace <unnamed>

bool registerROLFactory(
   FactoryBlock &        factory_block)
{
  ROLFactory *factory = new ROLFactory(factory_block.analysisManager_, factory_block.linearSystem_, factory_block.nonlinearManager_, factory_block.loader_, factory_block.topology_, factory_block.initialConditionsManager_);

  addAnalysisFactory(factory_block, factory);

  factory_block.optionsManager_.addCommandParser(".ROL", extractROLData);

  factory_block.optionsManager_.addCommandProcessor("ROL", new ROLAnalysisReg(*factory));

  factory_block.optionsManager_.addCommandParser(".ROL_DC", extractROLDCData);

  factory_block.optionsManager_.addCommandProcessor("ROL_DC", IO::createRegistrationOptions(*factory, &ROLFactory::setROLDCBlock));

  factory_block.optionsManager_.addCommandParser(".ROL_OBJ", extractROLObjData);

  factory_block.optionsManager_.addCommandProcessor("ROL_OBJ", IO::createRegistrationOptions(*factory, &ROLFactory::setROLObjBlock));

  factory_block.optionsManager_.addOptionsProcessor("TIMEINT", IO::createRegistrationOptions(*factory, &ROLFactory::setTimeIntegratorOptionBlock));

  factory_block.optionsManager_.addOptionsProcessor("LINSOL", IO::createRegistrationOptions(*factory, &ROLFactory::setLinSolOptionBlock));

  factory_block.optionsManager_.addCommandProcessor("DATA", IO::createRegistrationOptions(*factory, &ROLFactory::setDotDataBlock) );

  return true;
}

} // namespace Analysis
} // namespace Xyce
 
