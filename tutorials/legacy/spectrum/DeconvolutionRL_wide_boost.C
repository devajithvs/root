/// \file
/// \ingroup tutorial_spectrum
/// Example to illustrate deconvolution function (class TSpectrum).
///
/// \macro_image
/// \macro_code
///
/// \authors Miroslav Morhac, Olivier Couet

void DeconvolutionRL_wide_boost()
{
   Int_t i;
   const Int_t nbins = 256;
   Double_t xmin = 0;
   Double_t xmax = nbins;
   Double_t source[nbins];
   Double_t response[nbins];
   gROOT->ForceStyle();

   TString dir = gROOT->GetTutorialDir();
   TString file = dir + "/legacy/spectrum/TSpectrum.root";
   TFile *f = new TFile(file.Data());
   TH1F *h = (TH1F *)f->Get("decon3");
   h->SetTitle(
      "Deconvolution of closely positioned overlapping peaks using boosted Richardson-Lucy deconvolution method");
   TH1F *d = (TH1F *)f->Get("decon_response_wide");

   for (i = 0; i < nbins; i++)
      source[i] = h->GetBinContent(i + 1);
   for (i = 0; i < nbins; i++)
      response[i] = d->GetBinContent(i + 1);

   h->Draw("L");
   TSpectrum *s = new TSpectrum();
   s->DeconvolutionRL(source, response, 256, 200, 50, 1.2);

   for (i = 0; i < nbins; i++)
      d->SetBinContent(i + 1, source[i]);
   d->SetLineColor(kRed);
   d->Draw("SAME L");
}
