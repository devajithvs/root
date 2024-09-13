// @(#)root/textinput:$Id$
// Author: Axel Naumann <axel@cern.ch>, 2011-05-21

/*************************************************************************
 * Copyright (C) 1995-2011, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "Getline.h"
#include "strlcpy.h"

#include <algorithm>
#include <string>
#include <sstream>
#include "TApplication.h"
#include "TInterpreter.h"

#include "llvm/LineEditor/LineEditor.h"

extern "C" {
   int (* Gl_in_key)(int ch) = nullptr;
   int (* Gl_beep_hook)() = nullptr;
}

namespace {
   class ROOTTabCompletion {
   public:
      // Placeholder
   };

   // Helper to define the lifetime of the LineEditor singleton.
   class LineEditorHolder {
   public:
      LineEditorHolder(const std::string &historyFile) : LE("root", historyFile.c_str()) {}

      ~LineEditorHolder() = default;

      const char* TakeInput(bool force = false) {
         TakeInput(fInputLine, force);
         fInputLine += "\n"; // ROOT wants a trailing newline.
         return fInputLine.c_str();
      }

      void TakeInput(std::string &input, bool force)
      {
         static llvm::LineEditor &LE = getHolder().get(); // Get the LineEditor instance

         // Read a line from the editor
         std::optional<std::string> readLine = LE.readLine();

         if (readLine) {
            // Store the input
            input = *readLine;

            // Remove trailing carriage return characters if present
            while (!input.empty() && input.back() == '\r') {
               input.pop_back();
            }

            // Reset the state or signal that input was taken
            LE.setPrompt(LE.getPrompt()); // Example: resetting the prompt (adapt as necessary)

            // Reset internal states (if applicable) and continue with normal operation
         } else {
            // Handle EOF scenario if no input is retrieved
            input.clear();
            if (force) {
               // If forcing input, prepare the editor for another input cycle
               LE.setPrompt(LE.getPrompt());
            }
         }
      }

      void SetColors(const char* colorTab, const char* colorTabComp,
                     const char* colorBracket, const char* colorBadBracket,
                     const char* colorPrompt) {
         // LE.setPromptColor(colorPrompt); // Placeholder
      }

      static void SetHistoryFile(const char* hist) {
         // Placeholder
      }

      static void SetHistSize(int size, int save) {
         // Placeholder
      }

      static LineEditorHolder& getHolder() {
         static LineEditorHolder sLEHolder(fgHistoryFile);
         return sLEHolder;
      }

      llvm::LineEditor& get() {
         return LE;
      }

   private:
      llvm::LineEditor LE;  // LineEditor instance
      std::string fInputLine; // Stores the input line taken from user

      static std::string fgHistoryFile;
   };

   std::string LineEditorHolder::fgHistoryFile;
}

/************************ extern "C" part *********************************/

#include <termios.h>
#include <unistd.h>
#include <iostream>

extern "C" {
void
Gl_config(const char* which, int value) {
   if (strcmp(which, "noecho") == 0) {
      // Placeholder
   } else {
      // unsupported directive
      printf("Gl_config unsupported: %s ?\n", which);
   }
}

void Gl_histadd(const char *buf)
{
   // llvm::LineEditor handles history automatically.
}

/* Wrapper around textinput.
 * Modes: -1 = init, 0 = line mode, 1 = one char at a time mode, 2 = cleanup, 3 = clear input line
 */
const char *Getlinem(EGetLineMode mode, const char *prompt)
{

   if (mode == kClear) {
      LineEditorHolder::getHolder().TakeInput(true);

      return nullptr;
   }

   if (mode == kCleanUp) {
      // Placeholder
      return nullptr;
   }

   if (mode == kOneChar) {
      // Placeholder
      // mode = kLine1;
   }

   if (mode == kInit || mode == kLine1) {
      llvm::LineEditor &LE = LineEditorHolder::getHolder().get();
      // Set the prompt
      if (prompt) {
         LE.setPrompt(prompt);
      }

      if (mode == kInit) {
         return nullptr;
      }

      // Read the input line using LineEditor
      // std::optional<std::string> line = LE.readLine();
      // if (line) {
      //    // return line->c_str();
      // }
   } else
      return LineEditorHolder::getHolder().TakeInput();
   return nullptr;
}

const char*
Getline(const char* prompt) {
   // Get a line of user input, showing prompt.
   // Does not return after every character entered, but
   // only returns once the user has hit return.
   // For ROOT Getline.c backward compatibility reasons,
   // the returned value is volatile and will be overwritten
   // by the subsequent call to Getline() or Getlinem(),
   // so copy the string if it needs to stay around.
   // The returned value must not be deleted.
   // The returned string contains a trailing newline '\n'.

   return Getlinem(kLine1, prompt);
}


/******************* Simple C -> C++ forwards *********************************/

void
Gl_histsize(int size, int save) {
   LineEditorHolder::SetHistSize(size, save);
}

void
Gl_histinit(const char* file) {
   // Has to be called before constructing LineEditorHolder singleton.
   LineEditorHolder::SetHistoryFile(file);
}

int Gl_eof() {
    // Check for EOF using std::optional from LineEditor
    llvm::LineEditor& editor = LineEditorHolder::getHolder().get();
    std::optional<std::string> line = editor.readLine();
    if (!line) {
        return 1;  // EOF detected
    }
    return 0;
}


void
Gl_setColors(const char* colorTab, const char* colorTabComp, const char* colorBracket,
             const char* colorBadBracket, const char* colorPrompt) {
   // call to enhance.cxx to set colours
   LineEditorHolder::getHolder().SetColors(colorTab, colorTabComp, colorBracket,
                                          colorBadBracket, colorPrompt);
}

/******************** Superseded interface *********************************/

void Gl_setwidth(int /*w*/) {
   // ignored, handled by displays themselves.
}


void Gl_windowchanged() {
   // ignored, handled by displays themselves.
}

} // extern "C"
