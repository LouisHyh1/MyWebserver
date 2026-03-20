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
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
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

// 处理 HTTP 的 POST 请求，专门提取表单中的用户名和密码
// 并根据是“登录”还是“注册”去查数据库验证，最后根据验证结果修改请求路径（跳转到成功或失败页面）。
void HttpRequest::ParsePost_() {
    // 1. 判断是否是我们需要处理的 POST 请求
    if (method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        // 调用专门的解析函数，将 `body_` 中的字符串拆解为键值对，存入 `post_` 这个哈希表中
        ParseFromUrlencoded_();
        // 去全局路由表 `DEFAULT_HTML_TAG` 中查找当前请求的路径（比如 `/login.html`）。
        auto it = DEFAULT_HTML_TAG.find(path_);
        if (it != DEFAULT_HTML_TAG.end()) {
            // 如果找到了，取出对应的 `tag`（标志位）。
            int tag = it->second;
            // 打印调试日志。
            LOG_DEBUG("Tag:%d", tag);
            // 安全校验：确保 `tag` 确实是 0 或 1。
            if (tag == 0 || tag == 1) {
                // 将整型 tag 转换为布尔语义，可读性更强。
                bool isLogin = (tag == 1);
                // 从刚才解析好的 `post_` 字典中拿出 `username` 和 `password`，传给数据库校验函数 `UserVerify`。
                if (UserVerify(post_["username"], post_["password"], isLogin)) {
                    // 如果 `UserVerify` 返回 `true`（登录成功 或 注册成功）
                    // 将请求的路径 `path_` 强行篡改为 `"/welcome.html"`。
                    path_ = "/welcome.html";
                } else {
                    // 如果失败（密码错误、用户已存在等），篡改为 `"/error.html"`。
                    path_ = "/error.html";
                }
            }
        }
    }
}

void HttpRequest::ParseFromUrlencoded_() {
    if (body_.size() == 0) return;
    string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;
    for (; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        // 说明之前走过的字符拼成了一个 `Key`（键）。
        case '=':
            // 用 `substr` 从 `j` 开始截取长度为 `i-j` 的字符串，赋给 `key`。
            key = body_.substr(j, i - j);
            // 然后将 `j` 移动到 `i+1`，准备迎接后续的 `Value`。
            j = i + 1;
            break;
        case '+':
            // 在 URL 编码标准中，表单里的空格会被编码成 `+`。
            body_[i] = ' ';
            break;
        case '%':
            // 如果输入了特殊字符（比如中文或标点符号 `!`）
            // 浏览器会把它变成 `%` 加上两个十六进制数字（比如 `!` 变成 `%21`）。
            // 把十六进制转回十进制的 ASCII 码值（比如 `%21` 就是 $2 \times 16 + 1 = 33$，33 恰好是 `!` 的 ASCII 码）。
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            // 把计算出的十进制 ASCII 码拆成十位和个位，覆盖在原来的十六进制字符上。
            body_[i + 1] = num % 10 + '0';
            body_[i + 2] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            // 遇到 `&` 号：说明一个完整的键值对（`Key=Value`）结束了。
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert (j <= i);
    // 最后一个 `value`（也就是 `123`）在 `for` 循环里是不会被切分和保存的！
    // 所以在循环结束后，我们需要做个补救。如果 `j < i`（说明还有剩下的字符没切完）
    // 我们就把剩下的这串字符当作最后一个 `value`，存入 `post_` 字典中。
    if (post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

// 数据库交互与用户鉴权
bool HttpRequest::UserVerify(const string& name, const string& pwd, bool isLogin) {
    // 1. 如果用户名或密码为空，直接拒绝，返回失败。防御性编程。
    if (name == "" || pwd == "") return false;
    // 2. 记录一条日志，方便后台调试。
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    // 3. 声明数据库连接指针。
    MYSQL* sql;
    // 4. 【核心设计】使用 RAII 手法从“数据库连接池”中获取一个连接。
    SqlConnRAII(sql, SqlConnPool::Instance());
    // 5. 断言检查：确保成功拿到了数据库连接，拿不到就直接终止程序（说明服务器数据库挂了）。
    assert(sql != nullptr);

    bool flag = false;   // 默认最终结果为失败
    unsigned int j = 0;  // 列数计数器
    char order[256] = {0};  // SQL 语句缓冲区，初始化清零
    MYSQL_FIELD* fields = nullptr;   // 字段元数据指针
    MYSQL_RES* res = nullptr;   // 查询结果集指针

    // 登录逻辑：默认你失败 (`false`)。除非去数据库查到了你的账号，并且密码匹配，才给你改成 `true`。
    // 注册逻辑：默认你可以注册 (`true`)。除非去数据库一查，发现这个用户名已经被别人占用了，才给你改成 `false`。
    if (!isLogin) flag = true;

    /* 查询用户及密码 */
    // 1. 把 SQL 语句拼接到 order 数组中。LIMIT 1 是优化，只要找到一个匹配的就停止搜索。
    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    LOG_DEBUG("%s", order);

    // 2. mysql_query 执行 SQL 语句。返回 0 代表执行成功，非 0 代表 SQL 语句有语法错误或执行失败。
    if (mysql_query(sql, order)) {
        mysql_free_result(res);  // 释放可能存在的内存
        return false;            // 查询失败，直接退出
    }

    // 3. mysql_store_result 将刚才查询的结果从 MySQL 服务器拉取到本地内存中。
    res = mysql_store_result(sql);

    // 4. 获取列数和列字段信息（这通常规范写法，但在这段代码后面的逻辑中其实没用到）
    // mysql_num_fields() 函数会检查你的查询结果集（存储在 res 变量中），并计算出一共有多少列。
    j = mysql_num_fields(res);
    // mysql_fetch_fields() 函数会返回一个数组（准确地说是 MYSQL_FIELD 结构体的数组）
    // 里面包含了查询结果中每一列的详细定义。它将这个数组的指针赋值给变量 fields。
    fields = mysql_fetch_fields(res);

    // mysql_fetch_row 每次从结果集 res 中抓取一行。由于有 LIMIT 1，这个 while 循环最多只会执行 1 次。
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);

        // row[0] 是 username，row[1] 是 password。取出数据库里存的密码。
        string password{row[1]};
         // 判断当前是登录行为还是注册行为？
        if (isLogin) {
            // 是登录：比对用户输入的密码 (pwd) 和数据库里的密码 (password) 是否一致
            if (pwd == password) {
                flag = true;  // 密码正确！鉴权成功！
            } else {
                flag = false;
                LOG_DEBUG("pwd error!");  // 密码错误！
            }
        } else {
            // 是注册：既然 while 循环进来了，说明在数据库里查到了这个 username！
            // 说明用户名被别人注册过了，所以注册失败。
            flag = false;
            LOG_DEBUG("user used!");
        }
    }

    // 【极其重要】查完数据后，一定要释放结果集占据的内存，否则会导致内存泄漏！
    mysql_free_result(res);

    /* 注册行为 且 用户名未被使用*/
    // 如果是注册(!isLogin)，且 flag 依然是 true (说明上面的 while 循环根本没进去，没查到重名)
    if (!isLogin && flag) {
        LOG_DEBUG("register!");

        // 1. 清空刚才的 SQL 语句缓冲区
        bzero(order, 256);

        // 2. 拼接 INSERT 插入语句，将新用户的账密写入数据库
        snprintf(order, 256, "INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG("%s", order);

        // 3. 执行插入操作
        if (mysql_query(sql, order)) {
            LOG_DEBUG("Insert error!");
            flag = false;
        }

        // 否则，flag保持true不变
    }

    LOG_DEBUG("UserVerify success!!");

    // 返回最终结果：true(登录成功/注册成功) 或 false(密码错误/用户不存在/用户名被抢占/数据库错误)
    return flag;
}

string HttpRequest::path() const { return path_; }
string& HttpRequest::path() { return path_; }
string HttpRequest::method() const { return method_; }
string HttpRequest::version() const { return version_; }

// 从 HTTP POST 请求的请求体中提取对应的值（value）。
string HttpRequest::GetPost(const string& key) const {
    assert(key != "");
    auto it = post_.find(key);
    if (it != post_.end()) {
        return it->second;
    }
    return "";
}

string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    auto it = post_.find(key);
    if (it != post_.end()) {
        return it->second;
    }
    return "";
}
