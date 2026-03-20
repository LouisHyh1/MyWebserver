#include <unistd.h>
#include "server/webserver.h"

int main() {
    // port (端口号) = 5220  trigMode (触发模式) = 3(ET+ET)  timeoutMS (超时时间) = 60000(60秒)
    // OptLinger (优雅关闭) = false  sqlPort (数据库端口) = 3306(MySQL/MariaDB 数据库的默认官方端口)
    // sqlUser (数据库用户名) = "root"  sqlPwd (数据库密码) = "20020522"  dbName (数据库名) = "Louis_db"
    // connPoolNum (数据库连接池数量) = 12  threadNum (线程池数量) = 6(通常设置为 CPU核心数 或 CPU核心数 + 1)
    // openLog (开启日志) = true  logLevel (日志等级) = 1  logQueSize (日志异步队列大小) = 1024
    WebServer server(
        5220, 3, 60000, false,
        3306, "root", "20020522", "Louis_db",
        12, 6, true, 1, 1024
    );
    server.Start();
}