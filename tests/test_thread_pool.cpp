#include "ThreadPool.h"
#include <atomic>
#include <future>
#include <iostream>

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        ThreadPool pool(1, 1);
        std::promise<void> entered, release;
        const auto gate = release.get_future().share();
        std::atomic<int> finished{0};
        check(pool.tryEnqueue([&] { entered.set_value(); gate.wait(); ++finished; }), "首个任务入队失败");
        entered.get_future().wait();
        const bool queued = pool.tryEnqueue([&] { ++finished; });
        const bool excess = pool.tryEnqueue([&] { ++finished; });
        release.set_value();
        pool.shutdown();
        check(queued && !excess, "满队列没有明确拒绝");
        check(finished == 2, "任务重复执行或关闭时丢失");
        check(!pool.tryEnqueue([] {}), "关闭后仍接受任务");
        std::atomic<int> count{0};
        ThreadPool concurrent(4, 64);
        for (int i = 0; i < 4000; ++i) {
            while (!concurrent.tryEnqueue([&] { ++count; })) std::this_thread::yield();
        }
        concurrent.shutdown();
        check(count == 4000, "并发任务计数不一致");
        std::cout << "PASS thread_pool checks=5 tasks=4002\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
