// The receivers the plugin would call, as JSON, through the plugin's own parser (src/kiwi_list.h),
// so the manual's map and counts are the plugin's and not a copy of its rules.
//   clang++ -std=c++17 -O1 -I ../src receivers_json.cpp -o <somewhere>/receivers_json.exe
//   receivers_json.exe ../Content/receivers.js
#include "kiwi_list.h"

#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    const auto list = kiwi::parse(s.str());
    std::printf("[");
    for (size_t i = 0; i < list.size(); ++i)
        std::printf("%s{\"lat\":%.4f,\"lon\":%.4f,\"region\":%d,\"max\":%d}", i ? "," : "", list[i].lat, list[i].lon, list[i].region,
                    list[i].usersMax);
    std::printf("]\n");
    return 0;
}
