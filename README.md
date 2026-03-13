<!--
META DATA - 元数据
文档更新日期: 2026-03-13

【重要通知 - 致未来维护此文档的 AI】

1. 每次修改此文档时，请务必更新上方的"文档更新日期"，保持日期格式为 YYYY-MM-DD。
2. "致谢"章节是项目的重要信息，请务必保留，不得删除或修改。
-->

# cometIOCP

基于 Windows I/O Completion Port 的高性能异步网络通信库。

## 特性

- **简洁 API** - 清晰的头文件结构，易于集成
- **RAII 设计** - `WSAInitializer` 自动管理 Winsock 生命周期
- **多协议支持** - 同一服务端可注册多种协议处理不同连接
- **灵活回调** - 基于 `std::function` 的事件回调机制
- **多线程支持** - 可配置工作线程数，充分发挥多核性能
- **动态缓冲区** - 按需扩容/缩容，避免内存浪费
- **客户端/服务端** - 同时支持监听端口和主动连接
- **零拷贝发送** - `send_data_raw` 支持直接使用用户内存
- **IPv4/IPv6 双栈** - 自动检测地址类型，无缝支持 IPv6

## 项目结构

```
cometIOCP/
├── include/
│   └── cometIOCP.h      # 公共头文件（用户引入此文件）
├── src/
│   ├── driver.h         # Driver 类声明
│   ├── driver.cpp       # Driver 类实现
│   ├── types.h          # 类型定义
│   └── util.h           # WSAInitializer 工具类
├── example/
│   ├── server.cpp       # Echo 服务端示例
│   └── client.cpp       # Echo 客户端示例
├── test/
│   └── stress_test.cpp  # 功能正确性测试
└── CMakeLists.txt
```

## 快速开始

### 编译

```cmd
cmake -B build -A x64
cmake --build build --config Debug
```

编译目标：
- `cometIOCP` - 静态库
- `echo_server` - Echo 服务端示例
- `echo_client` - Echo 客户端示例
- `stress_test` - 功能正确性测试

### 服务端示例

```cpp
#include "cometIOCP.h"
#include <iostream>

using namespace comet_iocp;

int main() {
    // RAII 方式初始化 Winsock
    WSAInitializer wsa;
    if (!wsa.is_ok()) {
        std::cerr << "WSAStartup 失败: " << wsa.get_error() << std::endl;
        return 1;
    }

    Driver server;

    int proto_id = server.register_protocol(
        [](SOCKET fd, const AddressInfo& addr) -> int {
            std::cout << "Client connected" << std::endl;
            return 0;
        },
        [&](SOCKET fd, unsigned char* buff, int len) -> int {
            server.send_data(fd, buff, len); // Echo back
            return len;
        },
        [](SOCKET fd) {
            std::cout << "Client disconnected" << std::endl;
        }
    );

    server.listen_on("0.0.0.0", 8089, proto_id);
    server.run(2);  // 使用 2 个工作线程

    std::cin.get();
    server.stop();
    return 0;
}
```

### 客户端示例

```cpp
#include "cometIOCP.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace comet_iocp;

int main() {
    WSAInitializer wsa;
    if (!wsa.is_ok()) {
        std::cerr << "WSAStartup 失败" << std::endl;
        return 1;
    }

    Driver client;
    SOCKET client_fd = INVALID_SOCKET;

    int proto_id = client.register_protocol(
        [&](SOCKET fd, const AddressInfo& addr) -> int {
            std::cout << "Connected" << std::endl;
            client_fd = fd;
            return 0;
        },
        [](SOCKET fd, unsigned char* buff, int len) -> int {
            std::cout << "Received: " << std::string((char*)buff, len);
            return len;
        },
        [](SOCKET fd) {
            std::cout << "Disconnected" << std::endl;
        }
    );

    client.connect_to("127.0.0.1", 8089, proto_id);
    client.run(1);

    // 等待连接成功
    while (client_fd == INVALID_SOCKET) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 发送数据
    std::string msg = "Hello\n";
    client.send_data(client_fd, (unsigned char*)msg.c_str(), (int)msg.size());

    std::cin.get();
    client.stop();
    return 0;
}
```

## API 参考

### WSAInitializer 类

RAII 风格的 Winsock 初始化工具。

```cpp
WSAInitializer wsa;              // 默认 Winsock 2.2
if (!wsa.is_ok()) {              // 检查是否成功
    std::cerr << wsa.get_error() << std::endl;
}
// 程序退出时自动调用 WSACleanup()
```

### Driver 类

#### 生命周期

| 方法 | 说明 |
|------|------|
| `Driver()` | 构造函数，创建 IOCP 句柄 |
| `~Driver()` | 析构函数，停止线程并清理所有资源 |

#### 运行控制

| 方法 | 说明 |
|------|------|
| `void run(int thread_count = 1)` | 启动工作线程 |
| `void stop()` | 停止所有工作线程 |

#### 协议与连接

| 方法 | 说明 |
|------|------|
| `int register_protocol(on_connect, on_recv, on_close)` | 注册协议，返回协议 ID |
| `int listen_on(addr, port, protocol_id)` | 在指定端口监听（支持 IPv4/IPv6） |
| `int connect_to(addr, port, protocol_id)` | 连接到远程服务器（支持 IPv4/IPv6） |

#### 数据发送

| 方法 | 说明 |
|------|------|
| `void send_data(fd, buff, len)` | 发送数据（内部拷贝） |
| `void send_data_raw(fd, buff, len)` | 发送数据（零拷贝，库接管内存） |
| `void close_node(fd)` | 主动关闭连接 |

### 回调函数签名

```cpp
// 地址信息结构体（支持 IPv4 和 IPv6）
struct AddressInfo {
    int family;         // AF_INET 或 AF_INET6
    int port;           // 端口号
    
    bool is_ipv6() const;                 // 检查是否为 IPv6
    const IN_ADDR& get_ipv4() const;      // 获取 IPv4 地址
    const IN6_ADDR& get_ipv6() const;     // 获取 IPv6 地址
};

// 连接回调：返回 0 接受连接，非 0 拒绝
using ConnectCallback = std::function<int(SOCKET fd, const AddressInfo& addr)>;

// 接收回调：返回已处理的字节数
using RecvCallback = std::function<int(SOCKET fd, unsigned char* buff, int len)>;

// 关闭回调
using CloseCallback = std::function<void(SOCKET fd)>;
```

### IPv6 使用示例

```cpp
// IPv4 监听
server.listen_on("0.0.0.0", 8089, proto_id);

// IPv6 监听
server.listen_on("::", 8089, proto_id);

// IPv4 连接
client.connect_to("127.0.0.1", 8089, proto_id);

// IPv6 连接
client.connect_to("::1", 8089, proto_id);

// 连接回调中获取地址信息
int on_connect(SOCKET fd, const AddressInfo& addr) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (addr.is_ipv6()) {
        inet_ntop(AF_INET6, &addr.get_ipv6(), buf, sizeof(buf));
        std::cout << "IPv6 连接: [" << buf << "]:" << addr.port << std::endl;
    } else {
        inet_ntop(AF_INET, &addr.get_ipv4(), buf, sizeof(buf));
        std::cout << "IPv4 连接: " << buf << ":" << addr.port << std::endl;
    }
    return 0;
}
```

### 错误码

| 常量 | 值 | 说明 |
|------|----|----|
| `ERROR_OK` | 0 | 成功 |
| `ERROR_FAIL` | -1 | 一般错误 |
| `ERROR_INVALID_PROTOCOL` | -2 | 无效协议 ID |

## 运行测试

```cmd
# 编译并运行功能正确性测试
cmake --build build --config Debug --target stress_test
build\Debug\stress_test.exe
```

测试内容：
1. 基本 C→S 收发测试（小/中/大数据）
2. 基本 S→C 收发测试
3. 连续发送测试（验证 TCP 流式处理）
4. 边界条件测试
5. 多客户端并发测试

## 注意事项

1. **线程安全** - 所有公共 API 都是线程安全的
2. **内存管理** - `send_data_raw` 会接管传入内存的所有权
3. **接收回调** - 返回值表示已消费字节数，未消费数据保留在缓冲区
4. **WSA 初始化** - 使用 `WSAInitializer` 进行 RAII 管理

## 依赖

- Windows SDK (WinSock2, mswsock)
- C++17 或更高版本
- CMake 3.21+

## License

MIT License