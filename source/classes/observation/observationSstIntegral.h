/***********************************************/
/**
* @file observationSstIntegral.h
*
* @brief Satellite to satellite tracking (Short Arc Integral).
*
* @author Torsten Mayer-Guerr
* @date 2009-11-01
*
*/
/***********************************************/

#ifndef __GROOPS_OBSERVATIONSSTINTEGRAL__
#define __GROOPS_OBSERVATIONSSTINTEGRAL__

// Latex documentation
#ifdef DOCSTRING_Observation
static const char *docstringObservationSstIntegral = R"(
\subsection{SstIntegral}\label{observationType:sstIntegral}
Like \configClass{observation:podIntegral}{observationType:podIntegral} (see there for details)
but with two satellites and additional satellite-to-satellite (SST) observations.

If multiple \configFile{inputfileSatelliteTracking}{instrument} are given
all data are add together. So corrections in extra files like the light time correction
can easily be added. Empirical parameters for the SST observations can be setup with
\configClass{parametrizationSst}{parametrizationSatelliteTrackingType}.
The accuracy or the full covariance matrix of SST is provided in
\configClass{covarianceSst}{covarianceSstType}.
)";
#endif

/***********************************************/

#include "misc/observation/observationMiscSstIntegral.h"
#include "misc/observation/covariancePod.h"
#include "misc/observation/covarianceSst.h"

/***** CLASS ***********************************/

/** @brief Satellite to satellite tracking (Short Arc Integral).
* @ingroup observationGroup
* @see Observation */
class ObservationSstIntegral : public Observation
{
  ObservationMiscSstIntegralPtr observationMisc;
  CovarianceSstPtr covSst;
  CovariancePodPtr covPod1, covPod2;

public:
  ObservationSstIntegral(Config &config);
 ~ObservationSstIntegral() {}

  UInt parameterCount()          const {return observationMisc->parameterCount();}
  UInt gravityParameterCount()   const {return observationMisc->gravityParameterCount();}
  UInt rightSideCount()          const {return observationMisc->rightSideCount();}
  UInt arcCount()                const {return observationMisc->arcCount();}
  void parameterName(std::vector<ParameterName> &name) const {observationMisc->parameterName(name);}

  void observation(UInt arc, Matrix &l, Matrix &A, Matrix &B);
};

/***********************************************/

#endif /* __GROOPS_OBSERVATION__ */
