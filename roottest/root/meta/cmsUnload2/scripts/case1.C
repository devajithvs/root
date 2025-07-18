#include <string.h>
template <typename T> class Nothing;

void case1() {
    std::string output;
    // Does a lookup that provokes a auto-parsing following by
    // an (intentional) compilation error and thus a set of unloading.
    gInterpreter->GetInterpreterTypeName("Nothing<AnotherRandomClass>", output);

    gInterpreter->AutoParse("reco::utils::ClusterTotals");

        // // // This will cause autoload + parsing of the template
        // Nothing<AnotherRandomClass> *ptr = nullptr;

        // // std::string output;
        // // gInterpreter->GetInterpreterTypeName("Nothing<AnotherRandomClass>", output);
        
        // // Now provoke a compilation error, forcing unloading
        // ptr->NonExistentMethod();


    // AnotherRandomClass ptr2;
    // ptr2.print();

}
