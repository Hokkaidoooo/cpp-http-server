#include "Database.h"
#include "HttpServer.h"
#include "Router.h"
#include <mongocxx/instance.hpp>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }
}

int main(int argc, char** argv) {
    try {
        const auto config = Config::fromEnvironment(argc, argv);
        if (sodium_init() < 0) throw std::runtime_error("libsodium 初始化失败");
        struct sigaction action{};
        action.sa_handler = requestStop;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0)
            throw systemFailure("安装退出信号处理失败");
        mongocxx::instance mongoRuntime;
        Database database(config.mongoUri, config.mongoDatabase);
        Auth auth(database, config);
        ImageStorage storage(config);
        Router router(auth, storage, config);
        HttpServer server(config, [&router](const HttpRequest& request) { return router.routeRequest(request); });
        server.run(stopRequested);
        return 0;
    } catch (const mongocxx::exception& error) {
        std::cerr << "FATAL MongoDB 初始化失败，检查连接、权限与 schema；错误码=" << error.code().value() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FATAL " << error.what() << '\n';
    }
    return 1;
}
