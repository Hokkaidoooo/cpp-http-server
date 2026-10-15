#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Multipart.h"
#include <iostream>
#include <random>
#include <vector>

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string post(const std::string& body, const std::string& type = "application/x-www-form-urlencoded") {
    return "POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Type: " + type +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
int main() {
    try {
        std::size_t checks = 0;
        const std::string body = "username=demo&password=correct%2Bhorse";
        const auto wire = post(body);
        for (std::size_t split = 0; split < wire.size(); ++split) {
            HttpRequest request;
            check(request.feed(wire.substr(0, split)) != HttpRequest::State::Complete, "分段请求被提前接受");
            check(request.feed(wire.substr(split)) == HttpRequest::State::Complete, "完整请求未完成");
            check(request.body() == body, "正文不一致");
            check(request.parseFormBody().at("password") == "correct+horse", "百分号解码错误");
            ++checks;
        }
        HttpRequest bytewise;
        for (const char c : wire) bytewise.feed(std::string(1, c));
        check(bytewise.state() == HttpRequest::State::Complete, "逐字节请求失败"); ++checks;
        HttpRequest large;
        const auto largeWire = post(std::string(16000, 'x'));
        for (std::size_t i = 0; i < largeWire.size(); i += 733) large.feed(largeWire.substr(i, 733));
        check(large.state() == HttpRequest::State::Complete && large.body().size() == 16000, "大于 4096 字节正文失败"); ++checks;
        HttpRequest largeHeader;
        check(largeHeader.feed("GET / HTTP/1.1\r\nHost: a\r\nX-Test: " + std::string(6000, 'x') + "\r\n\r\n") == HttpRequest::State::Complete, "分段大请求头失败"); ++checks;
        const std::string prefix = "POST / HTTP/1.1\r\nHost: localhost\r\n";
        const std::vector<std::pair<std::string, int>> failures = {
            {prefix + "Content-Length: -1\r\n\r\n", 400},
            {prefix + "Content-Length: +1\r\n\r\nx", 400},
            {prefix + "Content-Length: x\r\n\r\n", 400},
            {prefix + "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx", 400},
            {prefix + "Content-Length: 999999999999999999999999999\r\n\r\n", 413},
            {prefix + "Content-Length: 2000000\r\n\r\n", 413},
            {prefix + "Transfer-Encoding: chunked\r\n\r\n", 501},
            {prefix + "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400},
            {prefix + "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n", 417},
            {prefix + "\r\n", 411},
            {prefix + "Content-Length: 1\r\n\r\nxx", 400},
            {"GET / HTTP/1.0\r\nHost: a\r\n\r\n", 505},
            {"PUT / HTTP/1.1\r\nHost: a\r\n\r\n", 405},
            {"GET / HTTP/1.1\r\n\r\n", 400},
            {"GET / HTTP/1.1\r\n Host: a\r\n\r\n", 400},
            {"GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(17000, 'x') + "\r\n\r\n", 431},
            {"GET / HTTP/1.1\r\nHost: a\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        };
        for (const auto& failure : failures) {
            HttpRequest request;
            check(request.feed(failure.first) == HttpRequest::State::Failed, "坏请求被接受");
            check(request.errorStatus() == failure.second, "错误状态码不符"); ++checks;
        }
        HttpRequest incomplete;
        incomplete.feed(prefix + "Content-Length: 10\r\n\r\n123");
        check(incomplete.endInput() == HttpRequest::State::Failed && incomplete.errorStatus() == 400, "断连没有失败"); ++checks;
        HttpRequest extra;
        extra.feed("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
        check(extra.feed("x") == HttpRequest::State::Failed, "完成后仍接受额外字节"); ++checks;
        std::string file("\r\n\0binary", 9);
        const auto multipart = "--abc\r\nContent-Disposition: form-data; name=\"file\"; filename=\"image.png\"\r\nContent-Type: image/png\r\n\r\n" + file +
                               "\r\n--abc\r\nContent-Disposition: form-data; name=\"description\"\r\n\r\n描述\r\n--abc--\r\n";
        HttpRequest upload;
        upload.feed(post(multipart, "multipart/form-data; boundary=\"abc\""));
        const auto parsed = parseUpload(upload);
        check(parsed.bytes == file && parsed.filename == "image.png" && parsed.description == "描述", "multipart 改写了二进制数据"); ++checks;
        std::mt19937 random(20261015);
        for (int sample = 0; sample < 5000; ++sample) {
            HttpRequest request;
            const auto length = random() % 512;
            std::string data;
            for (std::size_t i = 0; i < length; ++i) data += static_cast<char>(random() % 256);
            for (std::size_t i = 0; i < data.size(); i += 7) request.feed(data.substr(i, 7));
            request.endInput();
            check(request.state() == HttpRequest::State::Failed || request.state() == HttpRequest::State::Complete, "随机输入未终止");
            ++checks;
        }
        check(HttpResponse(200, "hello").toString().find("Content-Length: 5\r\n") != std::string::npos, "响应长度缺失"); ++checks;
        std::cout << "PASS parser checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
