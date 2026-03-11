/*
 * cometIOCP - 内部类型定义
 * 
 * 这些类型仅供库内部使用，用户不需要了解。
 */

#ifndef __COMET_TYPES_H__
#define __COMET_TYPES_H__

#include <WinSock2.h>
#include <mswsock.h>
#include <list>
#include <mutex>

namespace comet_iocp {

// ============================================================================
// IOCP 控制命令
// ============================================================================

// 通过 PostQueuedCompletionStatus 投递，work() 线程根据这些值执行特殊操作
const ULONG_PTR IOCP_EXIT = static_cast<ULONG_PTR>(-1);        // 通知工作线程退出
const ULONG_PTR IOCP_CLOSE_NODE = static_cast<ULONG_PTR>(-2);  // 通知工作线程关闭指定连接

// AcceptEx 需要: 本地地址(16) + 远程地址(16) + 两地址各加16字节保留空间
const int ACCEPT_ADDR_BUFFER_SIZE = (sizeof(sockaddr_in) + 16) * 2;

// ============================================================================
// IO 工作类型
// ============================================================================

enum IOType {
    IO_ACCEPT,      // 接受连接
    IO_RECV,        // 接收数据
    IO_SEND,        // 发送数据
    IO_CONNECTING   // 连接中
};

// ============================================================================
// 节点角色
// ============================================================================

enum NodeRole {
    Role_Listener,
    Role_Client
};

// ============================================================================
// IoContext - I/O 上下文
// ============================================================================

struct IoContext {
    OVERLAPPED overlapped;      // 重叠I/O核心结构
    WSABUF wsa_buff{ NULL, 0 }; // 数据缓冲区
    IOType io_type;             // 操作类型
    SOCKET fd = INVALID_SOCKET; // 关联的socket

    ~IoContext() {
        if (wsa_buff.buf != NULL)
            delete[] wsa_buff.buf;
    }
};

// ============================================================================
// Node - 通讯节点
// ============================================================================

struct Node {
    SOCKET fd;                              // socket
    int role;                               // 该node的角色
    int protocol_id;                        // 该node所适用的协议ID
    std::list<IoContext*> io_ctxs;          // 该node当前持有的io上下文
    std::mutex io_ctxs_mtx;                 // 保护 io_ctxs 的互斥锁
    
    // 接收缓冲区
    unsigned char* buffer{ nullptr };
    int buffer_capacity{ 0 };
    int data_size{ 0 };

    // 创建新的 IoContext
    IoContext* create_new_io() {
        IoContext* ctx = new IoContext;
        std::lock_guard<std::mutex> lock(io_ctxs_mtx);
        io_ctxs.emplace_front(ctx);
        return ctx;
    }

    // 移除并销毁 IoContext
    void erase_io(IoContext* ctx) {
        {
            std::lock_guard<std::mutex> lock(io_ctxs_mtx);
            for (auto it = io_ctxs.begin(); it != io_ctxs.end(); it++) {
                if (*it == ctx) {
                    io_ctxs.erase(it);
                    break;
                }
            }
        }
        delete ctx;
    }

    ~Node() {
        if (buffer != nullptr)
            delete[] buffer;
        // 清除这个node的所有io上下文
        std::lock_guard<std::mutex> lock(io_ctxs_mtx);
        for (auto ctx : io_ctxs) {
            delete ctx;
        }
    }
};

// ============================================================================
// Protocol - 协议处理器
// ============================================================================

struct Protocol {
    int protocol_id;
    ConnectCallback connect_cb;
    RecvCallback recv_cb;
    CloseCallback close_cb;
};

} // namespace comet_iocp

#endif // __COMET_TYPES_H__