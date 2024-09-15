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

      // const char* TakeInput(bool force = false) {
      //    TakeInput(fInputLine, force);
      //    fInputLine += "\n"; // ROOT wants a trailing newline.
      //    return fInputLine.c_str();
      // }

      const char* TakeInput(bool force = false) {
         static llvm::LineEditor &LE = getHolder().get();
         std::optional<std::string> readLine = LE.readLine();
         
         if (readLine) {
            // Store the input
            fInputLine = *readLine;

            // Strip trailing newlines or carriage returns
            // fInputLine.erase(fInputLine.find_last_not_of("\r\n") + 1);

            // Return the input with a trailing newline as expected by ROOT
            fInputLine += "\n";
            return fInputLine.c_str();
         }

         // Handle EOF
         fInputLine.clear();
         return nullptr;
      }

      // void TakeInput(std::string &input, bool force)
      // {
      //    static llvm::LineEditor &LE = getHolder().get(); // Get the LineEditor instance

      //    // Read a line from the editor
      //    std::optional<std::string> readLine = LE.readLine();

      //    if (readLine) {
      //       // Store the input
      //       input = *readLine;
      //       llvm::StringRef L = input;
      //       L = L.trim();
      //       input = L;

      //       // // Remove trailing carriage return characters if present
      //       // while (!input.empty() && input.back() == '\r') {
      //       //    input.pop_back();
      //       // }
      //       while (!input.empty() && input[input.length() - 1] == 13) {
      //          input.erase(input.length() - 1);
      //       }

      //       // Reset the state or signal that input was taken
      //       // LE.setPrompt(LE.getPrompt()); // Example: resetting the prompt (adapt as necessary)

      //       // Reset internal states (if applicable) and continue with normal operation
      //    } else {
      //       // Handle EOF scenario if no input is retrieved
      //       input.clear();
      //       if (force) {
      //          // If forcing input, prepare the editor for another input cycle
      //          LE.setPrompt(LE.getPrompt());
      //       }
      //    }
      // }

      // void
      // TextInput::TakeInput(std::string &input, bool force) {
      //    if (!force && fLastReadResult != kRRReadEOLDelimiter
      //       && fLastReadResult != kRREOF) {
      //       input.clear();
      //       return;
      //    }
      //    input = fContext->GetLine().GetText();
      //    while (!input.empty() && input[input.length() - 1] == 13) {
      //       input.erase(input.length() - 1);
      //    }
      //    fContext->GetEditor()->ResetText();

      //    // Signal displays that the input got taken.
      //    std::for_each(fContext->GetDisplays().begin(), fContext->GetDisplays().end(),
      //             [](Display *D) { return D->NotifyResetInput(); });

      //    ReleaseInputOutput();

      //    if (force || fLastReadResult == kRRReadEOLDelimiter) {
      //       // Input has been taken, we can continue reading.
      //       fLastReadResult = kRRNone;
      //       // We will have to redraw the prompt, even if unchanged.
      //       fNeedPromptRedraw = true;
      //    } else {
      //       fLastReadResult = kRREOF;
      //    }
      // }

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

#include <termios.h>
#include <unistd.h>

void DisableTerminalEcho() {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

void EnableTerminalEcho() {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

extern "C" {
void
Gl_config(const char* which, int value) {
   if (strcmp(which, "noecho") == 0) {
      // Placeholder
      DisableTerminalEcho();
   } else {
      // unsupported directive
      // printf("Gl_config unsupported: %s ?\n", which);
   }
}

void Gl_histadd(const char *buf)
{
   // llvm::LineEditor handles history automatically.
}

/* Wrapper around textinput.
 * Modes: -1 = init, 0 = line mode, 1 = one char at a time mode, 2 = cleanup, 3 = clear input line
 */
const char *Getlinem(EGetLineMode mode, const char *prompt) {
   if (mode == kClear) {
      return nullptr;
   }

   if (mode == kCleanUp) {
      return nullptr;
   }

   if (mode == kOneChar) {
      // Placeholder for single character mode (optional)
      mode = kLine1;
   }

   if (mode == kInit || mode == kLine1) {
      llvm::LineEditor &LE = LineEditorHolder::getHolder().get();

      // Set the prompt only if provided
      if (prompt) {
         LE.setPrompt(prompt);
      }

      // If in init mode, no input is expected, return
      if (mode == kInit) {
         return nullptr;
      }

      // Read input from the user using LineEditor
      const char* input = LineEditorHolder::getHolder().TakeInput();
      if (input && strlen(input) > 0) {
         return input;  // Return the line of input
      }

      // If no input or EOF, return nullptr
      return nullptr;
   }

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
    llvm::LineEditor& editor = LineEditorHolder::getHolder().get();
    std::optional<std::string> line = editor.readLine();
    return line ? 0 : 1;  // Return 1 if EOF is detected
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
