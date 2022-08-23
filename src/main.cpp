#include "myShell.h"
#include <cstdlib>

int main(int argc, char ** argv, char ** envp) {
    MyShell myShell;
    while (!myShell.isExitting()) {
        myShell.execute();
    }
    return EXIT_SUCCESS;
}