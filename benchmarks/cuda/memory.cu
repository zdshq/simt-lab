#include "chase.cuh"
#include "shared.cuh"
#include "coalesce.cuh"

namespace calibration {
void run(int argc, char** argv) {
    if (argc < 3) throw std::runtime_error("usage: memory-bench probe device | chase/shared/coalesce arguments device");
    check(cudaSetDevice(number(argv[argc-1],0,128))); metadata();
    const std::string mode = argv[1];
    if (mode == "probe" && argc == 3) return;
    if (mode == "chase") run_chase(argc,argv);
    else if (mode == "shared") run_shared(argc,argv);
    else if (mode == "coalesce") run_coalesce(argc,argv);
    else throw std::runtime_error("unknown experiment");
}
int run_application(int argc, char** argv) {
    try { run(argc,argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main(int argc, char** argv) { return calibration::run_application(argc,argv); }
