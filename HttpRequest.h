#pragma once
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

struct HttpLimits {
    std::size_t headerBytes = 16 * 1024;
    std::size_t bodyBytes = 1024 * 1024 + 16 * 1024;
};

class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& message) : std::runtime_error(message), status(status) {}
    const int status;
};

inline std::string lowerAscii(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

inline std::string trimOws(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

class HttpRequest {
public:
    enum class State { RequestLine, Headers, Body, Complete, Failed };
    explicit HttpRequest(HttpLimits limits = {}) : limits_(limits) {}

    State feed(const std::string& bytes) {
        if (state_ == State::Failed) return state_;
        if (state_ == State::Complete) {
            if (!bytes.empty()) fail(400, "一个连接只接受一个请求");
            return state_;
        }
        if (bytes.size() > limits_.headerBytes + limits_.bodyBytes - wire_.size()) {
            fail(413, "请求超过大小限制");
            return state_;
        }
        wire_ += bytes;
        while (state_ == State::RequestLine || state_ == State::Headers) {
            const auto end = wire_.find("\r\n", cursor_);
            if (end == std::string::npos) {
                if (wire_.size() > limits_.headerBytes) fail(431, "请求头过大");
                return state_;
            }
            if (end + 2 > limits_.headerBytes) {
                fail(431, "请求头过大");
                return state_;
            }
            const auto line = wire_.substr(cursor_, end - cursor_);
            cursor_ = end + 2;
            if (state_ == State::RequestLine) parseRequestLine(line);
            else if (line.empty()) finishHeaders();
            else parseHeader(line);
        }
        if (state_ != State::Body) return state_;
        const auto available = wire_.size() - cursor_;
        if (available > contentLength_) fail(400, "正文超出声明长度或包含流水线请求");
        else if (available == contentLength_) {
            body_ = wire_.substr(cursor_);
            wire_.clear();
            state_ = State::Complete;
        }
        return state_;
    }

    State endInput() {
        if (state_ != State::Complete && state_ != State::Failed) fail(400, "请求尚未完整时连接已结束");
        return state_;
    }

    State state() const { return state_; }
    int errorStatus() const { return errorStatus_; }
    const std::string& errorMessage() const { return errorMessage_; }
    const std::string& getMethodString() const { return method_; }
    const std::string& getPath() const { return path_; }
    const std::string& body() const { return body_; }
    std::string getHeader(const std::string& name) const {
        const auto it = headers_.find(lowerAscii(name));
        return it == headers_.end() ? std::string{} : it->second;
    }

    std::unordered_map<std::string, std::string> parseFormBody() const {
        if (lowerAscii(getHeader("content-type")) != "application/x-www-form-urlencoded")
            throw HttpError(415, "需要 application/x-www-form-urlencoded 表单");
        std::unordered_map<std::string, std::string> fields;
        std::size_t begin = 0;
        while (begin < body_.size()) {
            const auto amp = body_.find('&', begin);
            const auto end = amp == std::string::npos ? body_.size() : amp;
            const auto equal = body_.find('=', begin);
            if (equal == std::string::npos || equal >= end) throw HttpError(400, "表单字段缺少等号");
            const auto key = decodeForm(body_.substr(begin, equal - begin));
            if (key.empty() || !fields.emplace(key, decodeForm(body_.substr(equal + 1, end - equal - 1))).second)
                throw HttpError(400, "表单字段为空或重复");
            if (fields.size() > 8) throw HttpError(400, "表单字段过多");
            begin = end + 1;
        }
        return fields;
    }

private:
    static bool tokenChar(unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string::npos;
    }
    void fail(int status, const std::string& message) {
        state_ = State::Failed; errorStatus_ = status; errorMessage_ = message;
    }
    void parseRequestLine(const std::string& line) {
        const auto first = line.find(' '), second = line.find(' ', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos || line.find(' ', second + 1) != std::string::npos) {
            fail(400, "请求行格式错误"); return;
        }
        method_ = line.substr(0, first);
        path_ = line.substr(first + 1, second - first - 1);
        if (method_ != "GET" && method_ != "POST") { fail(405, "只支持 GET 和 POST"); return; }
        if (line.substr(second + 1) != "HTTP/1.1") { fail(505, "只支持 HTTP/1.1"); return; }
        if (path_.empty() || path_[0] != '/') { fail(400, "需要绝对路径形式的请求目标"); return; }
        for (const unsigned char c : path_) if (c <= 32 || c >= 127 || c == '\\' || c == '#') {
            fail(400, "请求路径包含非法字符"); return;
        }
        state_ = State::Headers;
    }
    void parseHeader(const std::string& line) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) { fail(400, "请求头缺少字段名"); return; }
        for (std::size_t i = 0; i < colon; ++i) if (!tokenChar(static_cast<unsigned char>(line[i]))) {
            fail(400, "请求头字段名非法"); return;
        }
        const auto value = trimOws(line.substr(colon + 1));
        for (const unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) {
            fail(400, "请求头值包含控制字符"); return;
        }
        if (!headers_.emplace(lowerAscii(line.substr(0, colon)), value).second) fail(400, "不接受重复请求头");
    }
    void finishHeaders() {
        if (getHeader("host").empty()) { fail(400, "缺少 Host"); return; }
        if (headers_.count("transfer-encoding")) {
            fail(headers_.count("content-length") ? 400 : 501, "不支持 Transfer-Encoding"); return;
        }
        if (headers_.count("expect")) { fail(417, "不支持 Expect"); return; }
        if (headers_.count("trailer")) { fail(400, "不支持 Trailer"); return; }
        const auto it = headers_.find("content-length");
        if (it == headers_.end()) {
            if (method_ == "POST") { fail(411, "POST 必须声明 Content-Length"); return; }
        } else {
            if (it->second.empty()) { fail(400, "Content-Length 为空"); return; }
            for (const unsigned char c : it->second) {
                if (c < '0' || c > '9') { fail(400, "Content-Length 必须为非负十进制整数"); return; }
                const auto digit = static_cast<std::size_t>(c - '0');
                if (contentLength_ > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                    fail(413, "Content-Length 溢出"); return;
                }
                contentLength_ = contentLength_ * 10 + digit;
            }
        }
        if (contentLength_ > limits_.bodyBytes) { fail(413, "请求正文过大"); return; }
        state_ = State::Body;
    }
    static int hex(unsigned char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    static std::string decodeForm(const std::string& value) {
        std::string result;
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = value[i];
            if (c == '+') result += ' ';
            else if (c == '%') {
                if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                    throw HttpError(400, "表单包含无效百分号编码");
                result += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2])); i += 2;
            } else result += c;
        }
        return result;
    }
    HttpLimits limits_;
    State state_ = State::RequestLine;
    std::size_t cursor_ = 0, contentLength_ = 0;
    int errorStatus_ = 0;
    std::string wire_, method_, path_, body_, errorMessage_;
    std::unordered_map<std::string, std::string> headers_;
};
