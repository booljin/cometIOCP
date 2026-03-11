/*
 * cometIOCP - WSA 辅助工具类
 * 
 * RAII 风格的 WSA 初始化/清理
 */

#ifndef __COMET_UTIL_H__
#define __COMET_UTIL_H__

#include <WinSock2.h>

namespace comet_iocp {

// ============================================================================
// WSAInitializer - WSA 辅助工具类（RAII）
// 
// 用法：在 main 函数开头创建 WSAInitializer 实例，程序退出时自动清理
// 
// 示例：
//   int main() {
//       comet_iocp::WSAInitializer wsa;
//       if (!wsa.is_ok()) {
//           printf("WSAStartup failed: %d\n", wsa.get_error());
//           return -1;
//       }
//       // ... 使用 Driver ...
//       return 0;
//   }
// ============================================================================

class WSAInitializer {
public:
    explicit WSAInitializer(WORD version = MAKEWORD(2, 2)) 
        : _error(WSAStartup(version, &_wsa_data)) {
    }
    
    ~WSAInitializer() {
        if (_error == 0) {
            WSACleanup();
        }
    }
    
    // 禁止拷贝和移动（RAII 资源独占）
    WSAInitializer(const WSAInitializer&) = delete;
    WSAInitializer& operator=(const WSAInitializer&) = delete;
    WSAInitializer(WSAInitializer&&) = delete;
    WSAInitializer& operator=(WSAInitializer&&) = delete;
    
    // 检查初始化是否成功
    bool is_ok() const { return _error == 0; }
    
    // 获取错误码（0 表示成功，否则为 WSAGetLastError 值）
    int get_error() const { return _error; }
    
    // 获取 WSA 版本信息
    const WSADATA& get_data() const { return _wsa_data; }

private:
    WSADATA _wsa_data;
    int _error;
};

} // namespace comet_iocp

#endif // __COMET_UTIL_H__