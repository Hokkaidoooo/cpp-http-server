#pragma once
#include "Auth.h"
#include "HttpResponse.h"
#include "ImageStorage.h"
#include <fstream>
#include <map>
#include <sstream>

inline std::string jsonString(const std::string& value) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

class Router {
public:
    Router(Auth& auth, ImageStorage& storage, const Config& config) : auth_(auth), storage_(storage) {
        for (const auto* name : {"login", "register", "upload"}) {
            const auto file = config.appRoot / "UI" / (std::string(name) + ".html");
            std::ifstream input(file, std::ios::binary);
            if (!input) throw std::runtime_error("无法读取页面：" + file.string());
            std::ostringstream content; content << input.rdbuf();
            if (input.bad()) throw std::runtime_error("页面读取失败：" + file.string());
            pages_.emplace("/" + std::string(name), content.str());
        }
    }
    HttpResponse routeRequest(const HttpRequest& request) {
        const auto& path = request.getPath();
        const auto& method = request.getMethodString();
        if (method == "POST") auth_.requireSameOrigin(request);
        const bool protectedPath = path == "/upload" || path == "/logout" || path == "/images" || path.rfind("/images/", 0) == 0;
        const auto user = protectedPath ? auth_.requireUser(request) : std::string{};
        if (method == "GET") {
            if (path == "/") return HttpResponse(200, "Hello, World!");
            const auto page = pages_.find(path);
            if (page != pages_.end()) {
                HttpResponse response(200, page->second);
                response.setHeader("Content-Type", "text/html; charset=utf-8");
                response.setHeader("Content-Security-Policy", "default-src 'none'; form-action 'self'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'");
                return response;
            }
            if (path == "/images") {
                std::string data = "[";
                for (const auto& image : storage_.list(user)) { if (data.size() > 1) data += ','; data += metadata(image); }
                return json(200, data + ']');
            }
            if (path.rfind("/images/", 0) == 0) {
                const auto image = storage_.read(user, path.substr(8));
                HttpResponse response(200, image.bytes);
                response.setHeader("Content-Type", "image/png");
                response.setHeader("Content-Disposition", "inline; filename=\"" + image.id + ".png\"");
                return response;
            }
        }
        if (method == "POST") {
            if (path == "/register") { auth_.registerUser(request); return HttpResponse(201, "注册成功，请前往 /login 登录。"); }
            if (path == "/login") {
                const auto token = auth_.login(request);
                HttpResponse response(303, "登录成功");
                response.setHeader("Set-Cookie", auth_.cookie(token)); response.setHeader("Location", "/upload"); return response;
            }
            if (path == "/logout") {
                auth_.logout(request);
                HttpResponse response(200, "已退出登录"); response.setHeader("Set-Cookie", auth_.cookie("")); return response;
            }
            if (path == "/upload") return json(201, metadata(storage_.store(user, parseUpload(request))));
        }
        throw HttpError(404, "接口不存在");
    }
private:
    static std::string metadata(const ImageRecord& image) {
        return "{\"id\":" + jsonString(image.id) + ",\"name\":" + jsonString(image.filename) +
               ",\"description\":" + jsonString(image.description) + ",\"bytes\":" + std::to_string(image.byteSize) +
               ",\"url\":" + jsonString("/images/" + image.id) + '}';
    }
    static HttpResponse json(int status, const std::string& data) {
        HttpResponse response(status, data); response.setHeader("Content-Type", "application/json; charset=utf-8"); return response;
    }
    Auth& auth_;
    ImageStorage& storage_;
    std::map<std::string, std::string> pages_;
};
