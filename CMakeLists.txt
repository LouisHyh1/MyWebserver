# 1. 设置 CMake 的最低版本要求
cmake_minimum_required(VERSION 3.10)

# 2. 设置项目名称和使用的语言 (C++)
project(WebServer CXX)

# 3. 设置 C++ 标准为 C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. 添加全局编译选项 (-O2 -Wall -g)
add_compile_options(-O2 -Wall -g)

# 5. 【替代 mkdir -p bin】
# 设置可执行文件的输出目录到根目录的 bin 文件夹下
# CMake 非常聪明，如果 bin 文件夹不存在，它在编译时会自动帮你创建！
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# 6. 获取所有源文件（注意：由于 CMakeLists 放在根目录，这里去掉了 ../）
file(GLOB SOURCES 
    "code/log/*.cpp"
    "code/pool/*.cpp"
    "code/timer/*.cpp"
    "code/http/*.cpp"
    "code/server/*.cpp"
    "code/buffer/*.cpp"
    "code/main.cpp"
)

# 7. 告诉 CMake 用上面找到的源代码生成名为 "server" 的可执行文件
add_executable(server ${SOURCES})

# 8. 查找系统的多线程库 (相当于 -pthread)
find_package(Threads REQUIRED)

# 9. 将多线程库和 MySQL 库链接到我们的 server 程序上 (相当于 -pthread -lmysqlclient)
target_link_libraries(server PRIVATE Threads::Threads mysqlclient)