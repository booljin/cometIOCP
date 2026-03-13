/**
 * @file stress_test.cpp
 * @brief IOCP 功能正确性测试程序
 * 
 * 测试目标：
 *   验证 IOCP 库在各种场景下的数据收发正确性
 * 
 * 测试内容：
 *   1. 基本收发测试：S→C 和 C→S 方向，小/中/大数据
 *   2. 双向同时测试：双方同时发送数据
 *   3. 连续发送测试：测试 TCP 流式处理的正确性
 *   4. 边界条件测试：空数据、缓冲区边界
 *   5. 多客户端并发测试：多个客户端同时操作
 * 
 * 架构：
 *   单一测试程序，内嵌服务端和客户端
 *   使用固定种子的随机数据确保可复现性
 * 
 * 数据格式：
 *   [4字节 序号][4字节 长度][4字节 CRC32][N字节 数据]
 *   接收方解析并验证 CRC32，确保数据完整性
 */

#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>
#include <algorithm>
#include <map>
#include <functional>
#include "cometIOCP.h"

// ============================================================================
// 测试配置
// ============================================================================
struct TestConfig {
    std::string server_addr = "127.0.0.1";
    int server_port = 9089;             // 测试专用端口
    int server_threads = 2;             // 服务端 IOCP 线程数
    int client_threads = 2;             // 客户端 IOCP 线程数
    int repeat_count = 3;               // 每种用例重复次数
    int concurrent_clients = 5;         // 并发客户端数量
};

// ============================================================================
// 数据格式定义
// ============================================================================
/**
 * 测试消息格式
 * 
 * +--------+--------+--------+------------------+
 * | 序号   | 长度   | CRC32  | 数据             |
 * | 4字节  | 4字节  | 4字节  | N字节            |
 * +--------+--------+--------+------------------+
 * 
 * - 序号：消息序号，用于追踪和验证
 * - 长度：数据部分长度
 * - CRC32：数据部分的校验和
 * - 数据：随机生成的测试数据
 */
struct TestMessage {
    uint32_t seq;           // 序号
    uint32_t length;        // 数据长度
    uint32_t crc32;         // CRC32 校验
    std::vector<uint8_t> data;
    
    static constexpr size_t HEADER_SIZE = 12;  // 头部大小：seq + length + crc32
};

// ============================================================================
// CRC32 计算
// ============================================================================
/**
 * @brief 计算数据的 CRC32 校验和
 * @param data 数据指针
 * @param len 数据长度
 * @return CRC32 值
 * 
 * 使用标准 CRC32 多项式 0xEDB88320
 */
uint32_t calculate_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            // 使用条件表达式替代一元负运算符，避免 C4146 警告
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

// ============================================================================
// 测试数据生成器
// ============================================================================
/**
 * @brief 测试数据生成器
 * 
 * 使用固定种子的随机数生成器，确保测试可复现
 */
class TestDataGenerator {
public:
    /**
     * @brief 构造函数
     * @param seed 随机种子，相同种子生成相同数据
     */
    explicit TestDataGenerator(uint32_t seed) : rng_(seed) {}
    
    /**
     * @brief 生成测试消息
     * @param seq 消息序号
     * @param data_size 数据部分大小
     * @return 完整的测试消息
     */
    TestMessage generate(uint32_t seq, size_t data_size) {
        TestMessage msg;
        msg.seq = seq;
        msg.length = static_cast<uint32_t>(data_size);
        msg.data.resize(data_size);
        
        // 生成随机数据（注意：uniform_int_distribution 不支持 uint8_t，使用 int 代替）
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& byte : msg.data) {
            byte = static_cast<uint8_t>(dist(rng_));
        }
        
        // 计算 CRC32
        msg.crc32 = calculate_crc32(msg.data.data(), msg.data.size());
        
        return msg;
    }
    
    /**
     * @brief 将消息序列化为字节流
     * @param msg 测试消息
     * @return 字节流
     */
    static std::vector<uint8_t> serialize(const TestMessage& msg) {
        std::vector<uint8_t> buffer(HEADER_SIZE + msg.data.size());
        
        // 写入头部（大端序）
        buffer[0] = (msg.seq >> 24) & 0xFF;
        buffer[1] = (msg.seq >> 16) & 0xFF;
        buffer[2] = (msg.seq >> 8) & 0xFF;
        buffer[3] = msg.seq & 0xFF;
        
        buffer[4] = (msg.length >> 24) & 0xFF;
        buffer[5] = (msg.length >> 16) & 0xFF;
        buffer[6] = (msg.length >> 8) & 0xFF;
        buffer[7] = msg.length & 0xFF;
        
        buffer[8] = (msg.crc32 >> 24) & 0xFF;
        buffer[9] = (msg.crc32 >> 16) & 0xFF;
        buffer[10] = (msg.crc32 >> 8) & 0xFF;
        buffer[11] = msg.crc32 & 0xFF;
        
        // 写入数据
        if (!msg.data.empty()) {
            std::copy(msg.data.begin(), msg.data.end(), buffer.begin() + HEADER_SIZE);
        }
        
        return buffer;
    }
    
    /**
     * @brief 从缓冲区解析消息（不消费数据）
     * @param buffer 缓冲区
     * @param len 缓冲区长度
     * @param out_msg 输出消息
     * @return 解析的消息总长度，0 表示数据不足，-1 表示解析错误
     */
    static int parse(const uint8_t* buffer, size_t len, TestMessage& out_msg) {
        // 检查是否有足够头部
        if (len < HEADER_SIZE) {
            return 0;
        }
        
        // 解析头部
        out_msg.seq = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
        out_msg.length = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
        out_msg.crc32 = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
        
        // 检查数据长度是否合理（最大 1MB）
        if (out_msg.length > 1024 * 1024) {
            return -1;
        }
        
        // 检查是否有足够数据
        size_t total_len = HEADER_SIZE + out_msg.length;
        if (len < total_len) {
            return 0;
        }
        
        // 复制数据
        out_msg.data.assign(buffer + HEADER_SIZE, buffer + total_len);
        
        return static_cast<int>(total_len);
    }
    
    static constexpr size_t HEADER_SIZE = 12;
    
private:
    std::mt19937 rng_;  // 梅森旋转算法，质量好且可复现
};

// ============================================================================
// 测试结果统计
// ============================================================================
/**
 * @brief 测试结果统计器
 * 
 * 线程安全地记录测试结果
 */
class TestResults {
public:
    /**
     * @brief 记录测试通过
     * @param test_name 测试名称
     * @param detail 详情
     */
    void pass(const std::string& test_name, const std::string& detail = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        passed_++;
        std::cout << "[PASS] " << test_name;
        if (!detail.empty()) {
            std::cout << " - " << detail;
        }
        std::cout << "\n";
    }
    
    /**
     * @brief 记录测试失败
     * @param test_name 测试名称
     * @param reason 失败原因
     */
    void fail(const std::string& test_name, const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_++;
        std::cout << "[FAIL] " << test_name << " - " << reason << "\n";
    }
    
    /**
     * @brief 打印最终报告
     */
    void print_report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "\n========== 测试结果 ==========\n";
        std::cout << "通过: " << passed_ << "\n";
        std::cout << "失败: " << failed_ << "\n";
        if (failed_ == 0) {
            std::cout << "状态: ✓ 所有测试通过\n";
        } else {
            std::cout << "状态: ✗ 存在失败的测试\n";
        }
        std::cout << "==============================\n";
    }
    
    bool all_passed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_ == 0;
    }
    
private:
    mutable std::mutex mutex_;
    int passed_ = 0;
    int failed_ = 0;
};

// ============================================================================
// 测试服务端
// ============================================================================
/**
 * @brief 测试服务端
 * 
 * 内嵌于测试程序中，接收客户端数据并验证
 * 同时也能主动发送测试数据给客户端
 */
class TestServer {
public:
    /**
     * @brief 构造函数
     * @param config 测试配置
     * @param results 测试结果记录器
     */
    TestServer(const TestConfig& config, TestResults& results)
        : config_(config), results_(results) {}
    
    /**
     * @brief 启动服务端
     * @return 成功返回 0
     */
    int start() {
        // 注册协议：处理客户端发送的数据
        server_proto_id_ = driver_.register_protocol(
            // 连接回调
            [this](SOCKET fd, const comet_iocp::AddressInfo& addr) {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_[fd] = ClientState{};
                // 如果有待发送的测试数据，立即发送
                if (pending_test_data_.valid && !pending_test_data_.sent) {
                    auto data = TestDataGenerator::serialize(pending_test_data_.msg);
                    driver_.send_data(fd, data.data(), static_cast<int>(data.size()));
                    pending_test_data_.sent = true;
                }
                return 0;
            },
            // 接收回调：验证客户端发来的数据
            [this](SOCKET fd, uint8_t* buff, int len) {
                return on_client_data(fd, buff, len);
            },
            // 关闭回调
            [this](SOCKET fd) {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.erase(fd);
            }
        );
        
        // 启动监听
        if (driver_.listen_on(config_.server_addr, config_.server_port, server_proto_id_) != 0) {
            results_.fail("Server start", "监听端口失败");
            return -1;
        }
        
        // 运行 IOCP
        driver_.run(config_.server_threads);
        
        return 0;
    }
    
    /**
     * @brief 停止服务端
     */
    void stop() {
        driver_.stop();
    }
    
    /**
     * @brief 获取协议 ID（供外部发送数据使用）
     */
    int get_protocol_id() const { return server_proto_id_; }
    
    /**
     * @brief 向指定客户端发送测试数据
     * @param fd 客户端 socket
     * @param msg 测试消息
     */
    void send_to_client(SOCKET fd, const TestMessage& msg) {
        auto data = TestDataGenerator::serialize(msg);
        driver_.send_data(fd, data.data(), static_cast<int>(data.size()));
    }
    
    /**
     * @brief 等待所有客户端验证完成
     */
    void wait_for_verifications(int expected_count, int timeout_seconds = 10) {
        auto start = std::chrono::steady_clock::now();
        while (verification_count_ < expected_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= timeout_seconds) {
                break;
            }
        }
    }
    
    /**
     * @brief 重置验证计数
     */
    void reset_verification_count() {
        verification_count_ = 0;
    }
    
    /**
     * @brief 获取底层驱动（供高级测试使用）
     */
    comet_iocp::Driver& get_driver() { return driver_; }
    
private:
    /**
     * @brief 处理客户端数据（验证）
     */
    int on_client_data(SOCKET fd, uint8_t* buff, int len) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return len;
        }
        
        ClientState& state = it->second;
        
        // 将数据追加到缓冲区
        state.buffer.insert(state.buffer.end(), buff, buff + len);
        
        // 尝试解析并验证消息
        while (true) {
            TestMessage msg;
            int parsed = TestDataGenerator::parse(state.buffer.data(), state.buffer.size(), msg);
            
            if (parsed == 0) {
                // 数据不足，等待更多数据
                break;
            } else if (parsed < 0) {
                // 解析错误
                results_.fail("Server parse", "无效的消息格式");
                state.buffer.clear();
                break;
            }
            
            // 验证 CRC32
            uint32_t expected_crc = calculate_crc32(msg.data.data(), msg.data.size());
            if (msg.crc32 != expected_crc) {
                results_.fail("Server verify", 
                    "CRC32 不匹配: 期望 " + std::to_string(expected_crc) + 
                    ", 实际 " + std::to_string(msg.crc32));
            } else {
                // 验证通过，生成测试名称
                std::string test_name = "C-S msg #" + std::to_string(msg.seq);
                std::string detail = std::to_string(msg.length) + " bytes";
                results_.pass(test_name, detail);
                verification_count_++;
            }
            
            // 移除已处理的数据
            state.buffer.erase(state.buffer.begin(), state.buffer.begin() + parsed);
        }
        
        return len;
    }
    
private:
    const TestConfig& config_;
    TestResults& results_;
    comet_iocp::Driver driver_;
    int server_proto_id_ = -1;
    
    // 客户端状态
    struct ClientState {
        std::vector<uint8_t> buffer;  // 接收缓冲区
    };
    std::map<SOCKET, ClientState> clients_;
    std::mutex clients_mutex_;
    
    std::atomic<int> verification_count_{0};
    
    // 待发送的测试数据（用于 S→C 测试）
    struct PendingTestData {
        TestMessage msg;
        bool valid = false;
        bool sent = false;
    } pending_test_data_;
    
public:
    /**
     * @brief 设置待发送的测试数据（在客户端连接后自动发送）
     */
    void set_pending_test_data(const TestMessage& msg) {
        pending_test_data_.msg = msg;
        pending_test_data_.valid = true;
        pending_test_data_.sent = false;
    }
    
    /**
     * @brief 清除待发送的测试数据
     */
    void clear_pending_test_data() {
        pending_test_data_.valid = false;
        pending_test_data_.sent = false;
    }
};

// ============================================================================
// 测试客户端
// ============================================================================
/**
 * @brief 测试客户端
 * 
 * 连接到服务端，发送测试数据并验证收到的数据
 */
class TestClient {
public:
    /**
     * @brief 构造函数
     * @param config 测试配置
     * @param results 测试结果记录器
     * @param client_id 客户端 ID（用于区分多个客户端）
     */
    TestClient(const TestConfig& config, TestResults& results, int client_id = 0)
        : config_(config), results_(results), client_id_(client_id) {}
    
    /**
     * @brief 连接到服务端
     * @return 成功返回 socket，失败返回 INVALID_SOCKET
     */
    SOCKET connect() {
        // 注册协议
        client_proto_id_ = driver_.register_protocol(
            // 连接回调：保存 fd 并通知连接成功
            [this](SOCKET fd, const comet_iocp::AddressInfo& addr) {
                fd_ = fd;  // 保存 socket fd
                connected_ = true;
                cv_.notify_one();
                return 0;
            },
            // 接收回调：验证服务端发来的数据
            [this](SOCKET fd, uint8_t* buff, int len) {
                return on_server_data(fd, buff, len);
            },
            // 关闭回调
            [this](SOCKET fd) {
                connected_ = false;
            }
        );
        
        // 启动 IOCP
        driver_.run(config_.client_threads);
        
        // 发起连接
        if (driver_.connect_to(config_.server_addr, config_.server_port, client_proto_id_) != 0) {
            results_.fail("Client connect", "连接失败");
            return INVALID_SOCKET;
        }
        
        // 等待连接建立
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(5), [this] { return connected_.load(); });
        
        if (!connected_) {
            results_.fail("Client connect", "连接超时");
            return INVALID_SOCKET;
        }
        
        // 返回保存的 socket fd
        return fd_;
    }
    
    /**
     * @brief 发送测试消息
     * @param fd 目标 socket
     * @param msg 测试消息
     */
    void send(SOCKET fd, const TestMessage& msg) {
        auto data = TestDataGenerator::serialize(msg);
        driver_.send_data(fd, data.data(), static_cast<int>(data.size()));
    }
    
    /**
     * @brief 等待验证完成
     */
    void wait_for_verifications(int expected_count, int timeout_seconds = 10) {
        auto start = std::chrono::steady_clock::now();
        while (verification_count_ < expected_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= timeout_seconds) {
                break;
            }
        }
    }
    
    /**
     * @brief 重置验证计数
     */
    void reset_verification_count() {
        verification_count_ = 0;
    }
    
    /**
     * @brief 停止客户端
     */
    void stop() {
        driver_.stop();
    }
    
    /**
     * @brief 获取底层驱动
     */
    comet_iocp::Driver& get_driver() { return driver_; }
    
    /**
     * @brief 获取客户端 ID
     */
    int get_client_id() const { return client_id_; }
    
private:
    /**
     * @brief 处理服务端数据（验证）
     */
    int on_server_data(SOCKET fd, uint8_t* buff, int len) {
        // 将数据追加到缓冲区
        buffer_.insert(buffer_.end(), buff, buff + len);
        
        // 尝试解析并验证消息
        while (true) {
            TestMessage msg;
            int parsed = TestDataGenerator::parse(buffer_.data(), buffer_.size(), msg);
            
            if (parsed == 0) {
                break;
            } else if (parsed < 0) {
                results_.fail("Client parse", "无效的消息格式");
                buffer_.clear();
                break;
            }
            
            // 验证 CRC32
            uint32_t expected_crc = calculate_crc32(msg.data.data(), msg.data.size());
            if (msg.crc32 != expected_crc) {
                results_.fail("Client verify",
                    "CRC32 不匹配: 期望 " + std::to_string(expected_crc) +
                    ", 实际 " + std::to_string(msg.crc32));
            } else {
                std::string test_name = "S-C msg #" + std::to_string(msg.seq) + 
                    " (client " + std::to_string(client_id_) + ")";
                std::string detail = std::to_string(msg.length) + " bytes";
                results_.pass(test_name, detail);
                verification_count_++;
            }
            
            // 移除已处理的数据
            buffer_.erase(buffer_.begin(), buffer_.begin() + parsed);
        }
        
        return len;
    }
    
private:
    const TestConfig& config_;
    TestResults& results_;
    int client_id_;
    comet_iocp::Driver driver_;
    int client_proto_id_ = -1;
    
    std::atomic<bool> connected_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    
    std::vector<uint8_t> buffer_;  // 接收缓冲区
    std::atomic<int> verification_count_{0};
    SOCKET fd_ = INVALID_SOCKET;   // 保存的 socket fd
};

// ============================================================================
// 测试协调器
// ============================================================================
/**
 * @brief 测试协调器
 * 
 * 统一管理服务端和客户端，执行测试用例
 */
class TestCoordinator {
public:
    TestCoordinator(const TestConfig& config)
        : config_(config), results_(), server_(config, results_) {}
    
    /**
     * @brief 运行所有测试
     */
    void run_all_tests() {
        // 立即刷新输出，便于调试
        std::cout.setf(std::ios::unitbuf);
        
        std::cout << "========== IOCP 功能正确性测试 ==========\n";
        std::cout << "服务端端口: " << config_.server_port << "\n";
        std::cout << "重复次数: " << config_.repeat_count << "\n";
        std::cout << "========================================\n\n";
        
        // 启动服务端
        if (server_.start() != 0) {
            results_.fail("Server", "启动失败");
            results_.print_report();
            return;
        }
        
        // 等待服务端就绪
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 定义数据大小
        struct DataSize {
            const char* name;
            size_t size;
        };
        DataSize sizes[] = {
            {"S", 64},          // 小：64 字节
            {"M", 4096},        // 中：4KB
            {"L", 65536},       // 大：64KB
        };
        
        int test_round = 0;
        
        // ========================================
        // 测试 1：基本收发测试 (C→S)
        // ========================================
        std::cout << "\n--- 测试 1：客户端发送到服务端 (C→S) ---\n";
        
        for (int repeat = 0; repeat < config_.repeat_count; repeat++) {
            for (const auto& size : sizes) {
                test_round++;
                
                // 创建客户端
                TestClient client(config_, results_, test_round);
                SOCKET fd = client.connect();
                if (fd == INVALID_SOCKET) continue;
                
                // 服务端重置验证计数
                server_.reset_verification_count();
                
                // 生成并发送测试数据
                TestDataGenerator gen(test_round * 1000);
                TestMessage msg = gen.generate(test_round, size.size);
                client.send(fd, msg);
                
                // 等待服务端验证
                server_.wait_for_verifications(1);
                
                client.stop();
            }
        }
        
        // ========================================
        // 测试 2：基本收发测试 (S→C)
        // ========================================
        std::cout << "\n--- 测试 2：服务端发送到客户端 (S→C) ---\n";
        
        for (int repeat = 0; repeat < config_.repeat_count; repeat++) {
            for (const auto& size : sizes) {
                test_round++;
                
                // 先设置待发送的测试数据（客户端连接后会自动发送）
                TestDataGenerator gen(test_round * 1000);
                TestMessage msg = gen.generate(test_round, size.size);
                server_.set_pending_test_data(msg);
                
                TestClient client(config_, results_, test_round);
                client.reset_verification_count();
                
                SOCKET fd = client.connect();
                if (fd == INVALID_SOCKET) {
                    server_.clear_pending_test_data();
                    continue;
                }
                
                // 等待客户端验证
                client.wait_for_verifications(1);
                
                server_.clear_pending_test_data();
                client.stop();
            }
        }
        
        // ========================================
        // 测试 3：连续发送测试
        // ========================================
        std::cout << "\n--- 测试 3：连续发送测试 (10 条消息) ---\n";
        
        {
            TestClient client(config_, results_, ++test_round);
            SOCKET fd = client.connect();
            if (fd != INVALID_SOCKET) {
                server_.reset_verification_count();
                
                // 连续发送 10 条消息
                TestDataGenerator gen(test_round * 1000);
                for (int i = 0; i < 10; i++) {
                    TestMessage msg = gen.generate(test_round * 100 + i, 256);
                    client.send(fd, msg);
                }
                
                server_.wait_for_verifications(10, 15);
                client.stop();
            }
        }
        
        // ========================================
        // 测试 4：边界条件测试
        // ========================================
        std::cout << "\n--- 测试 4：边界条件测试 ---\n";
        
        // 测试 4.1：缓冲区边界大小（1024 字节）
        {
            TestClient client(config_, results_, ++test_round);
            SOCKET fd = client.connect();
            if (fd != INVALID_SOCKET) {
                server_.reset_verification_count();
                
                TestDataGenerator gen(test_round * 1000);
                TestMessage msg = gen.generate(test_round, 1024);  // 刚好 1KB
                client.send(fd, msg);
                
                server_.wait_for_verifications(1);
                client.stop();
            }
        }
        
        // ========================================
        // 测试 5：多客户端并发测试
        // ========================================
        std::cout << "\n--- 测试 5：多客户端并发测试 ---\n";
        
        {
            std::vector<std::unique_ptr<TestClient>> clients;
            std::vector<SOCKET> fds;
            
            // 创建多个客户端
            for (int i = 0; i < config_.concurrent_clients; i++) {
                auto client = std::make_unique<TestClient>(config_, results_, i + 1);
                SOCKET fd = client->connect();
                if (fd != INVALID_SOCKET) {
                    fds.push_back(fd);
                    clients.push_back(std::move(client));
                }
            }
            
            server_.reset_verification_count();
            
            // 所有客户端同时发送数据
            TestDataGenerator gen(50000);  // 固定种子
            for (size_t i = 0; i < fds.size(); i++) {
                TestMessage msg = gen.generate(static_cast<uint32_t>(i), 128);
                clients[i]->send(fds[i], msg);
            }
            
            // 等待所有验证完成
            server_.wait_for_verifications(static_cast<int>(fds.size()), 15);
            
            // 停止所有客户端
            for (auto& client : clients) {
                client->stop();
            }
        }
        
        // 停止服务端
        server_.stop();
        
        // 打印报告
        results_.print_report();
    }
    
    /**
     * @brief 检查是否所有测试通过
     */
    bool all_passed() const {
        return results_.all_passed();
    }
    
private:
    TestConfig config_;
    TestResults results_;
    TestServer server_;
};

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char* argv[]) {
    // 使用 RAII 工具初始化 Winsock（程序退出时自动清理）
    comet_iocp::WSAInitializer wsa;
    if (!wsa.is_ok()) {
        std::cerr << "WSAStartup 失败: " << wsa.get_error() << "\n";
        return 1;
    }
    
    // 解析命令行参数
    TestConfig config;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.server_port = std::stoi(argv[++i]);
        } else if ((arg == "-r" || arg == "--repeat") && i + 1 < argc) {
            config.repeat_count = std::stoi(argv[++i]);
        } else if ((arg == "-c" || arg == "--clients") && i + 1 < argc) {
            config.concurrent_clients = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "选项:\n"
                      << "  -p, --port <端口>     服务端端口 (默认: 9089)\n"
                      << "  -r, --repeat <次数>   每种用例重复次数 (默认: 3)\n"
                      << "  -c, --clients <数量>  并发客户端数 (默认: 5)\n"
                      << "  -h, --help            显示帮助\n";
            return 0;
        }
    }
    
    // 运行测试
    TestCoordinator coordinator(config);
    coordinator.run_all_tests();
    
    // 返回值：0 表示全部通过
    return coordinator.all_passed() ? 0 : 1;
}
