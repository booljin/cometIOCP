/*
 * cometIOCP - Driver 类声明
 *
 * 这是用户主要使用的类，提供异步网络通信功能。
 */

#ifndef __COMET_DRIVER_H__
#define __COMET_DRIVER_H__

// Windows 头文件
#include <WinSock2.h>
#include <ws2tcpip.h>  // IPv6 支持
#include <Windows.h>
#include <MSWSock.h>  // AcceptEx, ConnectEx 等

// C++ 标准库
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>

// 内部类型定义（包含 AddressInfo, ConnectCallback 等）
#include "types.h"

namespace comet_iocp {

// ============================================================================
// 监视器回调（其他回调已在 types.h 中定义）
// ============================================================================

using MonitorCallback = std::function<void(int info, const std::string& msg)>;

// ============================================================================
// 错误码定义
// ============================================================================

// 使用 COMET_ 前缀避免与 Windows 宏冲突
enum ErrorCode {
    COMET_SUCCESS = 0,
    COMET_ERROR_FAIL = -1,
    COMET_ERROR_INVALID_PARAMETER = -2,
    COMET_ERROR_INVALID_SOCKET = -3,
    COMET_ERROR_INVALID_PROTOCOL = -4,
    COMET_ERROR_INVALID_CONTEXT = -5,
    COMET_ERROR_INVALID_BUFFER = -6,
    COMET_ERROR_INVALID_LENGTH = -7,
};

// ============================================================================
// Driver 类 - 核心网络驱动
// ============================================================================

// 前向声明（内部类型，用户不需要了解）
struct Node;
struct IoContext;
struct Protocol;

class Driver {
public:
    Driver();
    virtual ~Driver();

    // 缓冲区配置
    struct BufferConfig {
        int initial_size = 1000;        // 初始缓冲区大小（字节）
        float grow_factor = 1.5f;       // 增长因子
        float shrink_threshold = 0.25f; // 收缩阈值（使用量 < 容量 * 阈值时考虑收缩）
        int shrink_min_size = 4000;     // 最小收缩大小（小于此值不收缩）
    };

    // ------------------------------------------------------------------------
    // 生命周期管理
    // ------------------------------------------------------------------------
    
    // 启动工作线程（默认1个，可指定多个）
    void run(int thread_count = 1);
    
    // 设置缓冲区配置（需在 run() 之前调用）
    void set_buffer_config(const BufferConfig& config);
    
    // 停止工作线程，一般不需要手动调用
    void stop();

    // ------------------------------------------------------------------------
    // 协议与监听
    // ------------------------------------------------------------------------
    
    // 注册监视器（需在 run() 之前调用）
    void register_monitor(MonitorCallback handle);

    // 注册协议，返回协议 ID
    // 警告：必须在 run() 之前调用！运行时注册会导致数据竞争
    int register_protocol(ConnectCallback on_connect, RecvCallback on_recv, CloseCallback on_close);

    // 在指定地址端口上监听，关联指定协议
    int listen_on(const std::string& addr, int port, int protocol_id);

    // 连接到指定地址，关联指定协议
    int connect_to(const std::string& addr, int port, int protocol_id);

    // ------------------------------------------------------------------------
    // 数据发送
    // ------------------------------------------------------------------------
    
    // 发送数据（复制模式，调用者可立即释放缓冲区）
    void send_data(SOCKET fd, unsigned char* buff, int len);

    // 发送数据（零拷贝模式）
    // 警告：库会在发送完成后自动 delete[] 释放缓冲区，调用者不可再访问！
    void send_data_raw(SOCKET fd, unsigned char* buff, int len);

    // 主动关闭连接
    void close_node(SOCKET fd);

protected:
    // 内部实现（用户不需要关注）
    int post_accept(Node* node_ctx, IoContext* io_ctx);
    int post_recv(Node* node_ctx, IoContext* io_ctx);
    int post_send(Node* node_ctx, IoContext* io_ctx);
    void work();
    void on_accept(Node*, IoContext*);
    void on_connecting(Node*, IoContext*);
    void on_recv(Node*, IoContext*, int len);
    void on_send(Node*, IoContext*);
    void close_context(Node*);

protected:
    HANDLE _iocp;
    std::vector<std::thread> _work_threads;
    std::map<SOCKET, Node*> _nodes;
    std::mutex _node_mtx;
    std::vector<Protocol> _protocols;
    MonitorCallback _monitor{ nullptr };
    BufferConfig _buffer_config;

private:
    LPFN_ACCEPTEX _acceptex{ NULL };
    LPFN_GETACCEPTEXSOCKADDRS _get_acceptex_sock_addrs{ NULL };
    LPFN_CONNECTEX _connectex{ NULL };
    std::atomic<bool> _running{ false };
};

} // namespace comet_iocp

#endif // __COMET_DRIVER_H__