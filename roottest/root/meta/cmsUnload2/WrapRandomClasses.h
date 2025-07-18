#ifndef WrapRandomClasses_H
#define WrapRandomClasses_H

#include "SomeRandomClass.h"
#include "AnotherRandomClass.h"
template <typename T> class Wrapper {};

struct dictionary1 {
   Wrapper<SomeRandomClass<AnotherRandomClass>> zs4_bis;
   Wrapper<AnotherRandomClass> zs0;
};

#endif
