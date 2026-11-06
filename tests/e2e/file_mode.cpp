#include "platform.h"

import std;

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    unsigned mode = 0;
    if (llmgate::platform::file_mode(argv[1], &mode) != 0) {
        return 1;
    }
    std::println("{:o}", mode & 0777U);
    return 0;
}
