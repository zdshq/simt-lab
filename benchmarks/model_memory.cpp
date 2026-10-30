#include "model_memory.hpp"

namespace calibration::model {
void run(int argc, char** argv) {
    if (argc < 2) throw std::runtime_error("expected coalesce/shared/chase");
    const std::string mode = argv[1];
    if (mode == "coalesce" && argc == 4) coalescing(number(argv[2],0,8),number(argv[3],0,7));
    else if (mode == "shared" && argc == 4) shared(number(argv[2],0,33),number(argv[3],1,65536));
    else if (mode == "chase" && argc == 6) {
        const std::string policy = argv[2];
        if (policy != "ca" && policy != "cg") throw std::runtime_error("invalid cache policy");
        chase(policy=="cg",number(argv[3],1024,1048576),number(argv[4],0,65536),number(argv[5],1,UINT32_MAX));
    }
    else throw std::runtime_error("invalid experiment arguments");
}
int application(int argc, char** argv) {
    try { run(argc,argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
int main(int argc, char** argv) { return calibration::model::application(argc,argv); }
