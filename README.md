# TaskScheduler - 高性能多线程任务调度器

## 概述

TaskScheduler 是一个基于 C++17 标准的高性能多线程任务调度系统。它采用了生产者-消费者模式，支持多种类型的任务并行处理，并提供了完整的统计和监控功能。

## 主要特性

### 🚀 高性能特性
- **线程安全队列**：使用自定义的 `ThreadSafeQueue` 模板类，提供 O(1) 的原子 size() 操作
- **多工作线程**：支持动态配置工作线程数量（默认：CPU 核心数）
- **负载均衡**：采用轮询（Round-Robin）算法公平分配任务
- **零拷贝优化**：使用移动语义减少不必要的拷贝操作

### 🛡️ 线程安全保证
- **原子操作**：关键数据结构使用适当的内存序（memory_order）优化
- **条件变量同步**：正确使用条件变量，避免虚假唤醒问题
- **RAII 设计**：自动资源管理，析构时安全清理
- **异常安全**：任务执行异常不影响调度器运行

### 📊 监控与统计
- **实时统计**：队列大小、任务数量、执行状态
- **性能指标**：成功率、平均执行时间
- **详细日志**：多级别日志系统（DEBUG/INFO/WARN/ERROR）

### 🔧 可扩展性
- **模板化队列**：`ThreadSafeQueue` 可用于任何类型
- **任务类型扩展**：通过继承 `XXXTask` 支持新任务类型
- **配置灵活**：工作线程数、轮询间隔、统计输出均可配置

## 架构设计

```
XXXTaskScheduler (任务调度器)
    ├── XXXWorker (工作线程) × N
    │   ├── ThreadSafeQueue A (线程安全队列A)
    │   ├── ThreadSafeQueue B (线程安全队列B)
    │   └── ThreadSafeQueue C (线程安全队列C)
    └── XXXTask (任务基类)
        ├── XXXTaskTypeA (int + string)
        ├── XXXTaskTypeB (double + vector<int>)
        └── XXXTaskTypeC (string + bool + int)
```

## 核心功能

### 三种任务类型
- **TypeA**：处理整数和字符串参数
- **TypeB**：处理浮点数和整型数组
- **TypeC**：处理字符串、布尔值和整数

### 三个独立队列
- 每种任务类型对应一个线程安全队列

### 多线程支持
- **生产者**：支持多线程同时提交任务
- **消费者**：工作线程池处理任务

### 调度策略
- 轮询（Round-Robin）算法公平调度

## 编译与运行

### 编译要求
- C++17 或更高版本
- 支持 pthread 的操作系统

```bash
# 编译主程序
g++ -std=c++17 -Wall -Wextra -O2 -pthread TaskScheduler.cpp -o TaskScheduler
g++ -std=c++17 -fsanitize=thread -fno-omit-frame-pointer -g -pthread TaskScheduler.cpp -o scheduler_tsan -Wno-unused-parameter

# 运行示例
./TaskScheduler
```

## 快速开始

### 基本使用

```cpp
// 创建配置
XXXTaskScheduler::Config config;
config.worker_count = 4;
config.enable_stats_logging = true;
config.stats_interval = std::chrono::seconds{5};

// 创建调度器
XXXTaskScheduler scheduler(config);

// 提交不同类型的任务
scheduler.submit_task_a(42, "Hello",
    [](int v, const string& s) {
        // TaskA 处理逻辑
    }, "MyTaskA", 0);

scheduler.submit_task_b(3.14, {1, 2, 3},
    [](double d, const vector<int>& v) {
        // TaskB 处理逻辑
    }, "MyTaskB", 1);

scheduler.submit_task_c("Data", true, 100,
    [](const string& s, bool b, int i) {
        // TaskC 处理逻辑
    }, "MyTaskC", 2);

// 等待所有任务完成
scheduler.wait_for_completion();

// 获取统计信息
uint64_t submitted, completed, failed;
array<uint64_t, 3> type_completed, type_failed;
scheduler.get_statistics(submitted, completed, failed,
                        type_completed, type_failed);
```

## 生产级特性

### 线程安全
- 所有操作都是线程安全的
- 原子操作和锁机制保证数据一致性

### 异常处理
- 完善的异常处理和错误日志
- 任务异常不影响调度器运行

### 性能监控
- 详细的统计信息和性能指标
- 实时队列状态监控

### 资源管理
- 正确的生命周期管理和资源清理
- RAII 设计模式

### 配置灵活
- 可配置的参数和选项
- 运行时动态调整

### 日志系统
- 分级日志记录（DEBUG/INFO/WARN/ERROR）
- 时间戳自动添加

### 优雅关闭
- 支持超时的优雅关闭机制
- 处理剩余任务后再退出

## 测试程序

项目包含多个测试程序：

```bash
# 编译并运行所有测试
g++ -std=c++17 -Wall -Wextra -O2 -pthread TaskScheduler.cpp -o TaskScheduler
g++ -std=c++17 -fsanitize=thread -fno-omit-frame-pointer -g -pthread TaskScheduler.cpp -o scheduler_tsan -Wno-unused-parameter

# 运行测试

```

## 文档结构

- `TaskScheduler_Documentation.md` - 详细设计和实现文档
- `Memory_Order_Summary.md` - 内存序分析和优化报告
- `Condition_Variable_Best_Practices.md` - 条件变量最佳实践
- `ThreadSafeQueue_Improvement_Report.md` - 队列改进和性能优化报告

## 性能指标

- **队列大小查询**：平均 0.02μs（原子操作，无需加锁）
- **任务吞吐量**：支持高并发任务提交和处理
- **内存使用**：优化的内存布局，避免伪共享

## 示例输出

```
=== XXXTaskScheduler Thread-Safe Test ===
[2024-01-03 12:00:54] [INFO]  XXXTaskScheduler started with 4 workers
[2024-01-03 12:00:54] [INFO]  Executing TaskA: TestTaskA_0 (ID: 1)
[2024-01-03 12:00:54] [INFO]  Completed TaskA: TestTaskA_0 (ID: 1)
...
[2024-01-03 12:00:54] [INFO]  Performance - Success Rate: 100.00%, Avg Execution Time: 218ms
[2024-01-03 12:00:54] [INFO]  XXXTaskScheduler shutdown complete

=== Final Task Scheduler Statistics ===
Queue Status - TypeA: 0, TypeB: 0, TypeC: 0
Total - Submitted: 37, Completed: 37, Failed: 0
Performance - Success Rate: 100.00%, Avg Execution Time: 218.24ms
```

## 注意事项

1. **内存序**：正确使用了 acquire/release 和 relaxed 内存序优化
2. **虚假唤醒**：条件变量使用谓词避免虚假唤醒问题
3. **资源清理**：程序退出前会处理所有剩余任务
4. **编译警告**：使用 -Wall -Wextra 确保代码质量

## 许可证

MIT License

## 贡献

欢迎提交 Issue 和 Pull Request！
