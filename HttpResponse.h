#pragma once
#include "HttpRequest.h"
#include <map>
#include <sstream>

class HttpResponse {
public:
    explicit HttpResponse(int code = 200, std::string body = "") : status_(code), body_(std::move(body)) {}
    void setHeader(const std::string& name, const std::string& value) {
        if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
            throw std::logic_error("响应头包含非法字符");
        const auto key = lowerAscii(name);
        if (key == "content-length" || key == "connection" || key == "transfer-encoding")
            throw std::logic_error("响应传输字段由 HttpResponse 统一生成");
        headers_[key] = value;
    }
    std::string toString() const {
        std::ostringstream result;
        result << "HTTP/1.1 " << status_ << ' ' << reason(status_) << "\r\n"
               << "Content-Length: " << body_.size() << "\r\nConnection: close\r\n"
               << "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
        if (!headers_.count("content-type")) result << "Content-Type: text/plain; charset=utf-8\r\n";
        for (const auto& header : headers_) result << header.first << ": " << header.second << "\r\n";
        return result.str() + "\r\n" + body_;
    }
private:
    static const char* reason(int status) {
        switch (status) {
            case 200: return "OK"; case 201: return "Created"; case 303: return "See Other";
            case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
            case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
            case 409: return "Conflict"; case 411: return "Length Required"; case 413: return "Content Too Large";
            case 415: return "Unsupported Media Type"; case 417: return "Expectation Failed";
            case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
            case 501: return "Not Implemented"; case 503: return "Service Unavailable";
            case 505: return "HTTP Version Not Supported";
            default: throw std::logic_error("未定义的 HTTP 状态码");
        }
    }
    int status_;
    std::string body_;
    std::map<std::string, std::string> headers_;
};
