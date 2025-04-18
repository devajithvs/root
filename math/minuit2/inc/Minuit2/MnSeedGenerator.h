// @(#)root/minuit2:$Id$
// Authors: M. Winkler, F. James, L. Moneta, A. Zsenei   2003-2005

/**********************************************************************
 *                                                                    *
 * Copyright (c) 2005 LCG ROOT Math team,  CERN/PH-SFT                *
 *                                                                    *
 **********************************************************************/

#ifndef ROOT_Minuit2_MnSeedGenerator
#define ROOT_Minuit2_MnSeedGenerator

#include "Minuit2/MinimumSeedGenerator.h"
#include "Minuit2/AnalyticalGradientCalculator.h"

namespace ROOT {

namespace Minuit2 {

/** concrete implementation of the MinimumSeedGenerator interface; used within
    ModularFunctionMinimizer;
 */

class MnSeedGenerator : public MinimumSeedGenerator {
public:
   MinimumSeed operator()(const MnFcn &, const GradientCalculator &, const MnUserParameterState &,
                          const MnStrategy &) const override;

private:
   MinimumSeed CallWithAnalyticalGradientCalculator(const MnFcn &, const AnalyticalGradientCalculator &,
                                                    const MnUserParameterState &, const MnStrategy &) const;
};

} // namespace Minuit2

} // namespace ROOT

#endif // ROOT_Minuit2_MnSeedGenerator
