/*
 * cometIOCP - Driver 类实现
 */

#include "driver.h"
#include "types.h"

#include <stdexcept>
#include <ws2tcpip.h>
#include <mswsock.h>

namespace comet_iocp {

// ============================================================================
// 辅助函数：检测地址族
// ============================================================================

// 检测地址字符串是 IPv4 还是 IPv6
// 返回 AF_INET, AF_INET6, 或 AF_UNSPEC（无效）
static int detect_address_family(const std::string& addr) {
    IN_ADDR ipv4_test;
    if (inet_pton(AF_INET, addr.c_str(), &ipv4_test) == 1) {
        return AF_INET;
    }
    
    IN6_ADDR ipv6_test;
    if (inet_pton(AF_INET6, addr.c_str(), &ipv6_test) == 1) {
        return AF_INET6;
    }
    
    return AF_UNSPEC;
}

// ============================================================================
// Driver 类实现
// ============================================================================

Driver::Driver() {
    _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (_iocp == nullptr) {
        throw std::runtime_error("CreateIoCompletionPort failed");
    }
}

Driver::~Driver() {
    stop();
    
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        for (auto& node : _nodes) {
            SOCKET fd = node.second->fd;
            if (INVALID_SOCKET != fd) {
                shutdown(fd, SD_BOTH);
                closesocket(fd);
            }
            delete node.second;
        }
        _nodes.clear();
    }
    
    if (_iocp != nullptr) {
        CloseHandle(_iocp);
        _iocp = nullptr;
    }
}

void Driver::run(int thread_count) {
    _running = true;
    for (int i = 0; i < thread_count; i++) {
        _work_threads.emplace_back([this]() { work(); });
    }
}

void Driver::stop() {
    for (size_t i = 0; i < _work_threads.size(); i++) {
        PostQueuedCompletionStatus(_iocp, 0, static_cast<ULONG_PTR>(IOCP_EXIT), NULL);
    }
    for (auto& t : _work_threads) {
        if (t.joinable()) t.join();
    }
    _work_threads.clear();
    _running = false;
}

void Driver::register_monitor(MonitorCallback handle) {
    _monitor = handle;
}

void Driver::set_buffer_config(const BufferConfig& config) {
    _buffer_config = config;
}

int Driver::register_protocol(ConnectCallback on_connect, RecvCallback on_recv, CloseCallback on_close) {
    std::unique_lock<std::shared_mutex> lock(_protocol_mtx);  // 写锁：独占
    _protocols.emplace_back(Protocol{(int)_protocols.size(), on_connect, on_recv, on_close});
    return static_cast<int>(_protocols.size() - 1);
}

int Driver::listen_on(const std::string& addr, int port, int protocol_id) {
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (protocol_id < 0 || protocol_id >= (int)_protocols.size()) {
            return COMET_ERROR_INVALID_PROTOCOL;
        }
    }
    
    // 检测地址族
    int addr_family = detect_address_family(addr);
    if (addr_family == AF_UNSPEC) {
        return COMET_ERROR_INVALID_PARAMETER;
    }
    
    SOCKET listen_fd = WSASocket(addr_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == listen_fd) {
        return COMET_ERROR_FAIL;
    }
    
    Node* node_ctx = new Node;
    node_ctx->fd = listen_fd;
    node_ctx->role = Role_Listener;
    node_ctx->protocol_id = protocol_id;
    node_ctx->addr_family = addr_family;  // 保存地址族

    // 使用 sockaddr_storage 容纳 IPv4 或 IPv6 地址
    sockaddr_storage server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    int addr_len = 0;
    
    if (addr_family == AF_INET) {
        sockaddr_in* addr_in = (sockaddr_in*)&server_addr;
        addr_in->sin_family = AF_INET;
        inet_pton(AF_INET, addr.c_str(), &addr_in->sin_addr);
        addr_in->sin_port = htons(port);
        addr_len = sizeof(sockaddr_in);
    } else { // AF_INET6
        sockaddr_in6* addr_in6 = (sockaddr_in6*)&server_addr;
        addr_in6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, addr.c_str(), &addr_in6->sin6_addr);
        addr_in6->sin6_port = htons(port);
        addr_len = sizeof(sockaddr_in6);
    }
    
    if ((SOCKET_ERROR == bind(listen_fd, (sockaddr*)&server_addr, addr_len))
        || (SOCKET_ERROR == listen(listen_fd, SOMAXCONN))
        || (NULL == CreateIoCompletionPort((HANDLE)listen_fd, _iocp, (ULONG_PTR)node_ctx, 0))) {
        close_context(node_ctx);
        return COMET_ERROR_FAIL;
    }

    if (_acceptex == NULL || _get_acceptex_sock_addrs == NULL) {
        DWORD t;
        GUID get_who = WSAID_ACCEPTEX;
        int ret = WSAIoctl(listen_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &get_who, sizeof(get_who), &_acceptex, sizeof(_acceptex), &t, NULL, NULL);
        if (SOCKET_ERROR == ret) {
            close_context(node_ctx);
            return COMET_ERROR_FAIL;
        }
        get_who = WSAID_GETACCEPTEXSOCKADDRS;
        ret = WSAIoctl(listen_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &get_who, sizeof(get_who), &_get_acceptex_sock_addrs, sizeof(_get_acceptex_sock_addrs), &t, NULL, NULL);
        if (SOCKET_ERROR == ret) {
            close_context(node_ctx);
            return COMET_ERROR_FAIL;
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        _nodes[listen_fd] = node_ctx;
    }
    
    IoContext* io_ctx = node_ctx->create_new_io();
    io_ctx->io_type = IO_ACCEPT;
    io_ctx->wsa_buff.buf = new char[ACCEPT_ADDR_BUFFER_SIZE];
    io_ctx->wsa_buff.len = ACCEPT_ADDR_BUFFER_SIZE;
    post_accept(node_ctx, io_ctx);
    return 0;
}

int Driver::connect_to(const std::string& addr, int port, int protocol_id) {
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (protocol_id < 0 || protocol_id >= (int)_protocols.size()) {
            return COMET_ERROR_INVALID_PROTOCOL;
        }
    }
    
    // 检测地址族
    int addr_family = detect_address_family(addr);
    if (addr_family == AF_UNSPEC) {
        return COMET_ERROR_INVALID_PARAMETER;
    }
    
    SOCKET connect_fd = WSASocket(addr_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == connect_fd) {
        return COMET_ERROR_FAIL;
    }

    Node* node_ctx = new Node;
    node_ctx->fd = connect_fd;
    node_ctx->role = Role_Client;
    node_ctx->protocol_id = protocol_id;
    node_ctx->addr_family = addr_family;  // 保存地址族

    if (_connectex == NULL) {
        DWORD t;
        GUID get_who = WSAID_CONNECTEX;
        int ret = WSAIoctl(connect_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &get_who, sizeof(get_who), &_connectex, sizeof(_connectex), &t, NULL, NULL);
        if (SOCKET_ERROR == ret) {
            close_context(node_ctx);
            return COMET_ERROR_FAIL;
        }
    }

    if (NULL == CreateIoCompletionPort((HANDLE)connect_fd, _iocp, (ULONG_PTR)node_ctx, 0)) {
        close_context(node_ctx);
        return COMET_ERROR_FAIL;
    }

    // 准备服务器地址
    sockaddr_storage server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    int server_addr_len = 0;
    
    if (addr_family == AF_INET) {
        sockaddr_in* addr_in = (sockaddr_in*)&server_addr;
        addr_in->sin_family = AF_INET;
        inet_pton(AF_INET, addr.c_str(), &addr_in->sin_addr);
        addr_in->sin_port = htons(port);
        server_addr_len = sizeof(sockaddr_in);
    } else { // AF_INET6
        sockaddr_in6* addr_in6 = (sockaddr_in6*)&server_addr;
        addr_in6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, addr.c_str(), &addr_in6->sin6_addr);
        addr_in6->sin6_port = htons(port);
        server_addr_len = sizeof(sockaddr_in6);
    }

    // 绑定本地地址
    sockaddr_storage local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    int local_addr_len = 0;
    
    if (addr_family == AF_INET) {
        sockaddr_in* local_in = (sockaddr_in*)&local_addr;
        local_in->sin_family = AF_INET;
        local_in->sin_addr.S_un.S_addr = INADDR_ANY;
        local_in->sin_port = 0;
        local_addr_len = sizeof(sockaddr_in);
    } else { // AF_INET6
        sockaddr_in6* local_in6 = (sockaddr_in6*)&local_addr;
        local_in6->sin6_family = AF_INET6;
        local_in6->sin6_addr = in6addr_any;
        local_in6->sin6_port = 0;
        local_addr_len = sizeof(sockaddr_in6);
    }
    
    if (SOCKET_ERROR == bind(connect_fd, (sockaddr*)&local_addr, local_addr_len)) {
        close_context(node_ctx);
        return COMET_ERROR_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        _nodes[connect_fd] = node_ctx;
    }

    IoContext* io_ctx = node_ctx->create_new_io();
    io_ctx->io_type = IO_CONNECTING;
    io_ctx->fd = connect_fd;
    memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
    
    if (!_connectex(connect_fd, (sockaddr*)&server_addr, server_addr_len, NULL, 0, NULL, &io_ctx->overlapped)) {
        if (WSA_IO_PENDING != WSAGetLastError()) {
            node_ctx->erase_io(io_ctx);
            close_context(node_ctx);
            return COMET_ERROR_FAIL;
        }
    }
    return 0;
}

void Driver::send_data(SOCKET fd, unsigned char* buff, int len) {
    Node* node_ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        auto it = _nodes.find(fd);
        if (it != _nodes.end()) {
            node_ctx = it->second;
        }
    }
    if (node_ctx == nullptr) return;

    IoContext* io_ctx = node_ctx->create_new_io();
    io_ctx->fd = node_ctx->fd;
    io_ctx->io_type = IO_SEND;
    io_ctx->wsa_buff.buf = new char[len];
    io_ctx->wsa_buff.len = len;
    memcpy(io_ctx->wsa_buff.buf, buff, len);
    post_send(node_ctx, io_ctx);
}

void Driver::send_data_raw(SOCKET fd, unsigned char* buff, int len) {
    Node* node_ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        auto it = _nodes.find(fd);
        if (it != _nodes.end()) {
            node_ctx = it->second;
        }
    }
    if (node_ctx == nullptr) {
        delete[] buff;
        return;
    }

    IoContext* io_ctx = node_ctx->create_new_io();
    io_ctx->fd = node_ctx->fd;
    io_ctx->io_type = IO_SEND;
    io_ctx->wsa_buff.buf = (char*)buff;
    io_ctx->wsa_buff.len = len;
    post_send(node_ctx, io_ctx);
}

void Driver::close_node(SOCKET fd) {
    PostQueuedCompletionStatus(_iocp, static_cast<DWORD>(IOCP_CLOSE_NODE), (ULONG_PTR)fd, NULL);
}

int Driver::post_accept(Node* node_ctx, IoContext* io_ctx) {
    memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
    
    // 根据监听 socket 的地址族创建 accept socket
    int addr_family = node_ctx->addr_family;
    io_ctx->fd = WSASocket(addr_family, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == io_ctx->fd) {
        close_context(node_ctx);
        return -1;
    }
    
    // 使用 sockaddr_in6 的大小（足够容纳 IPv4 和 IPv6）
    int addr_size = sizeof(sockaddr_in6) + 16;
    DWORD dw_t;
    if (!_acceptex(node_ctx->fd, io_ctx->fd, io_ctx->wsa_buff.buf, 0, addr_size, addr_size, &dw_t, &io_ctx->overlapped)) {
        if (WSA_IO_PENDING != WSAGetLastError()) {
            close_context(node_ctx);
            return -1;
        }
    }
    return 0;
}

int Driver::post_recv(Node* node_ctx, IoContext* io_ctx) {
    memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
    
    DWORD flag = 0;
    if (SOCKET_ERROR == WSARecv(node_ctx->fd, &(io_ctx->wsa_buff), 1, NULL, &flag, &(io_ctx->overlapped), NULL)) {
        if (ERROR_IO_PENDING != WSAGetLastError()) {
            close_context(node_ctx);
            return -1;
        }
    }
    return 0;
}

int Driver::post_send(Node* node_ctx, IoContext* io_ctx) {
    memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
    
    if (SOCKET_ERROR == WSASend(node_ctx->fd, &io_ctx->wsa_buff, 1, NULL, 0, &io_ctx->overlapped, NULL)) {
        if (ERROR_IO_PENDING != WSAGetLastError()) {
            close_context(node_ctx);
            return -1;
        }
    }
    return 0;
}

void Driver::work() {
    DWORD bytes;
    Node* node_ctx;
    IoContext* io_ctx;
    OVERLAPPED* ol;
    
    while (true) {
        bool ok = GetQueuedCompletionStatus(_iocp, &bytes, (PULONG_PTR)&node_ctx, &ol, INFINITE);
        io_ctx = CONTAINING_RECORD(ol, IoContext, overlapped);
        
        if (IOCP_EXIT == (ULONG_PTR)node_ctx) {
            break;
        }
        
        if (IOCP_CLOSE_NODE == (ULONG_PTR)bytes) {
            SOCKET fd = (SOCKET)node_ctx;
            if (INVALID_SOCKET == fd) continue;
            Node* target_node = nullptr;
            {
                std::lock_guard<std::mutex> lock(_node_mtx);
                auto it = _nodes.find(fd);
                if (it != _nodes.end()) {
                    target_node = it->second;
                }
            }
            if (target_node != nullptr) {
                close_context(target_node);
            } else {
                closesocket(fd);
            }
            continue;
        }

        if (!ok) {
            DWORD err = GetLastError();
            if (WAIT_TIMEOUT == err) {
                continue;
            } else {
                close_context(node_ctx);
                continue;
            }
        }
        
        if (0 == bytes && (IO_RECV == io_ctx->io_type || IO_SEND == io_ctx->io_type)) {
            close_context(node_ctx);
            continue;
        }
        
        switch (io_ctx->io_type) {
        case IO_ACCEPT:
            on_accept(node_ctx, io_ctx);
            break;
        case IO_RECV:
            on_recv(node_ctx, io_ctx, bytes);
            break;
        case IO_SEND:
            on_send(node_ctx, io_ctx);
            break;
        case IO_CONNECTING:
            on_connecting(node_ctx, io_ctx);
            break;
        }
    }
}

void Driver::on_accept(Node* node_ctx, IoContext* io_ctx) {
    // 使用 SOCKADDR_STORAGE 容纳 IPv4 或 IPv6 地址
    SOCKADDR_STORAGE* remote = NULL;
    SOCKADDR_STORAGE* local = NULL;
    int len_r = sizeof(SOCKADDR_STORAGE);
    int len_l = sizeof(SOCKADDR_STORAGE);
    
    // 使用 sockaddr_in6 的大小（足够容纳 IPv4 和 IPv6）
    int addr_size = sizeof(sockaddr_in6) + 16;
    _get_acceptex_sock_addrs(io_ctx->wsa_buff.buf, 0, addr_size, addr_size,
        (LPSOCKADDR*)&local, &len_l, (LPSOCKADDR*)&remote, &len_r);
    
    Node* n = new Node;
    n->fd = io_ctx->fd;
    n->role = Role_Client;
    n->protocol_id = node_ctx->protocol_id;
    n->addr_family = node_ctx->addr_family;  // 继承监听 socket 的地址族
    
    if (NULL == CreateIoCompletionPort((HANDLE)io_ctx->fd, _iocp, (ULONG_PTR)n, 0)) {
        close_context(n);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        _nodes[n->fd] = n;
    }

    ConnectCallback connect_cb;
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (n->protocol_id >= 0 && n->protocol_id < (int)_protocols.size()) {
            connect_cb = _protocols[n->protocol_id].connect_cb;
        }
    }
    if (connect_cb) {
        // 构建 AddressInfo
        AddressInfo addr_info;
        addr_info.family = remote->ss_family;
        
        if (remote->ss_family == AF_INET) {
            sockaddr_in* remote_in = (sockaddr_in*)remote;
            addr_info.addr.ipv4 = remote_in->sin_addr;
            addr_info.port = ntohs(remote_in->sin_port);
        } else if (remote->ss_family == AF_INET6) {
            sockaddr_in6* remote_in6 = (sockaddr_in6*)remote;
            addr_info.addr.ipv6 = remote_in6->sin6_addr;
            addr_info.port = ntohs(remote_in6->sin6_port);
        }
        
        if (0 != connect_cb(io_ctx->fd, addr_info)) {
            close_context(n);
            return;
        }
    }

    {
        IoContext* io_ctx_ = n->create_new_io();
        io_ctx_->fd = n->fd;
        io_ctx_->io_type = IO_RECV;
        io_ctx_->wsa_buff.buf = new char[_buffer_config.initial_size];
        io_ctx_->wsa_buff.len = _buffer_config.initial_size;
        post_recv(n, io_ctx_);
    }
    
    post_accept(node_ctx, io_ctx);
}

void Driver::on_connecting(Node* node_ctx, IoContext* io_ctx) {
    ConnectCallback connect_cb;
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (node_ctx->protocol_id >= 0 && node_ctx->protocol_id < (int)_protocols.size()) {
            connect_cb = _protocols[node_ctx->protocol_id].connect_cb;
        }
    }
    if (connect_cb) {
        // 客户端连接成功，传递地址信息
        AddressInfo addr_info;
        addr_info.family = node_ctx->addr_family;
        addr_info.port = 0;
        connect_cb(node_ctx->fd, addr_info);
    }
    
    node_ctx->erase_io(io_ctx);
    
    {
        IoContext* io_ctx_ = node_ctx->create_new_io();
        io_ctx_->fd = node_ctx->fd;
        io_ctx_->io_type = IO_RECV;
        io_ctx_->wsa_buff.buf = new char[_buffer_config.initial_size];
        io_ctx_->wsa_buff.len = _buffer_config.initial_size;
        post_recv(node_ctx, io_ctx_);
    }
}

void Driver::on_recv(Node* node_ctx, IoContext* io_ctx, int bytes) {
    int remain = node_ctx->buffer_capacity - node_ctx->data_size;
    if (remain < bytes) {
        int new_size = node_ctx->buffer_capacity;
        if (new_size == 0) new_size = _buffer_config.initial_size;
        while (new_size < (node_ctx->data_size + bytes)) {
            new_size = (int)(new_size * _buffer_config.grow_factor);
            if (new_size <= node_ctx->buffer_capacity) new_size = node_ctx->buffer_capacity + 1;
        }
        unsigned char* new_buff = new unsigned char[new_size];
        if (node_ctx->buffer) {
            memcpy(new_buff, node_ctx->buffer, node_ctx->data_size);
            delete[] node_ctx->buffer;
        }
        node_ctx->buffer = new_buff;
        node_ctx->buffer_capacity = new_size;
    }
    
    memcpy(node_ctx->buffer + node_ctx->data_size, io_ctx->wsa_buff.buf, bytes);
    node_ctx->data_size = node_ctx->data_size + bytes;
    
    RecvCallback recv_cb;
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (node_ctx->protocol_id >= 0 && node_ctx->protocol_id < (int)_protocols.size()) {
            recv_cb = _protocols[node_ctx->protocol_id].recv_cb;
        }
    }
    if (recv_cb) {
        int recv_len = recv_cb(node_ctx->fd, node_ctx->buffer, node_ctx->data_size);
        if (recv_len > 0) {
            if (recv_len < node_ctx->data_size) {
                memcpy(node_ctx->buffer, node_ctx->buffer + recv_len, node_ctx->data_size - recv_len);
                node_ctx->data_size -= recv_len;
            } else {
                node_ctx->data_size = 0;
            }
        }
    } else {
        node_ctx->data_size = 0;
    }
    
    if (node_ctx->buffer_capacity > _buffer_config.shrink_min_size &&
        node_ctx->data_size < node_ctx->buffer_capacity * _buffer_config.shrink_threshold) {
        int new_size = node_ctx->data_size * 2;
        if (new_size < _buffer_config.initial_size) {
            new_size = _buffer_config.initial_size;
        }
        if (new_size < node_ctx->buffer_capacity) {
            unsigned char* new_buff = new unsigned char[new_size];
            if (node_ctx->buffer && node_ctx->data_size > 0) {
                memcpy(new_buff, node_ctx->buffer, node_ctx->data_size);
            }
            if (node_ctx->buffer) {
                delete[] node_ctx->buffer;
            }
            node_ctx->buffer = new_buff;
            node_ctx->buffer_capacity = new_size;
        }
    }
    
    post_recv(node_ctx, io_ctx);
}

void Driver::on_send(Node* node_ctx, IoContext* io_ctx) {
    node_ctx->erase_io(io_ctx);
}

void Driver::close_context(Node* node_ctx) {
    SOCKET fd = node_ctx->fd;
    if (INVALID_SOCKET != fd) {
        shutdown(fd, SD_BOTH);
        closesocket(fd);
    }
    if (node_ctx->buffer != nullptr) {
        delete[] node_ctx->buffer;
        node_ctx->buffer = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(_node_mtx);
        _nodes.erase(fd);
    }
    
    CloseCallback close_cb;
    {
        std::shared_lock<std::shared_mutex> lock(_protocol_mtx);  // 读锁：共享
        if (node_ctx->protocol_id >= 0 && node_ctx->protocol_id < (int)_protocols.size()) {
            close_cb = _protocols[node_ctx->protocol_id].close_cb;
        }
    }
    if (close_cb) close_cb(fd);
    delete node_ctx;
}

} // namespace comet_iocp