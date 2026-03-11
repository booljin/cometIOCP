/**
 * @file server.cpp
 * @brief cometIOCP Echo 服务端示例
 * 
 * 功能：将收到的数据原样返回给客户端
 * 测试：发送 "killme" 可让服务端主动关闭连接
 */

#include "cometIOCP.h"
#include <iostream>
#include <string>

using namespace comet_iocp;

int main() {
    // 使用 RAII 工具初始化 Winsock
    WSAInitializer wsa;
    if (!wsa.is_ok()) {
        std::cerr << "WSAStartup 失败: " << wsa.get_error() << std::endl;
        return 1;
    }

    Driver server;

    // 注册协议
    int proto_id = server.register_protocol(
        // 连接回调
        [&](SOCKET fd, IN_ADDR addr, int port) {
            std::cout << "客户端连接: fd=" << fd 
                      << ", port=" << port << std::endl;
            return 0;
        },
        // 接收回调：Echo 服务
        [&](SOCKET fd, unsigned char* buff, int len) {
            if (len > 0) {
                std::string msg((char*)buff, len);
                if (msg == "killme") {
                    server.close_node(fd);
                    return len;
                }
                server.send_data(fd, buff, len);
            }
            return len;
        },
        // 关闭回调
        [&](SOCKET fd) {
            std::cout << "客户端断开: fd=" << fd << std::endl;
        }
    );

    // 启动监听
    if (server.listen_on("0.0.0.0", 8089, proto_id) != 0) {
        std::cerr << "监听端口失败" << std::endl;
        return 1;
    }

    std::cout << "Echo 服务端启动，监听端口 8089..." << std::endl;

    // 运行 IOCP（2 个工作线程）
    server.run(2);

    // 等待用户输入退出
    std::cin.get();

    server.stop();
    std::cout << "服务端已停止" << std::endl;

    return 0;
}