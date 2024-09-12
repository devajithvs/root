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

extern "C" {
   int (* Gl_in_key)(int ch) = nullptr;
   int (* Gl_beep_hook)() = nullptr;
}

#include "llvm/LineEditor/LineEditor.h"

// using namespace textinput;

namespace {
   class ROOTTabCompletion {
   public:
      std::vector<llvm::LineEditor::Completion> operator()(llvm::StringRef Buffer, size_t Pos) const {
         char fLineBuf[16 * 1024]; // Buffer size
         std::vector<std::string> displayCompletions;

         // Invoke the application’s tab completion mechanism
         std::stringstream sstr;
         int cursorInt = (int)Pos;
         size_t posFirstChange = gApplication->TabCompletionHook(fLineBuf, &cursorInt, sstr);

         if (posFirstChange == (size_t)-1) {
               return {};  // No completions available
         }

         std::string compLine;
         while (std::getline(sstr, compLine)) {
               displayCompletions.push_back(compLine);
         }

         std::vector<llvm::LineEditor::Completion> results;
         for (const auto& comp : displayCompletions) {
               results.emplace_back(comp, comp);
         }

         return results;
      }
   };


   // Helper to define the lifetime of the TextInput singleton.
   class LineEditorHolder {
   public:
   LineEditorHolder(const std::string& historyFile)
        : LE("root-repl", historyFile.c_str()), historyFile(historyFile) {
   }

      ~LineEditorHolder() = default;

      // Take input from the user, returns the input line
      const char* TakeInput(bool force = false) {
         std::optional<std::string> Input = LE.readLine();
         if (Input) {
               fInputLine = *Input + "\n"; // ROOT wants trailing newline
         } else {
            fInputLine.clear();
         }
         return fInputLine.c_str();
      }

      void SetColors(const char* colorTab, const char* colorTabComp,
                     const char* colorBracket, const char* colorBadBracket,
                     const char* colorPrompt) {
         // This part depends on whether you want to use any custom coloring in LineEditor
         // Custom color support can be configured for prompts or completions here
         // LE.setPromptColor(colorPrompt); // Example of setting prompt color
      }

      static void SetHistoryFile(const char* hist) {
         fgHistoryFile = hist;
      }

      static void SetHistSize(int size, int save) {
         fgSizeLines = size;
         fgSaveLines = save;
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
      std::string fInputLine;  // Stores the input line taken from user
      std::string historyFile;  // Path to history file

      static std::string fgHistoryFile;
      static int fgSizeLines;
      static int fgSaveLines;
   };

   std::string LineEditorHolder::fgHistoryFile;
   int LineEditorHolder::fgSizeLines = 500;
   int LineEditorHolder::fgSaveLines = -1;
}

/************************ extern "C" part *********************************/

#include <termios.h>
#include <unistd.h>
#include <iostream>

void SetEcho(bool enableEcho) {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    if (!enableEcho) {
        tty.c_lflag &= ~ECHO;  // Disable echoing
    } else {
        tty.c_lflag |= ECHO;  // Enable echoing
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);  // Apply the settings
}

extern "C" {
void
Gl_config(const char* which, int value) {
    if (strcmp(which, "noecho") == 0) {
        SetEcho(value == 0);  // Disable echo if value == 0
    } else {
        printf("Gl_config unsupported: %s ?\n", which);
    }
}

void Gl_histadd(const char* buf) {
   // llvm::LineEditor handles history automatically.
}

/* Wrapper around textinput.
 * Modes: -1 = init, 0 = line mode, 1 = one char at a time mode, 2 = cleanup, 3 = clear input line
 */
const char* Getlinem(EGetLineMode mode, const char* prompt) {
    static std::string inputLine;

    if (mode == kClear) {
        inputLine.clear(); // Clear input buffer
        return nullptr;
    }

    if (mode == kCleanUp) {
        // Cleanup actions
        return nullptr;
    }

    if (mode == kInit || mode == kLine1) {
        llvm::LineEditor& editor = LineEditorHolder::getHolder().get();
        editor.setPrompt(prompt ? prompt : "");

        std::optional<std::string> line = editor.readLine();
        if (line) {
            inputLine = *line + "\n";  // Add newline for ROOT

            // If the command is '.q' or '.quit', exit
            if (inputLine == ".q\n" || inputLine == ".quit\n") {
                std::exit(0);  // Exit the program
            }

            // Add the line to history if necessary
            Gl_histadd(inputLine.c_str());

            // **Process the input using the ROOT interpreter**
            // This is important to ensure that the commands are executed.
            gInterpreter->ProcessLine(inputLine.c_str());

            return inputLine.c_str();  // Return the input line with a newline
        }

        return nullptr;
    }

    if (mode == kOneChar) {
        // Implement single-character input reading
        struct termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);  // Get terminal attributes
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echoing
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        char ch;
        read(STDIN_FILENO, &ch, 1);  // Read one character from stdin

        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore terminal attributes
        inputLine = ch;  // Store the single character input
        return inputLine.c_str();
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
