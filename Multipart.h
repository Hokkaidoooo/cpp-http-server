#pragma once
#include "HttpRequest.h"
#include <map>

struct Upload {
    std::string filename, contentType, description, bytes;
};

inline std::map<std::string, std::string> parameters(const std::string& value, const std::string& expected) {
    const auto first = value.find(';');
    if (lowerAscii(trimOws(value.substr(0, first))) != expected) throw HttpError(415, "表单类型不受支持");
    std::map<std::string, std::string> result;
    if (first == std::string::npos) return result;
    std::size_t cursor = first + 1;
    while (cursor < value.size()) {
        const auto end = value.find(';', cursor);
        const auto part = trimOws(value.substr(cursor, end == std::string::npos ? end : end - cursor));
        const auto equal = part.find('=');
        if (equal == std::string::npos) throw HttpError(400, "表单参数缺少等号");
        const auto name = lowerAscii(trimOws(part.substr(0, equal)));
        auto content = trimOws(part.substr(equal + 1));
        if (!content.empty() && content.front() == '"') {
            if (content.size() < 2 || content.back() != '"') throw HttpError(400, "表单参数引号不完整");
            content = content.substr(1, content.size() - 2);
        }
        if (name.empty() || content.find('"') != std::string::npos || !result.emplace(name, content).second)
            throw HttpError(400, "表单参数非法或重复");
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    return result;
}

inline Upload parseUpload(const HttpRequest& request) {
    const auto type = parameters(request.getHeader("content-type"), "multipart/form-data");
    if (type.size() != 1 || !type.count("boundary")) throw HttpError(400, "multipart 必须具有唯一 boundary");
    const auto boundary = type.at("boundary");
    if (boundary.empty() || boundary.size() > 70) throw HttpError(400, "boundary 长度非法");
    for (const unsigned char c : boundary)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            throw HttpError(400, "boundary 包含不支持的字符");
    const auto delimiter = "--" + boundary;
    const auto& body = request.body();
    if (body.rfind(delimiter + "\r\n", 0) != 0) throw HttpError(400, "multipart 起始边界错误");
    std::size_t cursor = delimiter.size() + 2;
    bool hasFile = false, hasDescription = false;
    Upload upload;
    for (;;) {
        const auto headerEnd = body.find("\r\n\r\n", cursor);
        if (headerEnd == std::string::npos || headerEnd - cursor > 4096) throw HttpError(400, "multipart 部分头错误或过大");
        std::map<std::string, std::string> headers;
        while (cursor < headerEnd) {
            const auto lineEnd = body.find("\r\n", cursor);
            const auto line = body.substr(cursor, lineEnd - cursor);
            const auto colon = line.find(':');
            if (colon == std::string::npos || !headers.emplace(lowerAscii(line.substr(0, colon)), trimOws(line.substr(colon + 1))).second)
                throw HttpError(400, "multipart 部分头缺失或重复");
            cursor = lineEnd + 2;
        }
        if (!headers.count("content-disposition")) throw HttpError(400, "缺少 Content-Disposition");
        const auto disposition = parameters(headers.at("content-disposition"), "form-data");
        if (!disposition.count("name")) throw HttpError(400, "缺少 multipart 字段名");
        cursor = headerEnd + 4;
        const auto next = body.find("\r\n" + delimiter, cursor);
        if (next == std::string::npos) throw HttpError(400, "multipart 未结束");
        const auto name = disposition.at("name");
        const auto content = body.substr(cursor, next - cursor);
        if (name == "file") {
            if (hasFile || disposition.size() != 2 || !disposition.count("filename") ||
                headers.size() != 2 || !headers.count("content-type")) throw HttpError(400, "需要唯一 file 文件部分");
            hasFile = true; upload.filename = disposition.at("filename");
            upload.contentType = lowerAscii(headers.at("content-type")); upload.bytes = content;
        } else if (name == "description") {
            if (hasDescription || disposition.size() != 1 || headers.size() != 1) throw HttpError(400, "description 字段非法或重复");
            hasDescription = true; upload.description = content;
        } else throw HttpError(400, "存在不支持的 multipart 字段");
        cursor = next + 2 + delimiter.size();
        if (body.compare(cursor, 4, "--\r\n") == 0 && cursor + 4 == body.size()) break;
        if (body.compare(cursor, 2, "--") == 0 && cursor + 2 == body.size()) break;
        if (body.compare(cursor, 2, "\r\n") != 0) throw HttpError(400, "multipart 结束边界错误");
        cursor += 2;
    }
    if (!hasFile) throw HttpError(400, "没有上传文件");
    return upload;
}
