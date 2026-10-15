#pragma once
#include "Config.h"
#include <chrono>
#include <mutex>
#include <optional>
#include <sodium.h>

class UserRepository {
public:
    virtual ~UserRepository() = default;
    virtual bool insertUser(const std::string& username, const std::string& passwordHash) = 0;
    virtual std::optional<std::string> passwordHash(const std::string& username) = 0;
};

inline std::string randomToken() {
    unsigned char bytes[32];
    char hex[65];
    randombytes_buf(bytes, sizeof bytes);
    sodium_bin2hex(hex, sizeof hex, bytes, sizeof bytes);
    return hex;
}

inline std::string stableIdentifier(const std::string& value) {
    unsigned char bytes[32];
    char hex[65];
    if (crypto_generichash(bytes, sizeof bytes,
                           reinterpret_cast<const unsigned char*>(value.data()), value.size(), nullptr, 0) != 0)
        throw std::runtime_error("生成文件标识失败");
    sodium_bin2hex(hex, sizeof hex, bytes, sizeof bytes);
    return hex;
}

class Auth {
public:
    Auth(UserRepository& users, const Config& config) : users_(users), config_(config) {}
    void registerUser(const HttpRequest& request) {
        const auto credentials = readCredentials(request);
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str_alg(hash, credentials.second.data(), credentials.second.size(),
                                 crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                 crypto_pwhash_ALG_ARGON2ID13) != 0)
            throw std::runtime_error("Argon2id 内存分配失败");
        if (!users_.insertUser(credentials.first, hash)) throw HttpError(409, "用户名已存在");
    }
    std::string login(const HttpRequest& request) {
        const auto credentials = readCredentials(request);
        const auto hash = users_.passwordHash(credentials.first);
        if (!hash) throw HttpError(401, "用户名或密码错误");
        if (hash->rfind("$argon2id$", 0) != 0 || hash->find('\0') != std::string::npos ||
            crypto_pwhash_str_needs_rehash(hash->c_str(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                         crypto_pwhash_MEMLIMIT_INTERACTIVE) < 0)
            throw std::runtime_error("密码记录不是有效的 Argon2id 格式，需要显式迁移或修复");
        if (crypto_pwhash_str_verify(hash->c_str(), credentials.second.data(), credentials.second.size()) != 0)
            throw HttpError(401, "用户名或密码错误");
        const auto token = randomToken();
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();)
            if (it->second.expires <= now) it = sessions_.erase(it); else ++it;
        if (sessions_.size() >= config_.maxSessions) throw HttpError(503, "会话容量已满，请稍后重试");
        if (!sessions_.emplace(token, Session{credentials.first, now + std::chrono::seconds(config_.sessionSeconds)}).second)
            throw std::runtime_error("会话标识发生碰撞");
        return token;
    }
    std::string requireUser(const HttpRequest& request) {
        const auto token = cookieToken(request);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(token);
        if (found == sessions_.end()) throw HttpError(401, "请先登录");
        if (found->second.expires <= Clock::now()) {
            sessions_.erase(found); throw HttpError(401, "登录已过期");
        }
        return found->second.username;
    }
    void logout(const HttpRequest& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(cookieToken(request));
    }
    std::string cookie(const std::string& token) const {
        return "session=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
               std::to_string(token.empty() ? 0 : config_.sessionSeconds) + (config_.cookieSecure ? "; Secure" : "");
    }
    void requireSameOrigin(const HttpRequest& request) const {
        const auto origin = request.getHeader("origin");
        if (!origin.empty() && origin != config_.publicOrigin) throw HttpError(403, "请求来源不匹配");
        if (lowerAscii(request.getHeader("sec-fetch-site")) == "cross-site") throw HttpError(403, "不接受跨站写入");
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Session { std::string username; Clock::time_point expires; };
    static std::pair<std::string, std::string> readCredentials(const HttpRequest& request) {
        const auto fields = request.parseFormBody();
        if (fields.size() != 2 || !fields.count("username") || !fields.count("password"))
            throw HttpError(400, "必须且只能填写 username 和 password");
        const auto& username = fields.at("username");
        const auto& password = fields.at("password");
        if (username.size() < 3 || username.size() > 32) throw HttpError(400, "用户名长度须为 3 至 32 字节");
        for (const unsigned char c : username)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                throw HttpError(400, "用户名只能使用英文字母、数字、下划线和连字符");
        if (password.size() < 12 || password.size() > 128 || password.find('\0') != std::string::npos)
            throw HttpError(400, "密码长度须为 12 至 128 字节且不能含空字符");
        return {username, password};
    }
    static std::string cookieToken(const HttpRequest& request) {
        const auto cookies = request.getHeader("cookie");
        std::string result;
        std::size_t begin = 0;
        while (begin < cookies.size()) {
            const auto semi = cookies.find(';', begin);
            const auto part = trimOws(cookies.substr(begin, semi == std::string::npos ? std::string::npos : semi - begin));
            if (part.rfind("session=", 0) == 0) {
                if (!result.empty()) throw HttpError(400, "重复的会话 Cookie");
                result = part.substr(8);
                if (result.size() != 64) throw HttpError(401, "无效会话");
                for (const char c : result) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    throw HttpError(401, "无效会话");
            }
            if (semi == std::string::npos) break;
            begin = semi + 1;
        }
        return result;
    }
    UserRepository& users_;
    const Config& config_;
    std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
};
