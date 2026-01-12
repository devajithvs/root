{
   gSystem->Load("SecondStaticAssert");
   gROOT->ProcessLine("S<int> h1;");
   gROOT->ProcessLine("h1.Scale(1.0);"); // first failure
   gROOT->ProcessLine("h1.Scale(2.0);"); // second failure (bug: may succeed)
}
