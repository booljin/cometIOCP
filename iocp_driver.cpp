#include "iocp_driver.h"

#include <stdexcept>
#include <list>

#include <ws2tcpip.h>
#include <mswsock.h>

namespace IOCP_DRIVER {

	const DWORD IOCP_EXIT = -1;
	const DWORD IOCP_CLOSE_NODE = -2;

	// IO端口工作模式，工作线程以此决定行为
	enum IOType {
		IO_ACCEPT,    // 接受连接
		IO_RECV,      // 接收数据
		IO_SEND,      // 发送数据
        IO_CONNECTING,// 连接中
	};

	// 通讯上下文
	struct IO_context {

		OVERLAPPED overlapped;      // 重叠I/O核心结构
		WSABUF wsa_buff{ NULL, 0 }; // 数据缓冲区
		IOType io_type;             // 操作类型
		SOCKET fd = INVALID_SOCKET; // 关联的socket

		~IO_context() {
			if (wsa_buff.buf != NULL)
				delete[] wsa_buff.buf;
		}
	};


	enum NodeRole {
		Role_Listener,
		Role_Customer
	};

	// 通讯节点，每个socket对应一个
	struct node_context {
		SOCKET fd;								// socket
		int role;								// 该node的角色，主要是识别该socket是否是监听端口
        int protocol;							// 该node所适用的协议。每个socket上的数据流都会遵循一定的收发规范，否则无法正常通讯
		std::list<IO_context*> io_ctxs;			// 该node当前持有的io上下文。一个典型的node会始终持有一个recv io，以及可能有若干个send io
		
		// 为了避免频繁开辟内存，buff会随需要扩增，但暂不考虑缩减。如果需要缩减，需要统计当前buff余量
		unsigned char* buff{ nullptr };			// 该node会将所有已接收数据存入这个buff
		int total_len{0};						// 当前缓冲区上限，用于检测是否需要扩容
		int cur_len{0};							// 当前缓冲区使用量

		// 创建和销毁io_ctx的行为都应该在iocp的work线程上工作，work线程目前是单线程，不用考虑竞争问题，故不加锁
		IO_context* create_new_io() {
			IO_context* ctx = new IO_context;
			io_ctxs.emplace_front(ctx);
			return ctx;
		}
		void erase_io(IO_context* ctx) {
			for (auto it = io_ctxs.begin(); it != io_ctxs.end(); it++) {
				if (*it == ctx) {
					io_ctxs.erase(it);
					break;
				}
			}
			delete ctx;
		}
		~node_context() {
			if (buff != nullptr)
				delete[] buff;
			// 清除这个node的所有io上下文
			for (auto ctx : io_ctxs) {
				delete ctx;
			}
		}
	};

    // 不同通讯节点可能有不同协议，对应不同应用层，这里分别注册
    struct protocol{
        int protocol_id;
        IOCP_OnConnectCB connect_cb;
        IOCP_OnRecvCB recv_cb;
        IOCP_OnCloseCB close_cb;
    };

	iocp_driver::iocp_driver() {
		_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		if (_iocp == nullptr) {
			throw std::runtime_error("CreateIoCompletionPort failed");
		}
	}

    iocp_driver::~iocp_driver() {
		stop();
		for (auto& node : _nodes) {
			// 此时不能再调用close_node了;
			SOCKET fd = node.second->fd;
			if (INVALID_SOCKET != fd)
				closesocket(fd);
			delete(node.second);
		}
		if (_iocp != nullptr) {
			CloseHandle(_iocp);
		}
	}

	void iocp_driver::run() {
		_work_thread = std::thread([this]() {
			work();
			});
	}

    void iocp_driver::stop(){
        // 投递结束信号
		PostQueuedCompletionStatus(_iocp, 0, (DWORD)IOCP_EXIT, NULL);
        if(_work_thread.joinable())
			_work_thread.join();
    }

	void iocp_driver::regist_monitor(IOCP_MonitorCB handle) {
		_monitor = handle;
	}

    int iocp_driver::regist_protocol(IOCP_OnConnectCB ah, IOCP_OnRecvCB rh, IOCP_OnCloseCB ch){
        _protocols.emplace_back(protocol{(int)_protocols.size(), ah, rh, ch});
        return _protocols.size() - 1;
    }

    int iocp_driver::listen_from(const std::string& addr, int port, int protocol_id) {
		if(protocol_id < 0 || protocol_id >= _protocols.size()){
            // 未知协议，不知道该怎么处理通讯
            return IOCP_ERROR_INVALID_PROTOCOL;
        }
        SOCKET listen_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == listen_fd) {
			return IOCP_ERROR_FAIL;
		}
		node_context* node_ctx = new node_context;
		node_ctx->fd = listen_fd;
		node_ctx->role = Role_Listener;
        node_ctx->protocol = protocol_id;

		// 对指定端口进行监听
		sockaddr_in server_addr;
		memset((char*)&server_addr, 0, sizeof(server_addr));
		server_addr.sin_family = AF_INET;
		inet_pton(AF_INET, addr.c_str(), &server_addr.sin_addr);
		server_addr.sin_port = htons(port);
		if ((SOCKET_ERROR == bind(listen_fd, (sockaddr*)&server_addr, sizeof(server_addr)))
			|| (SOCKET_ERROR == listen(listen_fd, SOMAXCONN))
			|| (NULL == CreateIoCompletionPort((HANDLE)listen_fd, _iocp, (ULONG_PTR)node_ctx, 0))
			)
		{
			close_node(node_ctx);
			return IOCP_ERROR_FAIL;
		}

		if (_acceptex == NULL || _get_acceptex_sock_addrs == NULL) {
			DWORD t;
			// 获取AcceptEx和GetAcceptExSockAddrs的地址
			GUID get_who = WSAID_ACCEPTEX;
			int ret = WSAIoctl(listen_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
				&get_who, sizeof(get_who),
				&_acceptex, sizeof(_acceptex),
				&t, NULL, NULL);
			if (SOCKET_ERROR == ret) {
				close_node(node_ctx);
				return IOCP_ERROR_FAIL;
			}
			get_who = WSAID_GETACCEPTEXSOCKADDRS;
			ret = WSAIoctl(listen_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
				&get_who, sizeof(get_who),
				&_get_acceptex_sock_addrs, sizeof(_get_acceptex_sock_addrs),
				&t, NULL, NULL);
			if (SOCKET_ERROR == ret) {
				close_node(node_ctx);
				return IOCP_ERROR_FAIL;
			}
		}
		{
			std::lock_guard<std::mutex> lock(_node_mtx);
			_nodes[listen_fd] = node_ctx;
		}
		IO_context* io_ctx = node_ctx->create_new_io();
		io_ctx->io_type = IO_ACCEPT;
		io_ctx->wsa_buff.buf = new char[(sizeof(sockaddr_in) + 16) * 2];
        io_ctx->wsa_buff.len = (sizeof(sockaddr_in) + 16) * 2;
		post_accept(node_ctx, io_ctx);
        return 0;
	}

    int iocp_driver::connect_to(const std::string& addr, int port, int protocol_id) {
		if(protocol_id < 0 || protocol_id >= _protocols.size()){
            // 未知协议，不知道该怎么处理通讯
            return IOCP_ERROR_INVALID_PROTOCOL;
        }
        SOCKET connect_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == connect_fd) {
			return IOCP_ERROR_FAIL;
		}

        node_context* node_ctx = new node_context;
		node_ctx->fd = connect_fd;
		node_ctx->role = Role_Customer;
        node_ctx->protocol = protocol_id;

        if(_connectex == NULL){
            DWORD t;
            GUID get_who = WSAID_CONNECTEX;
            int ret = WSAIoctl(connect_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &get_who, sizeof(get_who),
                &_connectex, sizeof(_connectex),
                &t, NULL, NULL);
            if (SOCKET_ERROR == ret) {
                close_node(node_ctx);
                return IOCP_ERROR_FAIL;
            }
        }

        if(NULL == CreateIoCompletionPort((HANDLE)connect_fd, _iocp, (ULONG_PTR)node_ctx, 0)){
            close_node(node_ctx);
            return -1;
        }

		// 对指定端口进行监听
		sockaddr_in server_addr;
		memset((char*)&server_addr, 0, sizeof(server_addr));
		server_addr.sin_family = AF_INET;
		inet_pton(AF_INET, addr.c_str(), &server_addr.sin_addr);
		server_addr.sin_port = htons(port);

		sockaddr_in local_addr;
		memset((char*)&local_addr, 0, sizeof(local_addr));
		local_addr.sin_family = AF_INET;
		local_addr.sin_addr.S_un.S_addr = INADDR_ANY;
		local_addr.sin_port = 0;
		
		if (SOCKET_ERROR == bind(connect_fd, (sockaddr*)&local_addr, sizeof(local_addr))) {
			close_node(node_ctx);
		}

		{
			std::lock_guard<std::mutex> lock(_node_mtx);
			_nodes[connect_fd] = node_ctx;
		}

        IO_context* io_ctx = node_ctx->create_new_io();
		io_ctx->io_type = IO_CONNECTING;
		io_ctx->fd = connect_fd;
        memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
		if(!_connectex(connect_fd, (sockaddr*)&server_addr, sizeof(server_addr), NULL, 0, NULL, &io_ctx->overlapped)){
			int ttt = WSAGetLastError();
			if (WSA_IO_PENDING != WSAGetLastError()) {
				node_ctx->erase_io(io_ctx);
				close_node(node_ctx);
				return -1;
			}
        }

        return 0;
    }

	void iocp_driver::send_data(SOCKET fd, unsigned char* buff, int len) {
		std::lock_guard<std::mutex> lock(_node_mtx);
		if (_nodes.find(fd) == _nodes.end()) {
			return;
		}
		node_context* node_ctx = _nodes[fd];

        IO_context* io_ctx = node_ctx->create_new_io();
		io_ctx->fd = node_ctx->fd;
		io_ctx->io_type = IO_SEND;
		io_ctx->wsa_buff.buf = new char[len];
		io_ctx->wsa_buff.len = len;
		memcpy(io_ctx->wsa_buff.buf, buff, len);
		post_send(node_ctx, io_ctx);
	}

	void iocp_driver::send_data_raw(SOCKET fd, unsigned char* buff, int len) {
		std::lock_guard<std::mutex> lock(_node_mtx);
		if (_nodes.find(fd) == _nodes.end()) {
			delete[] buff;
			return;
		}
		node_context* node_ctx = _nodes[fd];

        IO_context* io_ctx = node_ctx->create_new_io();
		io_ctx->fd = node_ctx->fd;
		io_ctx->io_type = IO_SEND;
		io_ctx->wsa_buff.buf = (char*)buff;
		io_ctx->wsa_buff.len = len;
		post_send(node_ctx, io_ctx);
	}

	void iocp_driver::close_node(SOCKET fd) {
		PostQueuedCompletionStatus(_iocp, (DWORD)(fd), (DWORD)IOCP_CLOSE_NODE, NULL);
	}

    int iocp_driver::post_accept(node_context* node_ctx, IO_context* io_ctx){
        memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
		io_ctx->fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
        if(INVALID_SOCKET == io_ctx->fd){
			close_node(node_ctx);
            return -1;
        }
		
        DWORD dw_t;
		if (!_acceptex(node_ctx->fd, io_ctx->fd, io_ctx->wsa_buff.buf, 0, sizeof(sockaddr_in) + 16,
			sizeof(sockaddr_in) + 16, &dw_t, &io_ctx->overlapped)) {
			if (WSA_IO_PENDING != WSAGetLastError()) {
				close_node(node_ctx);
				return -1;
			}
		}
        return 0;
    }

	int iocp_driver::post_recv(node_context* node_ctx, IO_context* io_ctx) {
		memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
		
		DWORD flag = 0;
		if (SOCKET_ERROR == WSARecv(node_ctx->fd, &(io_ctx->wsa_buff), 1, NULL, &flag, &(io_ctx->overlapped), NULL)) {
			if (ERROR_IO_PENDING != WSAGetLastError()) {
				// 出现未预料到的情况
				close_node(node_ctx);
				return -1;
			}
		}
		return 0;
	}

	int iocp_driver::post_send(node_context* node_ctx, IO_context* io_ctx) {
		memset((char*)&io_ctx->overlapped, 0, sizeof(OVERLAPPED));
		
		if (SOCKET_ERROR == WSASend(node_ctx->fd, &io_ctx->wsa_buff, 1, NULL, 0, &io_ctx->overlapped, NULL)) {
			if (ERROR_IO_PENDING != WSAGetLastError()) {
				// 出现未预料到的情况
				close_node(node_ctx);
				return -1;
			}
		}
	}

	void iocp_driver::work() {
		DWORD bytes;
		node_context* node_ctx;
		IO_context* io_ctx;
		OVERLAPPED* ol;
		while (true) {
			bool ok = GetQueuedCompletionStatus(_iocp, &bytes, (PULONG_PTR)&node_ctx, &ol, INFINITE);
			io_ctx = CONTAINING_RECORD(ol, IO_context, overlapped);
			
			if (IOCP_EXIT == (DWORD)node_ctx){
				break;
			}
			if (IOCP_CLOSE_NODE == (DWORD)node_ctx) {
				SOCKET fd = (SOCKET)bytes;
				if (INVALID_SOCKET != fd)
					closesocket(fd);
				continue;
			}

			if (!ok) {
				DWORD err = GetLastError();
				if (WAIT_TIMEOUT == err) {
					// 暂不使用心跳
					continue;
				} else if (ERROR_NETNAME_DELETED == err) {
					close_node(node_ctx);
					continue;
				} else {
					// 遇到其他异常
					close_node(node_ctx);
					continue;
				}
			}
			if (0 == bytes && (IO_RECV == io_ctx->io_type || IO_SEND == io_ctx->io_type)) {
				// 客户端close
                close_node(node_ctx);
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

    void iocp_driver::on_accept(node_context* node_ctx, IO_context* io_ctx) {
		SOCKADDR_IN* remote = NULL;
		SOCKADDR_IN* local = NULL;
		int len_r = sizeof(SOCKADDR_IN);
		int len_l = sizeof(SOCKADDR_IN);
		_get_acceptex_sock_addrs(io_ctx->wsa_buff.buf, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
			(LPSOCKADDR*)&local, &len_l, (LPSOCKADDR*)&remote, &len_r);
		
		node_context* n = new node_context;
		n->fd = io_ctx->fd;
		n->role = Role_Customer;
        n->protocol = node_ctx->protocol;
		
		if (NULL == CreateIoCompletionPort((HANDLE)io_ctx->fd, _iocp, (ULONG_PTR)n, 0)) {
			close_node(n);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(_node_mtx);
			_nodes[n->fd] = n;
		}

        IOCP_OnConnectCB connect_cb;
        if(n->protocol >= 0 && n->protocol < _protocols.size()){
			connect_cb = _protocols[n->protocol].connect_cb;
        }
		if (connect_cb) {
			if (0 != connect_cb(io_ctx->fd, remote->sin_addr, ntohs(remote->sin_port))) {
				close_node(n);
				return;
			}
		}

        {// 开始监听新连接的数据
            IO_context* io_ctx_ = n->create_new_io();
            io_ctx_->fd = n->fd;
            io_ctx_->io_type = IO_RECV;
            // 没有几个客户端，每个开辟1M缓存是可以接受的
            io_ctx_->wsa_buff.buf = new char[1000000];
            io_ctx_->wsa_buff.len = 1000000;
            post_recv(n, io_ctx_);
        }
        post_accept(node_ctx, io_ctx);
	}

	void iocp_driver::on_connecting(node_context* node_ctx, IO_context* io_ctx) {
		/*{
			std::lock_guard<std::mutex> lock(_node_mtx);
			_nodes[node_ctx->fd] = node_ctx;
		}*/
		IOCP_OnConnectCB connect_cb;
        if(node_ctx->protocol >= 0 && node_ctx->protocol < _protocols.size()){
			connect_cb = _protocols[node_ctx->protocol].connect_cb;
        }
		if (connect_cb) {
			connect_cb(node_ctx->fd, IN_ADDR{INADDR_ANY}, 0);
		}
		node_ctx->erase_io(io_ctx);
		{// 开始监听新连接的数据
			IO_context* io_ctx_ = node_ctx->create_new_io();
			io_ctx_->fd = node_ctx->fd;
			io_ctx_->io_type = IO_RECV;
			// 没有几个客户端，每个开辟1M缓存是可以接受的
			io_ctx_->wsa_buff.buf = new char[1000000];
			io_ctx_->wsa_buff.len = 1000000;
			post_recv(node_ctx, io_ctx_);
		}
	}

	void iocp_driver::on_recv(node_context* node_ctx, IO_context* io_ctx, int bytes) {
		int remain = node_ctx->total_len - node_ctx->cur_len;
		if (remain < bytes) {   // 当前缓存不够，需要重新分配更大的空间
            int new_size = node_ctx->total_len * 2;
            if(new_size == 0) new_size = 1000;
            while(new_size < (node_ctx->cur_len + bytes))
                new_size *= 2;
			unsigned char* new_buff = new unsigned char[new_size];
			if (node_ctx->buff) {
				memcpy(new_buff, node_ctx->buff, node_ctx->cur_len);
				delete[] node_ctx->buff;
			}
			node_ctx->buff = new_buff;
			node_ctx->total_len = new_size;
		}
        // 将新接收到的数据存入缓冲区
		memcpy(node_ctx->buff + node_ctx->cur_len, io_ctx->wsa_buff.buf, bytes);
		node_ctx->cur_len = node_ctx->cur_len + bytes;
		IOCP_OnRecvCB recv_cb;
        if(node_ctx->protocol >= 0 && node_ctx->protocol < _protocols.size()){
            recv_cb = _protocols[node_ctx->protocol].recv_cb;
        }
        if (recv_cb) {
            // 如果应用层有处理函数，则通知应用层处理
			int recv_len = recv_cb(node_ctx->fd, node_ctx->buff, node_ctx->cur_len);
			if (recv_len > 0) {
                // 应用层如果读到有效数据，会返回读走的长度，此时需要将读走的数据从缓冲区中移除
				if (recv_len < node_ctx->cur_len) {
					memcpy(node_ctx->buff, node_ctx->buff + recv_len, node_ctx->cur_len - recv_len);
					node_ctx->cur_len -= recv_len;
				} else {
					node_ctx->cur_len = 0;
				}
			}
		} else {
			// 否则，数据直接丢弃。应该要报个错
			node_ctx->cur_len = 0;
		}
		post_recv(node_ctx, io_ctx);
	}

	void iocp_driver::on_send(node_context* node_ctx, IO_context* io_ctx) {
		// 暂时不需要进行特殊处理
		node_ctx->erase_io(io_ctx);
	}

	void iocp_driver::close_node(node_context* node_ctx) {
		SOCKET fd = node_ctx->fd;
		if (INVALID_SOCKET != fd)
			closesocket(fd);
		if (node_ctx->buff != nullptr) {
			delete[] node_ctx->buff;
			node_ctx->buff = nullptr;
		}
		{
			std::lock_guard<std::mutex> lock(_node_mtx);
			_nodes.erase(fd);
		}
        
		if (node_ctx->protocol >= 0 && node_ctx->protocol < _protocols.size()) {
			IOCP_OnCloseCB close_cb = _protocols[node_ctx->protocol].close_cb;
			if (close_cb)
				close_cb(fd);
		}
		delete node_ctx;
	}



	







	










}