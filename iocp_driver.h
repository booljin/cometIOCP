#ifndef __IOCP_DRIVER_H__
#define __IOCP_DRIVER_H__

#include <WinSock2.h>
#include <mswsock.h>

#include <map>
#include <vector>
#include <thread>
#include <functional>
#include <mutex>
#include <string>

using IOCP_OnConnectCB = std::function<int(SOCKET fd, IN_ADDR addr, int port) >;
using IOCP_OnRecvCB = std::function<int(SOCKET fd, unsigned char* buff, int len)>;
using IOCP_OnCloseCB = std::function<void(SOCKET fd)>;
using IOCP_MonitorCB = std::function<void(int info, const std::string& msg)>;

namespace IOCP_DRIVER{


// 通讯节点
struct node_context;
// 读写上下文
struct IO_context;
// 协议
struct protocol;

class iocp_driver{
public:
    iocp_driver();
	virtual ~iocp_driver();

public:
	// 启动工作线程
	void run();
	// 停止工作线程，一般不需要手动调用
	void stop();

public:
	// 注册监视器，用以非侵入式的监控iocp驱动器内部状态
	void regist_monitor(IOCP_MonitorCB handle);
	// 注册协议，返回协议ID
	int regist_protocol(IOCP_OnConnectCB ah, IOCP_OnRecvCB rh, IOCP_OnCloseCB ch);
	// 添加一个监听端口，后续连接关联指定协议
	int listen_from(const std::string& addr, int port, int protocol_id);
	// 连接到制定地址,关联指定协议
	int connect_to(const std::string& addr, int port, int protocol_id);
	// 往对应节点发送指定信息。应用层知道数据格式，所以这里不需要区分协议
	void send_data(SOCKET fd, unsigned char* buff, int len);
	// 与上一接口的区别是不会复制数据，直接使用数据块内存地址。发送端不可自己回收内存
	void send_data_raw(SOCKET fd, unsigned char* buff, int len);
	// 引用层主动关闭一个连接
	void close_node(SOCKET fd);
protected:
	// 往iocp模型投递对应消息
	int post_accept(node_context* node_ctx, IO_context* io_ctx); 
	int post_recv(node_context* node_ctx, IO_context* io_ctx);
	int post_send(node_context* node_ctx, IO_context* io_ctx);

protected:
	// 工作线程
	void work();
	void on_accept(node_context*, IO_context*);
	void on_connecting(node_context*, IO_context*);
	void on_recv(node_context*, IO_context*, int len);
	void on_send(node_context*, IO_context*);

protected:
	void close_node(node_context*);


protected:
	HANDLE _iocp;
	std::thread _work_thread;
protected:
    std::map<SOCKET, node_context*> _nodes;
	std::mutex _node_mtx;

	std::vector<protocol> _protocols;	
	IOCP_MonitorCB _monitor{ NULL };
private:
	LPFN_ACCEPTEX _acceptex{ NULL };
	LPFN_GETACCEPTEXSOCKADDRS _get_acceptex_sock_addrs{ NULL };
	LPFN_CONNECTEX _connectex{ NULL };
};


enum IOCP_ErrorCode{
	IOCP_SUCCESS = 0,
	IOCP_ERROR_FAIL = -1,
	IOCP_ERROR_INVALID_PARAMETER = -2,
	IOCP_ERROR_INVALID_SOCKET = -3,
	IOCP_ERROR_INVALID_PROTOCOL = -4,
	IOCP_ERROR_INVALID_CONTEXT = -5,
	IOCP_ERROR_INVALID_BUFFER = -6,
	IOCP_ERROR_INVALID_LENGTH = -7,
};




}


#endif // __IOCP_DRIVER_H__




//  #include <iostream> #include <winsock2.h> 
//  #pragma comment(lib, "ws2_32.lib") static DWORD WINAPI ClientWorkerThread(LPVOID lpParameter); t
//  ypedef struct _PER_HANDLE_DATA { SOCKET Socket; } PER_HANDLE_DATA, * LPPER_HANDLE_DATA; 
//  typedef struct { WSAOVERLAPPED wsaOverlapped; WSABUF wsaBuf; int OperationType; } PER_IO_DATA, * LPPER_IO_DATA; 
//  int main(void) { WSADATA WsaDat; WSAStartup(MAKEWORD(2, 2), &WsaDat); // Step 1 - Create an I/O completion port. 
// 	HANDLE hCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0); // Step 2 - Find how many processors. SYSTEM_INFO systemInfo; 
// 	GetSystemInfo(&systemInfo); // Step 3 - Create worker threads. 
// 	for (int i = 0; i < (int)systemInfo.dwNumberOfProcessors; i++) { HANDLE hThread = 
// 		CreateThread(NULL, 0, ClientWorkerThread, hCompletionPort, 0, NULL); CloseHandle(hThread); } // Step 4 - Create a socket. 
// 		SOCKET Socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED); 
// 		PER_HANDLE_DATA *pPerHandleData = new PER_HANDLE_DATA; 
// 		pPerHandleData->Socket = Socket; struct hostent *host; host = gethostbyname("localhost"); 
// 		SOCKADDR_IN SockAddr; SockAddr.sin_family = AF_INET; 
// 		SockAddr.sin_addr.s_addr = *((unsigned long*)host->h_addr); SockAddr.sin_port = htons(8888); 
// 		// Step 5 - Associate the socket with the I/O completion port. 
// 		CreateIoCompletionPort((HANDLE)Socket, hCompletionPort, (DWORD)pPerHandleData, 0); 
// 		WSAConnect(Socket, (SOCKADDR*)(&SockAddr), sizeof(SockAddr), NULL, NULL, NULL, NULL); static char buffer[1000]; 
// 		memset(buffer, 0, 999); PER_IO_DATA *pPerIoData = new PER_IO_DATA; pPerIoData->wsaBuf.buf = buffer; 
// 		pPerIoData->wsaBuf.len = sizeof(buffer); DWORD dwSendBytes = 0; DWORD dwReceivedBytes = 0; DWORD dwFlags = 0; 
// 		SecureZeroMemory((PVOID)&pPerIoData->wsaOverlapped, sizeof(pPerIoData->wsaOverlapped)); 
// 		pPerIoData->wsaOverlapped.hEvent = WSACreateEvent(); 
// 		WSARecv(Socket, &pPerIoData->wsaBuf, 1, &dwReceivedBytes, &dwFlags, &pPerIoData->wsaOverlapped, NULL); 
// 		std::cout << pPerIoData->wsaBuf.buf; for (;;) { int nError = WSAGetLastError(); 
// 			if (nError != WSAEWOULDBLOCK&&nError != 0) { std::cout << "Winsock error code: " << nError << "\r\n"; 
// 				std::cout << "Server disconnected!\r\n"; shutdown(Socket, SD_SEND); closesocket(Socket); break; } Sleep(1000); } 
// 				delete pPerHandleData; delete pPerIoData; WSACleanup(); return 0; } 
// 				static DWORD WINAPI ClientWorkerThread(LPVOID lpParameter) { HANDLE hCompletionPort = (HANDLE)lpParameter; 
// 					DWORD bytesCopied = 0; OVERLAPPED *overlapped = 0; LPPER_HANDLE_DATA PerHandleData; LPPER_IO_DATA PerIoData; 
// 					DWORD SendBytes, RecvBytes; DWORD Flags; BOOL bRet; while (TRUE) { 
// 						bRet = GetQueuedCompletionStatus(hCompletionPort, &bytesCopied, (LPDWORD)&PerHandleData, (LPOVERLAPPED*)&PerIoData, INFINITE); 
// 						if (bytesCopied == 0) { break; } else { Flags = 0; ZeroMemory(&(PerIoData->wsaOverlapped), sizeof(WSAOVERLAPPED)); 
// 							PerIoData->wsaBuf.len = 1000; 
// 							WSARecv(PerHandleData->Socket, &(PerIoData->wsaBuf), 1, &RecvBytes, &Flags, &(PerIoData->wsaOverlapped), NULL); } } return 0; } 
