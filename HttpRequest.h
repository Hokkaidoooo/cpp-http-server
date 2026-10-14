#pragma once
#include <string>
#include <unordered_map>
#include <sstream>

class HttpRequest {
public:
    enum Method {
        GET, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH, UNKNOWN
    };
    enum ParseState {
        REQUEST_LINE, HEADERS, BODY, FINISH
    };

    HttpRequest() : method(UNKNOWN), state(REQUEST_LINE) {}

    bool parse(const std::string& request) {
        std::istringstream iss(request);
        std::string line;
        bool result = true;

        while (std::getline(iss, line) && line != "\r") {
            if (state == REQUEST_LINE) {
                result = parseRequestLine(line);
            } else if (state == HEADERS) {
                result = parseHeader(line);
            }
            if (!result) {
                break;
            }
        }

        if (method == POST) {
            body = request.substr(request.find("\r\n\r\n") + 4);
            if (getHeader("Content-Type").find("multipart/form-data") != std::string::npos) {
                std::string boundary = getBoundary(getHeader("Content-Type"));
                parseMultipartFormData(boundary);
            }
        }

        return result;
    }

    std::unordered_map<std::string, std::string> parseFormBody() const {
        std::unordered_map<std::string, std::string> params;
        if (method != POST) return params;

        std::istringstream stream(body);
        std::string pair;

        while (std::getline(stream, pair, '&')) {
            std::size_t pos = pair.find('=');
            if (pos == std::string::npos) continue;
            std::string key = pair.substr(0, pos);
            std::string value = pair.substr(pos + 1);
            params[key] = value;
        }

        return params;
    }

    std::string getMethodString() const {
        switch (method) {
            case GET: return "GET";
            case POST: return "POST";
            default: return "UNKNOWN";
        }
    }

    const std::string& getPath() const {
        return path;
    }

    std::string getHeader(const std::string& key) const {
        auto it = headers.find(key);
        if (it != headers.end()) {
            return it->second;
        }
        return "";
    }

    std::string getFormField(const std::string& fieldName) const {
        auto it = formFields.find(fieldName);
        if (it != formFields.end()) {
            return it->second;
        }
        return "";
    }

    std::string getFileContent(const std::string& fieldName) const {
        auto it = fileContents.find(fieldName);
        if (it != fileContents.end()) {
            return it->second;
        }
        return "";
    }

    // 新增一个方法用于获取文件名
    std::string getFileName(const std::string& fieldName) const {
        auto it = fileNames.find(fieldName);
        if (it != fileNames.end()) {
            return it->second;
        }
        return "";
    }

private:
    bool parseRequestLine(const std::string& line) {
        std::istringstream iss(line);
        std::string methodStr;
        iss >> methodStr;
        if (methodStr == "GET") method = GET;
        else if (methodStr == "POST") method = POST;
        else method = UNKNOWN;
        iss >> path;
        iss >> version;
        state = HEADERS;
        return true;
    }

    bool parseHeader(const std::string& line) {
        size_t pos = line.find(": ");
        if (pos == std::string::npos) return false;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 2);
        headers[key] = value;
        return true;
    }

    // 解析多部分表单数据，这通常用于文件上传请求
    // @param boundary 分界符，用于识别请求主体中的不同部分
    void parseMultipartFormData(const std::string& boundary) {
        size_t pos = 0;
        size_t endPos = body.find(boundary, pos);

        // 循环遍历请求主体中的每个部分
        while (endPos != std::string::npos) {
            std::string part = body.substr(pos, endPos - pos - 2);
            parsePart(part); // 解析单个部分
            pos = endPos + boundary.length() + 2;
            endPos = body.find(boundary, pos);
        }
    }

    // 解析请求主体的单个部分，提取文件内容或表单字段
    // @param part 请求主体中的一个部分
    void parsePart(const std::string& part) {
        std::istringstream iss(part);
        std::string line;
        std::string name;
        std::string filename; // 存储文件名
        bool isFile = false;  // 标记部分是否为文件

        // 逐行读取部分内容，解析出文件名和表单字段名
        while (std::getline(iss, line) && line != "\r") {
            size_t pos = line.find(": ");
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 2);
                if (key == "Content-Disposition") {
                    // 解析表单字段名和文件名
                    size_t namePos = value.find("name=\"");
                    size_t filenamePos = value.find("filename=\"");
                    if (namePos != std::string::npos) {
                        namePos += 6;
                        size_t nameEnd = value.find("\"", namePos);
                        name = value.substr(namePos, nameEnd - namePos);
                    }
                    if (filenamePos != std::string::npos) {
                        filenamePos += 10;
                        size_t filenameEnd = value.find("\"", filenamePos);
                        filename = value.substr(filenamePos, filenameEnd - filenamePos);
                        isFile = true;
                    }
                }
            }
        }

        // 根据是否为文件来处理提取的内容
        std::string content = part.substr(iss.tellg());
        content.erase(0, content.find_first_not_of("\r\n"));
        if (isFile) {
            fileContents[name] = content; // 存储文件内容
            fileNames[name] = filename;    // 存储文件名
        } else {
            formFields[name] = content;    // 存储表单字段值
        }
    }

    std::string getBoundary(const std::string& contentType) const {
        size_t pos = contentType.find("boundary=");
        if (pos != std::string::npos) {
            pos += 9; // 跳过 "boundary="
            return "--" + contentType.substr(pos);
        }
        return "";
    }

    Method method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    ParseState state;
    std::string body;

    // 新增的成员变量
    std::unordered_map<std::string, std::string> fileNames;
    std::unordered_map<std::string, std::string> formFields;
    std::unordered_map<std::string, std::string> fileContents;
};
