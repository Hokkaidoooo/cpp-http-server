#pragma once
#include "Auth.h"
#include "Multipart.h"
#include "Posix.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <png.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <vector>

struct ImageRecord {
    std::string id, filename, description, bytes;
    std::size_t byteSize = 0;
};

class ImageStorage {
public:
    explicit ImageStorage(const Config& config) : config_(config) {
        std::filesystem::create_directories(config.storageRoot);
        root_ = UniqueFd(::open(config.storageRoot.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root_.get() < 0) throw systemFailure("打开上传目录失败");
    }
    ImageRecord store(const std::string& owner, const Upload& upload) {
        validate(upload);
        ImageRecord result{stableIdentifier(upload.filename), upload.filename, upload.description, "", upload.bytes.size()};
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto folder = ownerDirectory(owner, true);
        const auto existing = names(folder.get());
        const auto finalName = result.id + ".asset";
        if (std::find(existing.begin(), existing.end(), finalName) != existing.end()) throw HttpError(409, "同名文件已存在，请更换文件名");
        if (existing.size() >= config_.maxImagesPerUser) throw HttpError(413, "图片数量已达上限");
        const auto temporaryName = ".upload-" + randomToken() + ".part";
        UniqueFd output(::openat(folder.get(), temporaryName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (output.get() < 0) throw systemFailure("创建临时图片失败");
        PendingFile pending(folder.get(), temporaryName);
        std::string data = "CPIMG001";
        appendInteger(data, upload.filename.size());
        appendInteger(data, upload.description.size());
        appendInteger(data, upload.bytes.size());
        data += upload.filename; data += upload.description; data += upload.bytes;
        writeAll(output.get(), data);
        if (::fsync(output.get()) < 0) throw systemFailure("同步图片记录失败");
        if (::close(output.release()) < 0) throw systemFailure("关闭图片记录失败");
        // 文件内容与元信息一次提交，RENAME_NOREPLACE 同时解决并发覆盖和半文件可见性。
        constexpr unsigned int noReplace = 1;
        if (::syscall(SYS_renameat2, folder.get(), temporaryName.c_str(), folder.get(), finalName.c_str(), noReplace) < 0) {
            if (errno == EEXIST) throw HttpError(409, "同名文件已存在，请更换文件名");
            throw systemFailure("原子发布图片失败");
        }
        pending.committed = true;
        if (::fsync(folder.get()) < 0) throw systemFailure("同步图片目录失败（文件已完整发布）");
        return result;
    }
    std::vector<ImageRecord> list(const std::string& owner) const {
        auto folder = ownerDirectory(owner, false);
        if (folder.get() < 0) return {};
        std::vector<ImageRecord> result;
        for (const auto& name : names(folder.get())) result.push_back(readRecord(folder.get(), name, false));
        return result;
    }
    ImageRecord read(const std::string& owner, const std::string& id) const {
        if (!validId(id)) throw HttpError(400, "图片标识必须是 64 位小写十六进制字符");
        auto folder = ownerDirectory(owner, false);
        if (folder.get() < 0) throw HttpError(404, "图片不存在");
        return readRecord(folder.get(), id + ".asset", true);
    }
private:
    struct PendingFile {
        int directory;
        std::string name;
        bool committed = false;
        PendingFile(int directory, std::string name) : directory(directory), name(std::move(name)) {}
        ~PendingFile() {
            if (!committed && ::unlinkat(directory, name.c_str(), 0) < 0)
                std::cerr << "ERROR 临时图片清理失败 errno=" << errno << '\n';
        }
    };
    static bool validId(const std::string& id) {
        if (id.size() != 64) return false;
        return std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    static bool validUtf8(const std::string& text) {
        std::size_t i = 0;
        while (i < text.size()) {
            const auto first = static_cast<unsigned char>(text[i++]);
            if (first < 128) { if (first < 32 || first == 127) return false; continue; }
            unsigned int count = 0, value = 0, minimum = 0;
            if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 0x07; minimum = 0x10000; }
            else return false;
            if (i + count > text.size()) return false;
            for (unsigned int n = 0; n < count; ++n) {
                const auto c = static_cast<unsigned char>(text[i++]);
                if ((c & 0xc0) != 0x80) return false;
                value = (value << 6) | (c & 0x3f);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        }
        return true;
    }
    void validate(const Upload& upload) const {
        if (upload.bytes.empty() || upload.bytes.size() > config_.maxUploadBytes) throw HttpError(413, "图片为空或超过大小限制");
        const auto& name = upload.filename;
        if (name.empty() || name.size() > 120 || name.front() == '.' || name.find("..") != std::string::npos)
            throw HttpError(400, "文件名非法");
        for (const unsigned char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                throw HttpError(400, "文件名只能使用英文字母、数字、点、下划线和连字符");
        if (name.size() < 5 || lowerAscii(name.substr(name.size() - 4)) != ".png" || upload.contentType != "image/png")
            throw HttpError(415, "只接受 PNG 图片");
        if (upload.description.size() > 1024 || !validUtf8(upload.description)) throw HttpError(400, "描述须为不含控制字符的 UTF-8 文本，最多 1024 字节");
        validatePng(upload.bytes);
    }
    void validatePng(const std::string& bytes) const {
        if (bytes.size() < 20 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8)) throw HttpError(415, "PNG 文件头无效");
        std::size_t offset = 8;
        bool ended = false;
        while (offset + 12 <= bytes.size()) {
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
            const std::size_t length = (std::size_t(p[0]) << 24) | (std::size_t(p[1]) << 16) | (std::size_t(p[2]) << 8) | p[3];
            if (length > bytes.size() - offset - 12) throw HttpError(415, "PNG 数据块越界");
            const auto kind = bytes.substr(offset + 4, 4);
            offset += length + 12;
            if (kind == "IEND") { ended = length == 0 && offset == bytes.size(); break; }
        }
        if (!ended) throw HttpError(415, "PNG 结束块缺失或包含尾随内容");
        png_image png{};
        png.version = PNG_IMAGE_VERSION;
        struct PngCleanup { png_image* image; ~PngCleanup() { png_image_free(image); } } cleanup{&png};
        if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) throw HttpError(415, "PNG 解析失败");
        if (!png.width || !png.height || png.width > config_.maxImagePixels / png.height) throw HttpError(413, "PNG 像素数量超过限制");
        png.format = PNG_FORMAT_RGBA;
        std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(png));
        if (!png_image_finish_read(&png, nullptr, pixels.data(), 0, nullptr)) throw HttpError(415, "PNG 像素数据无效");
    }
    UniqueFd ownerDirectory(const std::string& owner, bool create) const {
        const auto directory = stableIdentifier(owner);
        if (create && ::mkdirat(root_.get(), directory.c_str(), 0700) < 0 && errno != EEXIST) throw systemFailure("创建用户图片目录失败");
        const int fd = ::openat(root_.get(), directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && !create && errno == ENOENT) return UniqueFd{};
        if (fd < 0) throw systemFailure("打开用户图片目录失败");
        return UniqueFd(fd);
    }
    std::vector<std::string> names(int directory) const {
        UniqueFd copy(::dup(directory));
        if (copy.get() < 0) throw systemFailure("复制目录句柄失败");
        DIR* raw = ::fdopendir(copy.get());
        if (!raw) throw systemFailure("读取图片目录失败");
        copy.release();
        std::unique_ptr<DIR, int(*)(DIR*)> entries(raw, ::closedir);
        std::vector<std::string> result;
        for (;;) {
            errno = 0;
            const auto* entry = ::readdir(entries.get());
            if (!entry) { if (errno) throw systemFailure("遍历图片目录失败"); break; }
            const std::string name = entry->d_name;
            if (name == "." || name == ".." || (name.rfind(".upload-", 0) == 0 && name.size() == 77 &&
                validId(name.substr(8, 64)) && name.substr(72) == ".part")) continue;
            if (name.size() != 70 || name.substr(64) != ".asset" || !validId(name.substr(0, 64)))
                throw std::runtime_error("图片目录含有无法识别的记录，未删除该文件");
            result.push_back(name);
            if (result.size() > config_.maxImagesPerUser) throw std::runtime_error("已有图片数量超过当前配置上限");
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    static void appendInteger(std::string& data, std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) data += static_cast<char>((value >> shift) & 255);
    }
    static std::uint64_t readInteger(const std::string& data, std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t i = offset; i < offset + 8; ++i) value = (value << 8) | static_cast<unsigned char>(data[i]);
        return value;
    }
    static std::string readExact(int fd, std::size_t length) {
        std::string result(length, '\0');
        std::size_t position = 0;
        while (position < length) {
            const auto count = ::read(fd, &result[position], length - position);
            if (count > 0) position += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR) continue;
            else if (count == 0) throw std::runtime_error("图片记录被截断");
            else throw systemFailure("读取图片记录失败");
        }
        return result;
    }
    static void writeAll(int fd, const std::string& data) {
        std::size_t position = 0;
        while (position < data.size()) {
            const auto count = ::write(fd, data.data() + position, data.size() - position);
            if (count > 0) position += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR) continue;
            else throw systemFailure("写入图片记录失败");
        }
    }
    ImageRecord readRecord(int directory, const std::string& name, bool withBytes) const {
        UniqueFd input(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (input.get() < 0) { if (errno == ENOENT) throw HttpError(404, "图片不存在"); throw systemFailure("打开图片记录失败"); }
        struct stat info{};
        if (::fstat(input.get(), &info) < 0) throw systemFailure("读取图片记录属性失败");
        if (!S_ISREG(info.st_mode)) throw std::runtime_error("图片记录不是普通文件");
        const auto header = readExact(input.get(), 32);
        const auto nameSize = readInteger(header, 8), descriptionSize = readInteger(header, 16), byteSize = readInteger(header, 24);
        if (header.substr(0, 8) != "CPIMG001" || nameSize == 0 || nameSize > 120 || descriptionSize > 1024 ||
            byteSize == 0 || byteSize > config_.maxUploadBytes || info.st_size != static_cast<off_t>(32 + nameSize + descriptionSize + byteSize))
            throw std::runtime_error("图片记录格式或长度错误");
        ImageRecord result;
        result.id = name.substr(0, 64); result.filename = readExact(input.get(), nameSize);
        result.description = readExact(input.get(), descriptionSize); result.byteSize = byteSize;
        if (stableIdentifier(result.filename) != result.id || !validUtf8(result.description)) throw std::runtime_error("图片元信息损坏");
        if (withBytes) result.bytes = readExact(input.get(), byteSize);
        return result;
    }
    const Config& config_;
    UniqueFd root_;
    std::mutex writeMutex_;
};
