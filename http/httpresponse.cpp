#include "httpresponse.h"
using namespace std;

// 文件后缀 -> MIME 类型的映射表
const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
    { ".html",  "text/html" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain" },
    { ".rtf",   "application/rtf" },
    { ".pdf",   "application/pdf" },
    { ".word",  "application/nsword" },
    { ".png",   "image/png" },
    { ".gif",   "image/gif" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".au",    "audio/basic" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".avi",   "video/x-msvideo" },
    { ".gz",    "application/x-gzip" },
    { ".tar",   "application/x-tar" },
    { ".css",   "text/css "},
    { ".js",    "text/javascript "},
};

// 状态码 -> 状态描述的映射表
const unordered_map<int, string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
};

// 状态码 -> 错误页面的映射表
const unordered_map<int, string> HttpResponse::CODE_PATH = {
    { 400, "/400.html" },
    { 403, "/403.html" },
    { 404, "/404.html" },
};

HttpResponse::HttpResponse() {
    code_ = -1;                 // 状态码初始化为 -1，表示未知状态
    path_ = srcDir_ = "";       // 请求的相对路径和服务器资源根目录均为空
    isKeepAlive_ = false;       // 默认不保持长连接
    mmFile_ = nullptr;          // 内存映射的指针置空（非常重要，防止野指针）
    mmFileStat_ = { 0 };        // 文件状态结构体清零
}

HttpResponse::~HttpResponse() {
    UnmapFile();  // 在对象销毁时，自动调用 `UnmapFile()` 解除对文件的内存映射。
}

void HttpResponse::Init(const string& srcDir, string& path, bool isKeepAlive, int code) {
    assert(srcDir != "");       // 断言：服务器资源根目录不能为空，否则直接终止程序
    if (mmFile_)  UnmapFile();  // 如果当前对象之前映射过文件，先解除映射
    code_ = code;               // 设置 HTTP 状态码（通常默认传入 -1）
    isKeepAlive_ = isKeepAlive; // 设置是否 Keep-Alive
    path_ = path;               // 设置请求的相对路径（如 "/index.html"）
    srcDir_ = srcDir;           // 设置服务器资源绝对路径（如 "/home/user/web/resources"）
    mmFile_ = nullptr;          // 指针重置
    mmFileStat_ = { 0 };        // 状态重置
}

// 核心拼装函数。
// 它会依次调用私有函数，生成状态行、响应头，存入 `buff` 中；并找到对应文件，完成 `mmap` 映射。
void HttpResponse::MakeResponse(Buffer& buff) {
    // `srcDir_`：服务器存放网页文件的绝对根目录
    // `path_`：浏览器请求的相对路径
    // `srcDir_ + path_`：拼起来就是硬盘上的真实绝对路径 

    // stat():根据传入的文件路径，去硬盘上把这个文件的所有属性（大小、创建时间、权限、是不是文件夹等）查出来。
    // `(srcDir_ + path_).data()`：把 C++ 的字符串转成 C 语言的字符串格式，传给 `stat` 函数。
    // &mmFileStat_:输出参数。`stat` 函数查到文件信息后，会把结果“填”进这个变量里。
    // 如果 `stat` 函数返回小于 0 的数字，说明系统底层报错了（通常是因为文件根本不存在）。

    // `mmFileStat_.st_mode`：刚才 `stat` 函数不仅查了文件在不在，还把文件的状态信息存到了 `st_mode` 这个变量里。
    // `S_ISDIR()`：这是 Linux 的一个宏函数。用来判断当前路径是不是一个文件夹（Directory）。
    // 为什么要判断？因为服务器不能把整个文件夹直接丢给浏览器，只能给具体的文件。
    if (stat((srcDir_ + path_).data(), &mmFileStat_) < 0 || S_ISDIR(mmFileStat_.st_mode)) {
        code_ = 404;
    }
    // 如果代码走到这里，说明文件存在，且不是文件夹。接下来要检查权限。
    // `S_IROTH`：“其他用户的读取权限”。它的底层就是一个特定的二进制数字
    // 如果按位与结果大于 0，说明这个文件允许其他用户读取。
    else if (!(mmFileStat_.st_mode & S_IROTH)) {
        code_ = 403;  // 文件在，但我（服务器）没有权限去读它，所以我不能把它发给你。
    }
    // 状态码还是 `-1`，说明文件存在、是普通文件、且有权限读取。
    else if (code_ == -1) {
        code_ = 200;
    }

    // 依次生成 HTTP 响应报文的各个部分
    ErrorHtml_();           // 添加错误路径
    AddStateLine_(buff);    // 添加 HTTP 状态行
    AddHeader_(buff);       // 添加 HTTP 响应头
    AddContent_(buff);      // 添加 HTTP 响应体/正文
}

char* HttpResponse::File() {
    return mmFile_;  // 返回内存映射区的文件数据指针
}

size_t HttpResponse::FileLen() const {
    return mmFileStat_.st_size;  // 返回文件大小
}

// 处理错误页面
void HttpResponse::ErrorHtml_() {
    auto it = CODE_PATH.find(code_);
    if (it != CODE_PATH.end()) {  // 如果状态码在错误字典中（比如 404）
        path_ = it->second;  // 把请求路径强行修改为错误页面的路径 (如 "/404.html")
        stat((srcDir_ + path_).data(), &mmFileStat_);  // 重新获取错误页面的文件信息
    }
}

// 添加 HTTP 状态行
void HttpResponse::AddStateLine_(Buffer& buff) {
    string status;
    auto it = CODE_STATUS.find(code_);
    if (it != CODE_STATUS.end()) {  // 查找状态码对应的文字描述
        status = it->second;
    } else { 
        code_ = 400;  // 防御性编程：如果不认识这个状态码，直接算作 400 Bad Request
        status = it->second;
    }
    // 按照 HTTP 协议格式拼接：版本号 状态码 状态描述 \r\n
    buff.Append("HTTP/1.1 " + to_string(code_) + " " + status + "\r\n");
}

// 添加 HTTP 响应头
void HttpResponse::AddHeader_(Buffer& buff) {
    buff.Append("Connection: "); // 准备写入连接状态头
    if (isKeepAlive_) { // 如果客户端请求 Keep-Alive
        buff.Append("keep-alive\r\n"); // 告诉客户端服务器支持保持连接
        // 告诉客户端长连接的策略：最多保持 6 个请求，超时时间 120 秒
        buff.Append("keep-alive: max=6, timeout=120\r\n"); 
    } else {
        buff.Append("close\r\n"); // 否则告诉客户端请求完就关闭 TCP 连接
    }
    // 写入 Content-type 头，通过 GetFileType_() 动态计算类型
    buff.Append("Content-type: " + GetFileType_() + "\r\n");
}

// 添加 HTTP 响应体/正文
void HttpResponse::AddContent_(Buffer& buff) {
    // 1. 打开文件：拿到文件的“操作凭证”
    int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
    if (srcFd < 0) {
        // 兜底防御：如果在 stat 检查后，文件突然被删了，open 就会失败
        ErrorContent(buff, "File NotFound!");
        return;
    }

    LOG_DEBUG("file path %s", (srcDir_ + path_).data()); // 打印日志

    // 2. 核心操作：mmap 内存映射
    int* mmRet = static_cast<int*>(mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0));
    // 检查 mmap 是否成功，如果失败会返回特殊值 MAP_FAILED（即 (void*)-1）
    if (*mmRet == -1) {
        ErrorContent(buff, "File NotFound!");
        return;
    }
    // 将映射成功的内存起始地址，保存到类的成员变量 mmFile_ 中
    mmFile_ = reinterpret_cast<char*>(mmRet);

    // 3. 极其精妙的一步：关闭文件描述符
    // 因为 mmap 已经在操作系统的内存管理模块中，把物理内存和磁盘文件绑定了。
    // srcFd 只是用来建立绑定的“媒介”。绑定完成后，关掉 srcFd 释放系统资源
    close(srcFd);

    // 4. 写入 HTTP 头部和空行
    buff.Append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}

// 调用 `munmap` 释放由 `mmap` 申请的内存映射区域。
void HttpResponse::UnmapFile() {
    if (mmFile_ != nullptr) {
        // 系统调用：解除从 mmFile_ 开始，长度为 st_size 的内存映射
        munmap(mmFile_, mmFileStat_.st_size);
        // 指针置空，防止悬垂指针（Dangling Pointer）引发段错误
        mmFile_ = nullptr;
    } 
}

// 通过解析 `path_` 中的文件后缀名（如 `.css`, `.jpg`），返回对应的文件类型
string HttpResponse::GetFileType_() {
    // 找到路径中最后一个 '.' 的位置
    auto idx = path_.find_last_of('.');
    if (idx == string::npos) {  // 如果没找到 '.'（说明没有后缀名）
        return "text/plain";   // 默认作为纯文本处理
    }
    string suffix = path_.substr(idx);   // 截取后缀名 (如 ".html")
    auto it = SUFFIX_TYPE.find(suffix);  // 去一开始的静态字典中查找
    if (it != SUFFIX_TYPE.end()) {
        return it->second;  // 找到了就返回对应的 MIME 类型
    }
    return "text/plain";  // 找不到对应类型，也作为纯文本处理
}

// 如果找不到对应的错误页面（比如 404.html 丢失了）
// 直接在 Buffer 里硬编码写一段短小的文本作为 HTML 响应体，告知用户错误信息。
void HttpResponse::ErrorContent(Buffer& buff, string message) {
    // `body`：用来装最终拼好的 HTML 网页源代码。
    // `status`：用来装 HTTP 的状态文字（比如 "Not Found" 或 "Forbidden"）。
    string body, status;
    // `<html>` 代表网页开始，`<title>Error</title>` 让浏览器标签页的标题显示为 "Error"。
    body += "<html><title>Error</title>";
    // `<body bgcolor=\"ffffff\">` 给网页设置了一个纯白色的背景（`ffffff` 是白色的颜色代码）。
    // 由于 C++ 字符串里不能直接写双引号，所以用反斜杠 `\"` 进行了转义。
    body += "<body bgcolor=\"ffffff\">";
    auto it = CODE_STATUS.find(code_);
    if (it != CODE_STATUS.end()) {
        status = it->second;
    } else {
        status = "Bad Request";
    }
    // 把数字转成字符串，拼上冒号和状态。结果比如：`"404 : Not Found\n"`
    body += to_string(code_) + " : " + status  + "\n";
    // `<p>` 是 HTML 的段落标签。把外界传进来的具体错误原因（`message` 变量
    // 比如上一节传入的 `"File NotFound!"`）放进去。
    body += "<p>" + message + "</p>";
    // `<hr>`：在网页上画一条水平分割线。
    // `<em>TinyWebServer</em>`：用斜体字署名，告诉用户这是 "TinyWebServer" 报的错。
    // `</body></html>`：完美收尾，HTML 网页结束。
    body += "<hr><em>TinyWebServer</em></body></html>"; // 底部留上服务器的名字

    // 写入 Content-length、空行，最后将这一段 HTML 文本塞进 Buffer 中
    // 在 HTTP 协议中，响应头（Header）和响应体（Body，也就是网页内容）之间，必须用两个回车换行符隔开。
    // 浏览器只要看到 `\r\n\r\n`，就知道头部结束了，后面的全都是网页内容。
    buff.Append("Content-length: " + to_string(body.size()) + "\r\n\r\n");
    buff.Append(body);
}