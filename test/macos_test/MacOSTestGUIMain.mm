#include <iostream>
#include <string>

#include "./MacOSTestGUI.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <window_title>" << std::endl;
        return 1;
    }

    std::string windowTitle = argv[1];
    MacOSTestGUI gui(windowTitle);
    gui.run();

    return 0;
}
