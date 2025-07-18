#include <string.h>

template <typename T> class Nothing;

void case1() {
    std::string output;
    // Does a lookup that provokes a auto-parsing following by
    // an (intentional) compilation error and thus a set of unloading.
    gInterpreter->GetInterpreterTypeName("Nothing<AnotherRandomClass>", output);
}
