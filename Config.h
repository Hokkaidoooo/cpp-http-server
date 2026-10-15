#pragma once
#include "HttpRequest.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>

struct Config {
    unsigned short port = 8080;
    std::size_t workers = 4, queueCapacity = 64, maxConnections = 512;
    std::size_t maxUploadBytes = 1024 * 1024, maxImagePixels = 4 * 1024 * 1024;
    std::size_t maxImagesPerUser = 100, maxSessions = 1024, sessionSeconds = 1800;
    std::size_t readTimeoutMs = 5000, processingTimeoutMs = 30000, writeTimeoutMs = 5000;
    bool cookieSecure = true;
    std::string mongoUri, mongoDatabase, bindAddress = "127.0.0.1";
    std::string publicOrigin = "http://localhost:8080";
    std::filesystem::path appRoot = std::filesystem::current_path(), storageRoot;
    HttpLimits httpLimits() const { return {16 * 1024, maxUploadBytes + 16 * 1024}; }
    static Config fromEnvironment(int argc, char** argv) {
        if (argc > 2) throw std::invalid_argument("用法：myserver [端口]");
        Config config;
        config.mongoUri = required("MONGODB_URI"); config.mongoDatabase = required("MONGODB_DATABASE");
        config.bindAddress = env("BIND_ADDRESS", "127.0.0.1");
        for (const unsigned char c : config.mongoDatabase)
            if (!(std::isalnum(c) || c == '_')) throw std::invalid_argument("MONGODB_DATABASE 只能使用字母、数字、下划线");
        config.port = static_cast<unsigned short>(number(argc == 2 ? argv[1] : env("PORT", "8080"), "PORT", 65535));
        config.workers = number(env("WORKER_THREADS", "4"), "WORKER_THREADS", 64);
        config.queueCapacity = number(env("TASK_QUEUE_CAPACITY", "64"), "TASK_QUEUE_CAPACITY", 4096);
        config.maxConnections = number(env("MAX_CONNECTIONS", "512"), "MAX_CONNECTIONS", 65536);
        config.maxUploadBytes = number(env("MAX_UPLOAD_BYTES", "1048576"), "MAX_UPLOAD_BYTES", 16 * 1024 * 1024);
        config.maxImagePixels = number(env("MAX_IMAGE_PIXELS", "4194304"), "MAX_IMAGE_PIXELS", 16 * 1024 * 1024);
        config.maxImagesPerUser = number(env("MAX_IMAGES_PER_USER", "100"), "MAX_IMAGES_PER_USER", 10000);
        config.maxSessions = number(env("MAX_SESSIONS", "1024"), "MAX_SESSIONS", 100000);
        config.sessionSeconds = number(env("SESSION_TTL_SECONDS", "1800"), "SESSION_TTL_SECONDS", 86400);
        config.readTimeoutMs = number(env("READ_TIMEOUT_MS", "5000"), "READ_TIMEOUT_MS", 300000);
        config.processingTimeoutMs = number(env("PROCESSING_TIMEOUT_MS", "30000"), "PROCESSING_TIMEOUT_MS", 300000);
        config.writeTimeoutMs = number(env("WRITE_TIMEOUT_MS", "5000"), "WRITE_TIMEOUT_MS", 300000);
        const auto secure = env("SESSION_COOKIE_SECURE", "1");
        if (secure != "0" && secure != "1") throw std::invalid_argument("SESSION_COOKIE_SECURE 必须为 0 或 1");
        config.cookieSecure = secure == "1";
        config.publicOrigin = env("PUBLIC_ORIGIN", "http://localhost:" + std::to_string(config.port));
        if ((config.publicOrigin.rfind("http://", 0) != 0 && config.publicOrigin.rfind("https://", 0) != 0) ||
            config.publicOrigin.find_first_of("\r\n \t") != std::string::npos)
            throw std::invalid_argument("PUBLIC_ORIGIN 必须为合法 HTTP(S) origin");
        config.appRoot = std::filesystem::absolute(env("APP_ROOT", config.appRoot.string()));
        config.storageRoot = std::filesystem::absolute(env("STORAGE_ROOT", (config.appRoot / "images").string()));
        return config;
    }
private:
    static std::string env(const char* name, const std::string& defaultValue) {
        const char* value = std::getenv(name); return value ? value : defaultValue;
    }
    static std::string required(const char* name) {
        const auto value = env(name, "");
        if (value.empty()) throw std::invalid_argument(std::string("缺少环境变量 ") + name);
        return value;
    }
    static std::size_t number(const std::string& value, const char* name, std::size_t maximum) {
        std::size_t result = 0;
        if (value.empty()) throw std::invalid_argument(std::string(name) + " 不能为空");
        for (const unsigned char c : value) {
            if (c < '0' || c > '9' || result > maximum / 10)
                throw std::invalid_argument(std::string(name) + " 数值非法或超限");
            result = result * 10 + c - '0';
            if (result > maximum) throw std::invalid_argument(std::string(name) + " 超限");
        }
        if (!result) throw std::invalid_argument(std::string(name) + " 必须大于零");
        return result;
    }
};
