// Author: Sergey Linev <s.linev@gsi.de>
// Date: 2018-10-24
// Warning: This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback is welcome!

/*************************************************************************
 * Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RWebDisplayArgs.hxx>

#include <ROOT/RConfig.hxx>
#include <ROOT/RLogger.hxx>
#include <ROOT/RWebWindow.hxx>

#include "TROOT.h"
#include <string>

using namespace ROOT;

ROOT::RLogChannel &ROOT::WebGUILog()
{
   static ROOT::RLogChannel sLog("ROOT.WebGUI");
   return sLog;
}


/** \class ROOT::RWebDisplayArgs
\ingroup webdisplay

Holds different arguments for starting browser with RWebDisplayHandle::Display() method

*/

///////////////////////////////////////////////////////////////////////////////////////////
/// Default constructor.
/// Browser kind configured from gROOT->GetWebDisplay()

RWebDisplayArgs::RWebDisplayArgs()
{
   SetBrowserKind("");
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Constructor.
/// Browser kind specified as std::string.
/// See \ref SetBrowserKind method for description of allowed parameters

RWebDisplayArgs::RWebDisplayArgs(const std::string &browser)
{
   SetBrowserKind(browser);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Constructor.
/// Browser kind specified as `const char *`.
/// See \ref SetBrowserKind method for description of allowed parameters

RWebDisplayArgs::RWebDisplayArgs(const char *browser)
{
   SetBrowserKind(browser);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Constructor.
/// Let specify window width and height

RWebDisplayArgs::RWebDisplayArgs(int width, int height, int x, int y, const std::string &browser)
{
   SetSize(width, height);
   SetPos(x, y);
   SetBrowserKind(browser);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Constructor.
/// Let specify master window and channel (if reserved already)

RWebDisplayArgs::RWebDisplayArgs(std::shared_ptr<RWebWindow> master, unsigned conndid, int channel)
{
   SetMasterWindow(master, conndid, channel);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Destructor.
/// Must be defined in source code to correctly call RWebWindow destructor

RWebDisplayArgs::~RWebDisplayArgs() = default;

///////////////////////////////////////////////////////////////////////////////////////////
/// Set size of web browser window as string like "800x600"

bool RWebDisplayArgs::SetSizeAsStr(const std::string &str)
{
   auto separ = str.find("x");
   if ((separ == std::string::npos) || (separ == 0) || (separ == str.length()-1)) return false;

   int width = 0, height = 0;

   try {
      width = std::stoi(str.substr(0,separ));
      height = std::stoi(str.substr(separ+1));
   } catch(...) {
       return false;
   }

   if ((width<=0) || (height<=0))
      return false;

   SetSize(width, height);
   return true;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Set position of web browser window as string like "100,100"

bool RWebDisplayArgs::SetPosAsStr(const std::string &str)
{
   auto separ = str.find(",");
   if ((separ == std::string::npos) || (separ == 0) || (separ == str.length()-1)) return false;

   int x = 0, y = 0;

   try {
      x = std::stoi(str.substr(0,separ));
      y = std::stoi(str.substr(separ+1));
   } catch(...) {
      return false;
   }

   if ((x < 0) || (y < 0))
      return false;

   SetPos(x, y);
   return true;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Set browser kind as string argument.
///
/// Recognized values:
///
///      chrome - use Google Chrome web browser
///     firefox - use Mozilla Firefox web browser
///        edge - use Microsoft Edge web browser (Windows only)
///      native - either chrome/edge or firefox, only these browsers support batch (headless) mode
///     default - default system web-browser, no batch mode
///         cef - Chromium Embeded Framework, local display, local communication
///         qt6 - Qt6 QWebEngineCore, local display, local communication
///       local - either cef or qt6
///         off - disable web display
///          on - first try "local", then "native", then "default" (default option)
///    `<prog>` - any program name which will be started to open widget URL, like "/usr/bin/opera"

RWebDisplayArgs &RWebDisplayArgs::SetBrowserKind(const std::string &_kind)
{
   std::string kind = _kind;

   auto pos = kind.find("?");
   if (pos == 0) {
      SetUrlOpt(kind.substr(1));
      kind.clear();
   } else if (pos != std::string::npos) {
      SetUrlOpt(kind.substr(pos+1));
      kind.resize(pos);
   }

   pos = kind.find("size:");
   if (pos != std::string::npos) {
      auto epos = kind.find_first_of(" ;", pos+5);
      if (epos == std::string::npos) epos = kind.length();
      SetSizeAsStr(kind.substr(pos+5, epos-pos-5));
      kind.erase(pos, epos-pos);
   }

   pos = kind.find("pos:");
   if (pos != std::string::npos) {
      auto epos = kind.find_first_of(" ;", pos+4);
      if (epos == std::string::npos) epos = kind.length();
      SetPosAsStr(kind.substr(pos+4, epos-pos-4));
      kind.erase(pos, epos-pos);
   }

   pos = kind.rfind("headless");
   if ((pos != std::string::npos) && (pos == kind.length() - 8)) {
      SetHeadless(true);
      kind.resize(pos);
      if ((pos > 0) && (kind[pos-1] == ';')) kind.resize(pos-1);
   }

   // very special handling of qt6 which can specify pointer as a string
   if (kind.find("qt6:") == 0) {
      SetDriverData((void *) std::stoull(kind.substr(4)));
      kind.resize(3);
   }

   // remove all trailing spaces
   while ((kind.length() > 0) && (kind[kind.length()-1] == ' '))
      kind.resize(kind.length()-1);

   // remove any remaining spaces?
   // kind.erase(remove_if(kind.begin(), kind.end(), std::isspace), kind.end());

   if (kind.empty())
      kind = gROOT->GetWebDisplay().Data();

   if (kind == "local")
      SetBrowserKind(kLocal);
   else if (kind == "native")
      SetBrowserKind(kNative);
   else if (kind.empty() || (kind == "on"))
      SetBrowserKind(kOn);
   else if ((kind == "dflt") || (kind == "default") || (kind == "browser"))
      SetBrowserKind(kDefault);
   else if (kind == "firefox")
      SetBrowserKind(kFirefox);
   else if ((kind == "chrome") || (kind == "chromium"))
      SetBrowserKind(kChrome);
#ifdef R__MACOSX
   else if (kind == "safari")
      SetBrowserKind(kSafari);
#endif
#ifdef _MSC_VER
   else if ((kind == "edge") || (kind == "msedge"))
      SetBrowserKind(kEdge);
#endif
   else if ((kind == "cef") || (kind == "cef3"))
      SetBrowserKind(kCEF);
   else if ((kind == "qt") || (kind == "qt6"))
      SetBrowserKind(kQt6);
   else if ((kind == "embed") || (kind == "embedded"))
      SetBrowserKind(kEmbedded);
   else if (kind == "server")
      SetBrowserKind(kServer);
   else if (kind == "off")
      SetBrowserKind(kOff);
   else if (!SetSizeAsStr(kind))
      SetCustomExec(kind);

   return *this;
}

/////////////////////////////////////////////////////////////////////
/// Returns configured browser name

std::string RWebDisplayArgs::GetBrowserName() const
{
   switch (GetBrowserKind()) {
      case kChrome: return "chrome";
      case kEdge: return "edge";
      case kSafari: return "safari";
      case kFirefox: return "firefox";
      case kNative: return "native";
      case kCEF: return "cef";
      case kQt6: return "qt6";
      case kLocal: return "local";
      case kDefault: return "default";
      case kServer: return "server";
      case kEmbedded: return "embed";
      case kOff: return "off";
      case kOn: return "on";
      case kCustom:
          auto pos = fExec.find(" ");
          return (pos == std::string::npos) ? fExec : fExec.substr(0,pos);
   }

   return "";
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Assign window, connection and channel id where other window will be embed

void RWebDisplayArgs::SetMasterWindow(std::shared_ptr<RWebWindow> master, unsigned connid, int channel)
{
   SetBrowserKind(kEmbedded);
   fMaster = master;
   fMasterConnection = connid;
   fMasterChannel = channel;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Append string to url options.
/// Add "&" as separator if any options already exists

void RWebDisplayArgs::AppendUrlOpt(const std::string &opt)
{
   if (opt.empty()) return;

   if (!fUrlOpt.empty())
      fUrlOpt.append("&");

   fUrlOpt.append(opt);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Returns full url, which is combined from URL and extra URL options.
/// Takes into account "#" symbol in url - options are inserted before that symbol

std::string RWebDisplayArgs::GetFullUrl() const
{
   std::string url = GetUrl(), urlopt = GetUrlOpt();
   if (url.empty() || urlopt.empty()) return url;

   auto rpos = url.find("#");
   if (rpos == std::string::npos) rpos = url.length();

   if (url.find("?") != std::string::npos)
      url.insert(rpos, "&");
   else
      url.insert(rpos, "?");
   url.insert(rpos+1, urlopt);

   return url;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Configure custom web browser.
/// Either just name of browser which can be used like "opera"
/// or full execution string which must includes $url like "/usr/bin/opera $url"

void RWebDisplayArgs::SetCustomExec(const std::string &exec)
{
   SetBrowserKind(kCustom);
   fExec = exec;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Returns custom executable to start web browser

std::string RWebDisplayArgs::GetCustomExec() const
{
   if (GetBrowserKind() != kCustom)
      return "";

#ifdef R__MACOSX
   if ((fExec == "safari") || (fExec == "Safari"))
      return "open -a Safari";
#endif

   return fExec;
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Returns string which can be used as argument in RWebWindow::Show() method
/// to display web window in provided Qt6 QWidget.
///
/// Kept for backward compatibility, use GetQtEmbedQualifier instead

std::string RWebDisplayArgs::GetQt5EmbedQualifier(const void *qparent, const std::string &urlopt, unsigned qtversion)
{
   return GetQtEmbedQualifier(qparent, urlopt, qtversion);
}

///////////////////////////////////////////////////////////////////////////////////////////
/// Returns string which can be used as argument in RWebWindow::Show() method
/// to display web window in provided Qt6 QWidget.
///
/// After RWebWindow is displayed created QWebEngineView can be found with the command:
///
///     auto view = qparent->findChild<QWebEngineView*>("RootWebView");

std::string RWebDisplayArgs::GetQtEmbedQualifier(const void *qparent, const std::string &urlopt, unsigned qtversion)
{
   std::string where;
   if (qtversion < 0x60000) {
      R__LOG_ERROR(WebGUILog()) << "GetQtEmbedQualifier no longer support Qt5";
   } else {
      where = "qt6";
      if (qparent) {
         where.append(":");
         where.append(std::to_string((uintptr_t)qparent));
      }
      if (!urlopt.empty()) {
         where.append("?");
         where.append(urlopt);
      }
   }
   return where;
}
