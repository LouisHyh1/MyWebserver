#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <fcntl.h>       // open
#include <unistd.h>      // close
#include <sys/stat.h>    // stat
#include <sys/mman.h>    // mmap, munmap
#include "../buffer/buffer.h"
#include "../log/log.h"

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    // 初始化响应对象。
    // `srcDir`：服务器托管静态资源的根目录路径（如 `/var/www/html`）。
    // `path`：客户端请求的相对路径（如 `/index.html`）。
    // `isKeepAlive`：是否保持长连接（由 HttpRequest 传入）。
    // `code`：HTTP状态码
    void Init(const std::string& srcDir, std::string& path, bool isKeepAlive = false, int code = -1);

    // 核心拼装函数。
    // 它会依次调用私有函数，生成状态行、响应头，存入 `buff` 中；并找到对应文件，完成 `mmap` 映射。
    void MakeResponse(Buffer& buff);

    // 调用 `munmap` 释放由 `mmap` 申请的内存映射区域。
    // 当文件发送完毕，或者连接断开时，必须释放映射区，否则系统的虚拟内存地址空间会被耗尽。
    void UnmapFile();

    char* File();  // 返回内存映射文件的首地址（即响应体内容的指针）
    size_t FileLen() const;  // 返回文件的大小。

    // 如果找不到对应的错误页面（比如 404.html 丢失了）
    // 直接在 Buffer 里硬编码写一段短小的文本作为 HTML 响应体，告知用户错误信息。
    void ErrorContent(Buffer& buff, std::string message);
    // 获取当前响应的状态码（如 200, 404）。
    int Code() const { return code_; }

private:
    
    void AddStateLine_(Buffer& buff);  // 添加 HTTP 状态行
    void AddHeader_(Buffer& buff);     // 添加 HTTP 响应头
    void AddContent_(Buffer& buff);    // 添加 HTTP 响应体/正文

    // 处理错误页面
    // 根据当前的 `code_`（如 404），修改 `path_` 的值，将其指向对应的错误页面路径（如 `/404.html`）。
    void ErrorHtml_();
    // 通过解析 `path_` 中的文件后缀名（如 `.css`, `.jpg`），返回对应的文件类型
    std::string GetFileType_();

    int code_;            // HTTP响应状态码
    bool isKeepAlive_;    // 是否长连接

    std::string path_;    // 请求文件路径。
    std::string srcDir_;  // 资源根目录。

    char* mmFile_;  // 通过 `mmap` 映射出来的内存首地址。
    struct stat mmFileStat_;  // Linux系统的结构体，保存了文件的详细信息

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;  // 文件后缀 -> MIME 类型的映射表
    static const std::unordered_map<int, std::string> CODE_STATUS;  // 状态码 -> 状态描述的映射表
    static const std::unordered_map<int, std::string> CODE_PATH;  // 状态码 -> 错误页面的映射表
};

#endif