/// \file TClingUtilsTests.cxx
///
/// \brief The file contain unit tests which test the TClingUtils.h
///
/// \author Vassil Vassilev <vvasilev@cern.ch>
///
/// \date Aug, 2019
///
/*************************************************************************
 * Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <TClingUtils.h>
#include <TClass.h>
#include <TInterpreter.h>

#include <ROOT/FoundationUtils.hxx>

#include <fstream>
#include <deque>

int main() {
   auto x = TClass::GetClass("std::deque<int>");
   return 0;
}
