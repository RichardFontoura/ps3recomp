#include "../ppu_diagnostics.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

int main()
{
    std::atomic<unsigned> reports{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (unsigned i = 0; i < 8; ++i)
                if (ppu_diag_hit("shared-gap")) ++reports;
        });
    for (auto& thread : threads) thread.join();
    assert(reports == 8);
    assert(ppu_diag_hit("different-gap") == 1);
    assert(ppu_diag_hit("shared-gap") == 0);
    puts("ppu_diagnostics: PASS");
}
