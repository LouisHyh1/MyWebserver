#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex>
#include <errno.h>
#include <mysql/mysql.h>
#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/sqlconnRAII.h"

class HttpRequest {
public:
    // 解析状态枚举（状态机核心）
    enum PARSE_STATE {
        REQUEST_LINE,   // 一开始状态是 `REQUEST_LINE`
        HEADERS,  // 读完第一行自动切换为 `HEADERS`
        BODY,  // 遇到空行自动切换为 `BODY`
        FINISH,  // 全读完变成 `FINISH`
    };

    // HTTP 响应码枚举
    enum HTTP_CODE {
        NO_REQUEST = 0,  // 请求还不完整，需要继续等客户端发数据。
        GET_REQUEST,  // 解析成功，是个好的请求！
        BAD_REQUEST,  // 客户端发送的 HTTP 请求报文有语法错误
        NO_RESOURSE,  // 找不到请求的资源。
        FORBIDDENT_REQUEST,  // 权限不足被拒绝。
        FILE_REQUEST,  // 获取文件成功。
        INTERNAL_ERROR,  // 服务器内部错误。
        CLOSED_CONNECTION,  // 连接已关闭。表示客户端主动断开了 TCP 连接
    };

    // 当这个对象被创建时，立刻调用里面的 `Init()` 方法。
    // 为什么不直接在这里初始化变量？因为在 Web 服务器中，往往使用 `Keep-Alive`（长连接），
    // 即一个 TCP 连接不断开，能发几十个 HTTP 请求。
    // 为了避免重复创建和销毁对象带来性能损耗，
    // 每次处理完一个请求，直接调用 `Init()` 把旧数据清空，对象就能立刻重用给下一个请求
    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    // 将所有状态、字符串、哈希表清空归零。
    void Init();
    // 解析函数，传入那个装着网络字节的 `Buffer` 仓库的引用。
    // 它会在内部跑一个 `while` 循环，根据刚才讲的状态机，一行行地吃掉 `buff` 里的字符串
    // 然后返回 `true`（解析成功）或 `false`（解析失败）。
    bool parse(Buffer& buff);

    //  Getter 方法（获取解析结果）
    std::string path() const;  // 获取 HTTP 请求的目标路径（只读）
    std::string& path();  // 获取 HTTP 请求的目标路径的引用（可读写），用于重定向
    std::string method() const;  // 获取 HTTP 请求的方法（包括 "GET"、"POST"、"HEAD" 等）。
    std::string version() const;  // 获取 HTTP 协议的版本号。
    // 从 HTTP POST 请求的请求体中提取对应的值（value）。
    std::string GetPost(const std::string& key) const;
    std::string GetPost(const char* key) const;

    // 判断当前这个 HTTP 请求是否要求长连接
    bool IsKeepAlive() const;

private:
    // 对应枚举里面那三种状态的处理函数。
    // 如果是起始状态，就调 `ParseRequestLine_`（用正则表达式提取 `GET`、`/index`、`HTTP/1.1`）。
    // 如果是请求头状态，就调 `ParseHeader_`（以冒号 `:` 为界，把左边当 Key 右边当 Value 塞进哈希表）。
    // 如果是主体状态，就调 `ParseBody_`（把请求正文存起来）。
    bool ParseRequestLine_(const std::string& line);
    void ParseHeader_(const std::string& line);
    void ParseBody_(const std::string& line);

    // 对解析出来的原始路径进行二次处理或路由映射。
    void ParsePath_();  
    // 解析 POST 请求的总控函数。
    // 当底层状态机把 HTTP 报文的请求行和请求头都解析完，并且发现这是一个 POST 请求时
    // 就会调用这个函数来处理剩余的请求体（Body）。
    void ParsePost_();  
    // 专门用于拆解和解码表单提交的数据字符串。
    void ParseFromUrlencoded_();

    // 验证用户名和密码的函数。
    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);

    // 一个枚举变量，记录当前流水线卡在哪个状态了
    PARSE_STATE state_;
    // 专门用来装切分好的“GET”、“/index.html”、“HTTP/1.1”以及请求的主体内容。
    std::string method_, path_, version_, body_;
    // 用于存储 HTTP 请求的头部信息（Headers）。
    std::unordered_map<std::string, std::string> header_;
    // 用于存储 HTTP POST 请求的表单数据（Body）。
    std::unordered_map<std::string, std::string> post_;

    // 静态常量哈希集合。它在类外面的 `.cpp` 文件里会被初始化为
    // 包含 `"index"`, `"register"`, `"login"` 等字符串的集合。
    // 用来快速判断用户访问的是不是合法的 HTML 页面。
    static const std::unordered_set<std::string> DEFAULT_HTML;
    // 静态常量哈希表。把页面名称映射成数字编号
    // 比如 `["register.html"] = 0`, `["login.html"] = 1`
    // 方便底层的 C++ 用 `switch-case` 语法快速做网页跳转逻辑。
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    // 把浏览器的 `%20`、`%2F` 中的 16 进制字符（`'2'`, `'A'`, `'F'`），转化为真正的数字
    static int ConverHex(char ch);
};  

#endif