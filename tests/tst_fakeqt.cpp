// Stand-in for a Qt Test executable, used only by RunQtTestSelfTest.cmake.
//   tst_fakeqt -o <log>,txt
// copies the file named by the environment variable FAKE_CONTENT to <log> (when
// set) and exits with the code in FAKE_EXIT. It touches nothing else.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char *argv[])
{
    std::string log;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "-o")
            log = argv[i + 1];
    }
    const std::string suffix = ",txt";
    if (log.size() > suffix.size() && log.compare(log.size() - suffix.size(), suffix.size(), suffix) == 0)
        log.resize(log.size() - suffix.size());

    const char *content = std::getenv("FAKE_CONTENT");
    if (content && !log.empty()) {
        std::ifstream in(content, std::ios::binary);
        std::ofstream out(log, std::ios::binary);
        out << in.rdbuf();
    }
    const char *code = std::getenv("FAKE_EXIT");
    return code ? std::atoi(code) : 0;
}
