/**
 * @file client.cpp
 * @brief cometIOCP Echo 客户端示例
 * 
 * 功能：连接到服务端，发送用户输入的数据并显示返回结果
 */

#include "cometIOCP.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace comet_iocp;

int main() {
    // 使用 RAII 工具初始化 Winsock
    WSAInitializer wsa;
    if (!wsa.is_ok()) {
        std::cerr << "WSAStartup 失败: " << wsa.get_error() << std::endl;
        return 1;
    }

    Driver client;
    SOCKET client_fd = INVALID_SOCKET;
    bool connected = false;

    // 注册协议
    int proto_id = client.register_protocol(
        // 连接回调
        [&](SOCKET fd, const AddressInfo& addr) {
            std::cout << "连接成功: fd=" << fd << std::endl;
            client_fd = fd;
            connected = true;
            return 0;
        },
        // 接收回调
        [&](SOCKET fd, unsigned char* buff, int len) {
            if (len > 0) {
                std::string msg((char*)buff, len);
                std::cout << "服务端返回: " << msg << std::endl;
            }
            return len;
        },
        // 关闭回调：自动重连
        [&](SOCKET fd) {
            std::cout << "连接断开，1秒后重连..." << std::endl;
            connected = false;
            client_fd = INVALID_SOCKET;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            client.connect_to("127.0.0.1", 8089, proto_id);
        }
    );

    // 连接到服务端
    if (client.connect_to("127.0.0.1", 8089, proto_id) != 0) {
        std::cerr << "连接失败" << std::endl;
        return 1;
    }

    // 运行 IOCP
    client.run(1);

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "已连接到服务端，输入消息发送（输入 exit 退出）：" << std::endl;

    // 主循环：读取用户输入并发送
    while (true) {
        std::string input;
        std::getline(std::cin, input);
        
        if (input == "exit") {
            break;
        }
        
        if (connected && client_fd != INVALID_SOCKET) {
            client.send_data(client_fd, 
                reinterpret_cast<unsigned char*>(const_cast<char*>(input.c_str())), 
                static_cast<int>(input.size()));
        } else {
            std::cout << "未连接，请稍候..." << std::endl;
        }
    }

    client.stop();
    std::cout << "客户端已退出" << std::endl;

    return 0;
}