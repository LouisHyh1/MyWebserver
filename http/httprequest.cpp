#include "httprequest.h"
using namespace std;

// 定义一个无序集合（基于哈希表），存储了服务器默认支持的网页路由路径。
const unordered_set<string> HttpRequest::DEFAULT_HTML {
    "/index", "/register", "/login", "/welcome", "/video", "/picture"
};

// 定义一个哈希映射，将特定的HTML页面映射为特定的整数标志（Tag）。
const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
    {"/register.html", 0}, {"/login.html", 1}
};

// 清空当前请求对象的所有数据，恢复到初始状态。
// 在高并发服务器中，频繁地 `new` 和 `delete` HttpRequest 对象会造成极大的内存碎片和性能开销。
// 通常的做法是连接建立时分配一个对象
// 每次处理完一个完整的HTTP请求后，调用 `Init()` 清空脏数据，用于解析下一个请求
void HttpRequest::Init() {
    // 清空 HTTP方法（GET/POST）、请求路径、HTTP版本号和请求体。
    method_  = path_ = version_ = body_ = "";
    // 将状态机重置为第一个状态：正在解析请求行。
    state_ = REQUEST_LINE;
    // 清空存储请求头键值对的哈希表。
    header_.clear();
    // 清空存储POST表单数据的哈希表。
    post_.clear();
}

// 判断客户端是否希望与服务器保持TCP长连接。
bool HttpRequest::IsKeepAlive() const {
    auto it = header_.find("Connection");
    if (it != header_.end()) {
        // 如果存在，并且它的值是 "keep-alive"，同时 HTTP 版本是 "1.1"，则返回 `true`。
        return it->second == "keep-alive" && version_ == "1.1";
    }
    // 返回 `false`，表示处理完请求后可以直接断开TCP连接。
    return false;
}

// 从底层的字节缓冲区 `buff` 中读取数据，并将其解析为结构化的HTTP请求。
bool HttpRequest::parse(Buffer& buff) {
    // 定义 HTTP 协议标准的换行符 `\r\n` (回车+换行)。
    const char CRLF[] = "\r\n";
    // 获取当前缓冲区里有多少未读字节。如果 <=0，说明没数据，直接返回 `false` 解析失败/未就绪。
    if (buff.ReadableBytes() <= 0) {
        return false;
    }
    // 只要缓冲区里还有数据，并且状态机还没到达 `FINISH`（解析完成）状态，就一直循环解析。
    while (buff.ReadableBytes() && state_ != FINISH) {
        // 在缓冲区的可读起点（`Peek()`）到写起点（`BeginWriteConst()`，即当前数据的结尾）之间
        // 寻找匹配 `\r\n` 的位置。返回值 `lineEnd` 指向 `\r` 的位置。
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        // 利用迭代器构造函数，将 `Peek()` 到 `lineEnd` 之间的字符拷贝出来
        // 形成一个不包含 `\r\n` 的纯净的字符串 `line`。
        string line(buff.Peek(), lineEnd);

        switch (state_) {
        // 状态机：解析请求行（例如：`GET /index HTTP/1.1`）
        case REQUEST_LINE:
            // 调用 `ParseRequestLine_(line)` 解析刚刚提取的一行。
            // 如果解析失败（比如格式不对），直接返回 `false`。
            if (!ParseRequestLine_(line)) {
                return false;
            }
            // 如果成功，调用 `ParsePath_()` 对提取出的 URL 路径进行格式化/转换。
            ParsePath_();
            break;
        case HEADERS:
            // 调用 `ParseHeader_(line)` 解析一行头部字段。
            ParseHeader_(line);
            // 边界处理。如果处理完这一行后，缓冲区剩下的数据不多于2个字节（通常只剩最后一个空行的 `\r\n`）
            // 这意味着没有请求体（比如普通的GET请求），状态机直接跳转到 `FINISH` 结束状态。
            if (buff.ReadableBytes() <= 2) {
                state_ = FINISH;
            }
            break;
        case BODY:
            // 调用 `ParseBody_` 读取请求体。
            ParseBody_(line);
            break;
        default:  // 防止状态枚举出现意外值的安全防御。
            break;
        }
        // 如果 `search` 没有找到 `\r\n`，`lineEnd` 会返回搜索范围的末尾。
        // 这意味着这行数据还没接收完整（半包）
        // 跳出循环，等待网络层下一次接收到更多数据后再继续解析。
        if (lineEnd == buff.BeginWrite()) break;
        // 如果成功提取了一行，将缓冲区的“读指针”向后移动
        // 跳过刚刚读取的这行数据以及末尾的 `\r\n`（共2个字节），为下一次循环读取下一行做准备。
        buff.RetrieveUntil(lineEnd + 2);
    }
    // 解析结束后，调用宏 `LOG_DEBUG` 打印日志，记录请求方法、路径和版本。返回 `true` 表示当前缓冲区的解析过程成功。
    // 通过string类对象的成员函数c_str()把string 对象转换成c中的字符串样式。
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

// 将客户端发来的简写路径转换为服务器的实际文件路径。
void HttpRequest::ParsePath_() {
    // 如果请求路径是根目录 `"/"`，直接将其修改为默认首页 `"/index.html"`。
    if (path_ == "/") {
        path_ = "/index.html";
    } 
    else {
        // 否则，遍历全局常量 `DEFAULT_HTML`（即 `/login`, `/register` 等）。
        for (auto& item : DEFAULT_HTML) {
            // 如果发现请求路径在集合中，则给它自动追加 `".html"`（如变成 `/login.html`），并跳出循环。
            if (item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}

// 拆解 `GET /index HTTP/1.1` 这样的字符串。
bool HttpRequest::ParseRequestLine_(const string& line) {
    // 定义正则表达式。
    regex patten("^([^ ]) ([^ ]) HTTP/([^ ])$");
    // 定义一个匹配结果集对象。
    // 它就像是多个抽屉，用来存放刚刚正则表达式里 `()`（捕获组）抓取出来的东西。
    smatch subMatch;

    // 执行严格的全行匹配。
    if (regex_match(line, subMatch, patten)) {
        method_ = subMatch[1];  // 获取第一组匹配到的词（如 "GET"），赋给 `method_`。
        path_ = subMatch[2];  // （如 "/index"）赋给 `path_`。
        version_ = subMatch[3];  // （如 "1.1"）赋给 `version_`。
        state_ = HEADERS;  // 状态机跃迁。请求行解析完毕，告诉外部大循环：下一行开始解析请求头了！
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

// 解析如 `Connection: keep-alive` 或 `Host: localhost:8080` 这样的键值对。
void HttpRequest::ParseHeader_(const string& line) {
    // `^([^:]*)` 捕获组1：匹配冒号 `:` 之前的任意字符（作为Key）。
    // `: ?` 匹配一个冒号，后面跟着0个或1个空格。
    // `(.*)$` 捕获组2：匹配直到行尾的所有剩余字符（作为Value）。
    regex patten("([^:]*): ?(.*)$");
    smatch subMatch;

    if (regex_match(line, subMatch, patten)) {
        // 如果正则匹配成功
        // 将 `subMatch[1]` (Key) 和 `subMatch[2]` (Value) 存入类成员变量 `header_`
        header_[subMatch[1]] = subMatch[2];
    } else {
        // 一旦匹配失败，说明请求头已经读完了
        // 状态机跃迁：`state_ = BODY;`，告诉主循环下一步该读取请求体了。
        state_ = BODY;
    }
}

// 读取请求体内容（通常只有 POST 登录/注册等提交表单时才会有）。
void HttpRequest::ParseBody_(const string& line) {
    // 将传入的字符串赋给成员变量 `body_`。
    body_ = line;  
    // 判断 `method_` 是否为 `POST`，如果是，则对 `body_` 进行拆解
    ParsePost_();
    // 状态机跃迁。整个HTTP请求已经彻底解析完毕，状态置为 `FINISH`
    state_ = FINISH;
    // 打印请求体的内容和长度，用于调试。
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

// 将十六进制字符转为对应的十进制数值。
int HttpRequest::ConverHex(char ch) {
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch;
}