// @(#)root/minuit2:$Id$
// Authors: M. Winkler, F. James, L. Moneta, A. Zsenei   2003-2005

/**********************************************************************
 *                                                                    *
 * Copyright (c) 2005 LCG ROOT Math team,  CERN/PH-SFT                *
 *                                                                    *
 **********************************************************************/

#ifndef MN_FlatRandomGen_H_
#define MN_FlatRandomGen_H_

#include <cmath>

namespace ROOT {

namespace Minuit2 {

class FlatRandomGen {

public:
   FlatRandomGen() = default;

   FlatRandomGen(double mean, double delta) : fMean(mean), fDelta(delta) {}

   double Mean() const { return fMean; }

   double Delta() const { return fDelta; }

   double operator()() const { return 2. * Delta() * (std::rand() / double(RAND_MAX) - 0.5) + Mean(); }

private:
   double fMean = 0.5;
   double fDelta = 0.5;
};

} // namespace Minuit2

} // namespace ROOT

#endif // MN_FlatRandomGen_H_
