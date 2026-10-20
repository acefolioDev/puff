#include "ui.hpp"
#include <iostream>
int main(int argc, char **argv) {
  try {
    puff::Application app;
    return app.run(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "My Puff: " << e.what() << '\n';
    return 1;
  }
}
