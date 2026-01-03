#include <iostream>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <chrono>
#include <random>
#include <memory>
#include <string>
#include <exception>
#include <future>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <cstdarg>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include "Logger.h"

// 定义获取线程ID的宏
#define GETTID() syscall(SYS_gettid)

// 获取当前线程运行的CPU核心
inline int getCurrentCPU() {
    return sched_getcpu();
}

// 设置线程亲和性的辅助函数
inline void setThreadAffinity(int cpu_id, pthread_t thread) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        Logger::log(LogLevel::WARN, "Failed to set thread affinity to CPU %d", cpu_id);
    }
}

// 前向声明
class XXXTaskScheduler;

// 线程安全的任务队列类
template<typename T>
class ThreadSafeQueue {
private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::condition_variable condition_;
    std::atomic<size_t> size_{0};

public:
    ThreadSafeQueue() = default;

    // 禁用拷贝构造和赋值
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // 向队列添加元素
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
        size_.fetch_add(1, std::memory_order_relaxed);  // relaxed 是合适的：size_ 仅用于统计
        condition_.notify_one();
    }

    // 从队列取出元素（阻塞）
    bool wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return !queue_.empty(); });
        value = std::move(queue_.front());
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_relaxed);  // relaxed：同步由 mutex 保证
        return true;
    }

    // 从队列取出元素（超时版本）
    bool wait_and_pop_for(T& value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return false; // 超时
        }
        value = std::move(queue_.front());
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_relaxed);  // relaxed
        return true;
    }

    // 尝试从队列取出元素（非阻塞）
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_relaxed);  // relaxed
        return true;
    }

    // 非阻塞检查是否为空
    // 注意：这个方法可能返回过时的结果，但能保证：
    // - 如果返回 true，队列确实为空或即将变为空
    // - 如果返回 false，队列一定不为空
    bool empty() const {
        return size_.load(std::memory_order_relaxed) == 0;
    }

    // 获取队列大小（原子操作）
    // 返回一个近似值，可能稍有延迟，但对于统计目的足够
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    // 清空队列
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        size_.store(0, std::memory_order_relaxed);
        condition_.notify_all();
    }

    // 通知所有等待的线程
    void notify_all() {
        condition_.notify_all();
    }
};


// 任务类型枚举
enum class TaskType {
    TYPE_A = 0,
    TYPE_B = 1,
    TYPE_C = 2
};

// 任务状态枚举
enum class TaskStatus {
    PENDING,
    RUNNING,
    COMPLETED,
    FAILED
};

// XXXTask基类
class XXXTask {
protected:
    static std::atomic<uint64_t> next_task_id_;
    uint64_t task_id_;
    TaskType type_;
    std::string task_name_;
    std::atomic<TaskStatus> status_;
    std::chrono::steady_clock::time_point create_time_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point end_time_;
    std::string error_message_;
    mutable std::mutex task_mutex_;

public:
    XXXTask(TaskType type, const std::string& name = "")
        : task_id_(next_task_id_++), type_(type),
          task_name_(name.empty() ? "Task_" + std::to_string(task_id_) : name),
          status_(TaskStatus::PENDING), create_time_(std::chrono::steady_clock::now()) {
        Logger::log(LogLevel::DEBUG, "Created task: %s (ID: %llu, Type: %d)",
                   task_name_.c_str(), task_id_, static_cast<int>(type_));
    }

    virtual ~XXXTask() = default;

    virtual void execute() = 0;
    virtual std::string get_description() const = 0;

    // Getter方法
    uint64_t get_id() const { return task_id_; }
    TaskType get_type() const { return type_; }
    const std::string& get_name() const { return task_name_; }
    TaskStatus get_status() const { return status_.load(); }

    std::chrono::milliseconds get_execution_time() const {
        std::lock_guard<std::mutex> lock(task_mutex_);
        if (status_ == TaskStatus::COMPLETED || status_ == TaskStatus::FAILED) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end_time_ - start_time_);
        }
        return std::chrono::milliseconds::zero();
    }

    std::chrono::milliseconds get_wait_time() const {
        std::lock_guard<std::mutex> lock(task_mutex_);
        if (status_ != TaskStatus::PENDING) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(start_time_ - create_time_);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - create_time_);
    }

    const std::string& get_error_message() const {
        std::lock_guard<std::mutex> lock(task_mutex_);
        return error_message_;
    }

protected:
    void set_status(TaskStatus status) {
        std::lock_guard<std::mutex> lock(task_mutex_);
        status_ = status;
        auto now = std::chrono::steady_clock::now();

        if (status == TaskStatus::RUNNING) {
            start_time_ = now;
        } else if (status == TaskStatus::COMPLETED || status == TaskStatus::FAILED) {
            end_time_ = now;
        }
    }

    void set_error(const std::string& error) {
        std::lock_guard<std::mutex> lock(task_mutex_);
        error_message_ = error;
        status_ = TaskStatus::FAILED;
        end_time_ = std::chrono::steady_clock::now();
    }

    friend class XXXWorker;
    friend class XXXTaskScheduler;
};

std::atomic<uint64_t> XXXTask::next_task_id_{1};

// 任务类型A：处理整数和字符串
class XXXTaskTypeA : public XXXTask {
private:
    int param1_;
    std::string param2_;
    std::function<void(int, const std::string&)> handler_;

public:
    XXXTaskTypeA(int param1, const std::string& param2,
                 std::function<void(int, const std::string&)> handler,
                 const std::string& name = "")
        : XXXTask(TaskType::TYPE_A, name), param1_(param1), param2_(param2), handler_(handler) {}

    void execute() override {
        try {
            set_status(TaskStatus::RUNNING);
            Logger::log(LogLevel::INFO, "Executing TaskA: %s (ID: %llu) - params: %d, '%s'",
                       task_name_.c_str(), task_id_, param1_, param2_.c_str());

            if (handler_) {
                handler_(param1_, param2_);
            }

            set_status(TaskStatus::COMPLETED);
            Logger::log(LogLevel::INFO, "Completed TaskA: %s (ID: %llu)", task_name_.c_str(), task_id_);
        } catch (const std::exception& e) {
            set_error(std::string("TaskA execution failed: ") + e.what());
            Logger::log(LogLevel::ERROR, "TaskA failed: %s (ID: %llu) - %s",
                       task_name_.c_str(), task_id_, e.what());
        }
    }

    std::string get_description() const override {
        return "TaskA(int=" + std::to_string(param1_) + ", string='" + param2_ + "')";
    }
};

// 任务类型B：处理浮点数和整数向量
class XXXTaskTypeB : public XXXTask {
private:
    double param1_;
    std::vector<int> param2_;
    std::function<void(double, const std::vector<int>&)> handler_;

public:
    XXXTaskTypeB(double param1, const std::vector<int>& param2,
                 std::function<void(double, const std::vector<int>&)> handler,
                 const std::string& name = "")
        : XXXTask(TaskType::TYPE_B, name), param1_(param1), param2_(param2), handler_(handler) {}

    void execute() override {
        try {
            set_status(TaskStatus::RUNNING);
            Logger::log(LogLevel::INFO, "Executing TaskB: %s (ID: %llu) - params: %.2f, vector[%zu]",
                       task_name_.c_str(), task_id_, param1_, param2_.size());

            if (handler_) {
                handler_(param1_, param2_);
            }

            set_status(TaskStatus::COMPLETED);
            Logger::log(LogLevel::INFO, "Completed TaskB: %s (ID: %llu)", task_name_.c_str(), task_id_);
        } catch (const std::exception& e) {
            set_error(std::string("TaskB execution failed: ") + e.what());
            Logger::log(LogLevel::ERROR, "TaskB failed: %s (ID: %llu) - %s",
                       task_name_.c_str(), task_id_, e.what());
        }
    }

    std::string get_description() const override {
        return "TaskB(double=" + std::to_string(param1_) + ", vector_size=" + std::to_string(param2_.size()) + ")";
    }
};

// 任务类型C：处理字符串、布尔值和整数
class XXXTaskTypeC : public XXXTask {
private:
    std::string param1_;
    bool param2_;
    int param3_;
    std::function<void(const std::string&, bool, int)> handler_;

public:
    XXXTaskTypeC(const std::string& param1, bool param2, int param3,
                 std::function<void(const std::string&, bool, int)> handler,
                 const std::string& name = "")
        : XXXTask(TaskType::TYPE_C, name), param1_(param1), param2_(param2), param3_(param3), handler_(handler) {}

    void execute() override {
        try {
            set_status(TaskStatus::RUNNING);
            Logger::log(LogLevel::INFO, "Executing TaskC: %s (ID: %llu) - params: '%s', %s, %d",
                       task_name_.c_str(), task_id_, param1_.c_str(), param2_ ? "true" : "false", param3_);

            if (handler_) {
                handler_(param1_, param2_, param3_);
            }

            set_status(TaskStatus::COMPLETED);
            Logger::log(LogLevel::INFO, "Completed TaskC: %s (ID: %llu)", task_name_.c_str(), task_id_);
        } catch (const std::exception& e) {
            set_error(std::string("TaskC execution failed: ") + e.what());
            Logger::log(LogLevel::ERROR, "TaskC failed: %s (ID: %llu) - %s",
                       task_name_.c_str(), task_id_, e.what());
        }
    }

    std::string get_description() const override {
        return "TaskC(string='" + param1_ + "', bool=" + (param2_ ? "true" : "false") +
               ", int=" + std::to_string(param3_) + ")";
    }
};

// Worker类 - 独立的任务处理器
class XXXWorker {
private:
    size_t worker_id_;
    std::atomic<pid_t> thread_id_;  // 线程的TID
    std::array<ThreadSafeQueue<std::shared_ptr<XXXTask>>, 3> task_queues_;
    std::thread worker_thread_;
    std::atomic<bool> is_running_;
    std::atomic<bool> is_shutdown_;
    std::chrono::milliseconds poll_interval_;

    // 每个worker自己的轮询索引
    std::atomic<size_t> round_robin_index_{0};

    // 统计回调函数
    std::function<void(std::shared_ptr<XXXTask>, TaskType, std::chrono::milliseconds, bool)> stats_callback_;

public:
    XXXWorker(size_t worker_id, std::chrono::milliseconds poll_interval = std::chrono::milliseconds{100})
        : worker_id_(worker_id), thread_id_(0), is_running_(false), is_shutdown_(false), poll_interval_(poll_interval) {}

    ~XXXWorker() {
        shutdown();
    }

    // 禁用拷贝构造和赋值
    XXXWorker(const XXXWorker&) = delete;
    XXXWorker& operator=(const XXXWorker&) = delete;

    // 设置统计回调函数
    void set_stats_callback(std::function<void(std::shared_ptr<XXXTask>, TaskType, std::chrono::milliseconds, bool)> callback) {
        std::lock_guard<std::mutex> lock(get_stats_mutex());
        stats_callback_ = callback;
    }

    // 启动worker
    bool start() {
        if (is_running_.load()) {
            Logger::log(LogLevel::WARN, "Worker %zu already running", worker_id_);
            return false;
        }

        is_running_ = true;
        is_shutdown_ = false;

        worker_thread_ = std::thread(&XXXWorker::worker_thread_function, this);

        Logger::log(LogLevel::INFO, "Worker %zu started", worker_id_);
        return true;
    }

    // 关闭worker
    void shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        if (is_shutdown_.load()) {
            return;
        }

        Logger::log(LogLevel::INFO, "Shutting down worker %zu...", worker_id_);
        is_running_ = false;

        // 通知所有队列
        for (auto& queue : task_queues_) {
            queue.notify_all();
        }

        // 等待线程结束
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        is_shutdown_ = true;
        Logger::log(LogLevel::INFO, "Worker %zu shutdown complete", worker_id_);
    }

    // 提交任务到指定类型的队列
    bool submit_task(std::shared_ptr<XXXTask> task) {
        if (!is_running_.load()) {
            Logger::log(LogLevel::WARN, "Cannot submit task: worker %zu not running", worker_id_);
            return false;
        }

        int type_index = static_cast<int>(task->get_type());
        // 边界检查
        type_index = std::max(0, std::min(type_index, 2));

        task_queues_[type_index].push(task);

        Logger::log(LogLevel::DEBUG, "Task submitted: %s (%s) to worker %zu",
                   task->get_name().c_str(), task->get_description().c_str(), worker_id_);
        return true;
    }

    // 获取各队列大小 - 现在使用原子操作，性能更好
    std::array<size_t, 3> get_queue_sizes() {
        std::array<size_t, 3> sizes{};

        for (int i = 0; i < 3; ++i) {
            sizes[i] = task_queues_[i].size();
        }
        return sizes;
    }

    // 获取总待处理任务数
    size_t get_pending_tasks_count() {
        auto sizes = get_queue_sizes();
        return sizes[0] + sizes[1] + sizes[2];
    }

    // 清空所有队列
    void clear_all_queues() {
        for (int i = 0; i < 3; ++i) {
            task_queues_[i].clear();
        }
        Logger::log(LogLevel::INFO, "Worker %zu: All task queues cleared", worker_id_);
    }

    // 获取worker ID
    size_t get_worker_id() const {
        return worker_id_;
    }

    // 获取线程TID
    pid_t get_thread_id() const {
        return thread_id_.load();
    }

    // 检查是否正在运行
    bool is_running() const {
        return is_running_.load();
    }

private:
    static std::mutex& get_stats_mutex() {
        static std::mutex stats_mutex;
        return stats_mutex;
    }

    // 工作线程函数
    void worker_thread_function() {
        // 设置线程亲和性
        int cpu_count = std::thread::hardware_concurrency();
        int target_cpu = -1;

        if (cpu_count > 0) {
            target_cpu = worker_id_ % cpu_count;
            setThreadAffinity(target_cpu, worker_thread_.native_handle());
            // 让调度器重新调度，以使本线程绑定到设置的CPU core
            std::this_thread::yield();
        }

        // 在工作线程内部获取真正的TID
        thread_id_.store(GETTID());
        int current_cpu = getCurrentCPU();

        Logger::log(LogLevel::INFO, "Worker %zu (ID:%d) started on CPU %d",
                   worker_id_, thread_id_.load(), current_cpu);

        while (is_running_.load()) {
            std::shared_ptr<XXXTask> task = nullptr;
            TaskType found_type = TaskType::TYPE_A;
            bool task_found = false;

            // 轮询检查自己的三个队列
            for (int attempt = 0; attempt < 3 && !task_found; ++attempt) {
                // 安全地获取轮询索引
                size_t queue_index = round_robin_index_.fetch_add(1) % 3;

                if (task_queues_[queue_index].try_pop(task)) {
                    found_type = static_cast<TaskType>(queue_index);
                    task_found = true;
                }

                if (task_found) break;
            }

            if (task_found && task) {
                // 执行任务
                execute_task(task, found_type);
            } else {
                // 没有任务时，检查是否应该继续运行
                if (!is_running_.load()) {
                    // shutdown 期间，可能需要处理剩余任务
                    // 再次快速检查所有队列，尝试处理剩余任务
                    for (int i = 0; i < 3; ++i) {
                        if (task_queues_[i].try_pop(task)) {
                            execute_task(task, static_cast<TaskType>(i));
                            task = nullptr; // 处理完后重置
                        }
                    }
                    break; // 退出循环
                }

                // 没有任务且仍在运行，等待一段时间
                size_t wait_queue = round_robin_index_.fetch_add(1) % 3;

                if (!task_queues_[wait_queue].wait_and_pop_for(task, poll_interval_)) {
                    // 超时，继续下一轮循环
                    continue;
                }

                // 成功获取到任务，执行它
                if (task) {
                    execute_task(task, static_cast<TaskType>(wait_queue));
                }
            }
        }

        // shutdown 前最后尝试处理剩余任务
        bool has_task = true;
        while (has_task) {
            has_task = false;
            for (int i = 0; i < 3; ++i) {
                std::shared_ptr<XXXTask> remaining_task;
                if (task_queues_[i].try_pop(remaining_task)) {
                    execute_task(remaining_task, static_cast<TaskType>(i));
                    has_task = true;
                }
            }
        }

        Logger::log(LogLevel::INFO, "Worker %zu thread stopped", worker_id_);
    }

    // 执行单个任务
    void execute_task(std::shared_ptr<XXXTask> task, TaskType type) {
        auto start_time = std::chrono::steady_clock::now();
        bool success = true;

        try {
            Logger::log(LogLevel::DEBUG, "Worker %zu (ID:%d) executing task: %s (ID: %llu)",
                       worker_id_, thread_id_.load(), task->get_name().c_str(), task->get_id());

            task->execute();

        } catch (const std::exception& e) {
            Logger::log(LogLevel::ERROR, "Task execution failed: %s (ID: %llu) - %s",
                       task->get_name().c_str(), task->get_id(), e.what());

            task->set_error(e.what());
            success = false;
        } catch (...) {
            Logger::log(LogLevel::ERROR, "Task execution failed with unknown exception: %s (ID: %llu)",
                       task->get_name().c_str(), task->get_id());

            task->set_error("Unknown exception");
            success = false;
        }

        // 更新统计信息 - 在锁外调用以避免死锁
        auto execution_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        // 复制回调函数到局部变量
        std::function<void(std::shared_ptr<XXXTask>, TaskType, std::chrono::milliseconds, bool)> callback;
        {
            std::lock_guard<std::mutex> lock(get_stats_mutex());
            callback = stats_callback_;
        }

        // 在锁外调用回调
        if (callback) {
            try {
                callback(task, type, execution_time, success);
            } catch (const std::exception& e) {
                Logger::log(LogLevel::ERROR, "Stats callback failed: %s", e.what());
            }
        }
    }
};

// 任务调度器类
class XXXTaskScheduler {
private:
    std::vector<std::unique_ptr<XXXWorker>> workers_;
    std::atomic<bool> is_running_;
    std::atomic<bool> is_shutdown_;

    // 统计信息 - 所有原子操作
    std::atomic<uint64_t> total_submitted_{0};
    std::atomic<uint64_t> total_completed_{0};
    std::atomic<uint64_t> total_failed_{0};
    std::atomic<uint64_t> total_execution_time_ms_{0};
    std::array<std::atomic<uint64_t>, 3> type_counts_{};
    std::array<std::atomic<uint64_t>, 3> type_completed_{};
    std::array<std::atomic<uint64_t>, 3> type_failed_{};

    // 性能监控
    std::thread stats_thread_;
    std::atomic<bool> enable_stats_logging_;

    mutable std::mutex stats_mutex_;
    std::atomic<uint64_t> active_workers_{0};

public:
    struct Config {
        size_t worker_count = std::thread::hardware_concurrency();
        std::chrono::milliseconds worker_poll_interval{100};
        bool enable_stats_logging = false;
        std::chrono::seconds stats_interval{5};

        Config() {
            if (worker_count == 0) {
                worker_count = 4;
            }
        }
    };

    explicit XXXTaskScheduler(const Config& config = Config{})
        : is_running_(false), is_shutdown_(false),
          enable_stats_logging_(config.enable_stats_logging) {

        // 创建workers
        for (size_t i = 0; i < config.worker_count; ++i) {
            workers_.emplace_back(
                std::make_unique<XXXWorker>(i, config.worker_poll_interval)
            );

            // 设置统计回调函数 - 使用lambda捕获this，保持线程安全
            workers_.back()->set_stats_callback(
                [this](std::shared_ptr<XXXTask> task, TaskType type,
                       std::chrono::milliseconds execution_time, bool success) {
                    this->update_statistics(task, type, execution_time, success);
                }
            );
        }

        start(config.stats_interval);
    }

    ~XXXTaskScheduler() {
        shutdown();
    }

    // 禁用拷贝构造和赋值
    XXXTaskScheduler(const XXXTaskScheduler&) = delete;
    XXXTaskScheduler& operator=(const XXXTaskScheduler&) = delete;

    // 启动调度器
    bool start(std::chrono::seconds stats_interval = std::chrono::seconds{5}) {
        if (is_running_.load(std::memory_order_acquire)) {
            Logger::log(LogLevel::WARN, "XXXTaskScheduler already running");
            return false;
        }

        is_running_.store(true, std::memory_order_release);
        is_shutdown_ = false;

        // 启动所有workers
        for (auto& worker : workers_) {
            if (!worker->start()) {
                Logger::log(LogLevel::ERROR, "Failed to start worker");
                return false;
            }
        }

        // 启动统计线程
        if (enable_stats_logging_.load(std::memory_order_acquire)) {
            stats_thread_ = std::thread([this, stats_interval]() {
                statistics_thread_function(stats_interval);
            });
        }

        Logger::log(LogLevel::INFO, "XXXTaskScheduler started with %zu workers", workers_.size());
        return true;
    }

    // 关闭调度器
    void shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        if (is_shutdown_.load()) {
            return;
        }

        Logger::log(LogLevel::INFO, "Shutting down XXXTaskScheduler...");
        is_running_.store(false, std::memory_order_release);

        // 关闭所有workers
        for (auto& worker : workers_) {
            worker->shutdown(timeout);
        }

        // 停止统计线程
        if (stats_thread_.joinable()) {
            stats_thread_.join();
        }

        is_shutdown_ = true;
        print_final_statistics();
        Logger::log(LogLevel::INFO, "XXXTaskScheduler shutdown complete");
    }

    // 提交任务A
    bool submit_task_a(int param1, const std::string& param2,
                      std::function<void(int, const std::string&)> handler,
                      const std::string& name = "",
                      size_t target_worker_id = 0) {
        if (!is_running_.load(std::memory_order_acquire)) {
            Logger::log(LogLevel::WARN, "Cannot submit task: scheduler not running");
            return false;
        }

        if (target_worker_id >= workers_.size()) {
            Logger::log(LogLevel::ERROR, "Invalid worker_id %zu, max is %zu",
                       target_worker_id, workers_.size());
            return false;
        }

        auto task = std::make_shared<XXXTaskTypeA>(param1, param2, handler, name);
        total_submitted_.fetch_add(1, std::memory_order_relaxed);
        type_counts_[0].fetch_add(1, std::memory_order_relaxed);

        return workers_[target_worker_id]->submit_task(task);
    }

    // 提交任务B
    bool submit_task_b(double param1, const std::vector<int>& param2,
                      std::function<void(double, const std::vector<int>&)> handler,
                      const std::string& name = "",
                      size_t target_worker_id = 0) {
        if (!is_running_.load(std::memory_order_acquire)) {
            Logger::log(LogLevel::WARN, "Cannot submit task: scheduler not running");
            return false;
        }

        if (target_worker_id >= workers_.size()) {
            Logger::log(LogLevel::ERROR, "Invalid worker_id %zu, max is %zu",
                       target_worker_id, workers_.size());
            return false;
        }

        auto task = std::make_shared<XXXTaskTypeB>(param1, param2, handler, name);
        total_submitted_.fetch_add(1, std::memory_order_relaxed);
        type_counts_[1].fetch_add(1, std::memory_order_relaxed);

        return workers_[target_worker_id]->submit_task(task);
    }

    // 提交任务C
    bool submit_task_c(const std::string& param1, bool param2, int param3,
                      std::function<void(const std::string&, bool, int)> handler,
                      const std::string& name = "",
                      size_t target_worker_id = 0) {
        if (!is_running_.load(std::memory_order_acquire)) {
            Logger::log(LogLevel::WARN, "Cannot submit task: scheduler not running");
            return false;
        }

        if (target_worker_id >= workers_.size()) {
            Logger::log(LogLevel::ERROR, "Invalid worker_id %zu, max is %zu",
                       target_worker_id, workers_.size());
            return false;
        }

        auto task = std::make_shared<XXXTaskTypeC>(param1, param2, param3, handler, name);
        total_submitted_.fetch_add(1, std::memory_order_relaxed);
        type_counts_[2].fetch_add(1, std::memory_order_relaxed);

        return workers_[target_worker_id]->submit_task(task);
    }

    // 等待所有任务完成
    bool wait_for_completion(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        auto start_time = std::chrono::steady_clock::now();

        while (get_pending_tasks_count() > 0) {
            if (timeout != std::chrono::milliseconds::max()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);
                if (elapsed >= timeout) {
                    Logger::log(LogLevel::WARN, "Wait for completion timed out");
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }

    // 获取统计信息
    void get_statistics(uint64_t& total_submitted, uint64_t& total_completed, uint64_t& total_failed,
                       std::array<uint64_t, 3>& type_completed, std::array<uint64_t, 3>& type_failed) const {
        total_submitted = total_submitted_.load(std::memory_order_acquire);
        total_completed = total_completed_.load(std::memory_order_acquire);
        total_failed = total_failed_.load(std::memory_order_acquire);
        for (int i = 0; i < 3; ++i) {
            type_completed[i] = type_completed_[i].load(std::memory_order_acquire);
            type_failed[i] = type_failed_[i].load(std::memory_order_acquire);
        }
    }

    // 获取平均执行时间
    double get_average_execution_time() const {
        uint64_t completed = total_completed_.load(std::memory_order_acquire);
        return completed > 0 ?
            static_cast<double>(total_execution_time_ms_.load(std::memory_order_acquire)) / completed : 0.0;
    }

    // 获取成功率
    double get_success_rate() const {
        uint64_t total = total_submitted_.load(std::memory_order_acquire);
        return total > 0 ?
            static_cast<double>(total_completed_.load(std::memory_order_acquire)) / total * 100.0 : 0.0;
    }

    // 获取各队列的待处理任务数
    std::array<size_t, 3> get_queue_sizes() {
        std::array<size_t, 3> total_sizes{};

        std::lock_guard<std::mutex> lock(stats_mutex_);
        for (const auto& worker : workers_) {
            if (worker) {
                auto worker_sizes = worker->get_queue_sizes();
                for (int i = 0; i < 3; ++i) {
                    total_sizes[i] += worker_sizes[i];
                }
            }
        }
        return total_sizes;
    }

    // 获取总的待处理任务数
    size_t get_pending_tasks_count() {
        auto sizes = get_queue_sizes();
        return sizes[0] + sizes[1] + sizes[2];
    }

    // 获取活跃工作线程数
    int get_active_workers() const {
        return active_workers_.load(std::memory_order_acquire);
    }

    // 打印当前状态
    void print_status() {
        auto queue_sizes = get_queue_sizes();
        uint64_t total_submitted, total_completed, total_failed;
        std::array<uint64_t, 3> type_completed, type_failed;
        get_statistics(total_submitted, total_completed, total_failed, type_completed, type_failed);

        Logger::log(LogLevel::INFO,
                   "Status - Queues[A:%zu, B:%zu, C:%zu], Active:%d, "
                   "Submitted:%llu, Completed:%llu, Failed:%llu, SuccessRate:%.1f%%",
                   queue_sizes[0], queue_sizes[1], queue_sizes[2],
                   get_active_workers(),
                   total_submitted, total_completed,
                   total_failed, get_success_rate());
    }

    // 清空所有队列
    void clear_all_queues() {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        for (auto& worker : workers_) {
            if (worker) {
                worker->clear_all_queues();
            }
        }
        Logger::log(LogLevel::INFO, "All task queues cleared");
    }

    // 设置统计日志开关
    void set_stats_logging(bool enable) {
        enable_stats_logging_.store(enable, std::memory_order_release);
    }

    // 更新统计信息（由worker回调）
    void update_statistics(std::shared_ptr<XXXTask> task, TaskType type,
                          std::chrono::milliseconds execution_time, bool success) {
        // 使用原子操作，无需额外锁
        if (success) {
            total_completed_.fetch_add(1, std::memory_order_relaxed);
            type_completed_[static_cast<int>(type)].fetch_add(1, std::memory_order_relaxed);
        } else {
            total_failed_.fetch_add(1, std::memory_order_relaxed);
            type_failed_[static_cast<int>(type)].fetch_add(1, std::memory_order_relaxed);
        }

        total_execution_time_ms_.fetch_add(execution_time.count(), std::memory_order_relaxed);

        Logger::log(LogLevel::DEBUG, "Task statistics updated: %s (%s) - %s, %lld ms",
                    task->get_name().c_str(), task->get_description().c_str(),
                    success ? "success" : "failed", execution_time.count());
    }

private:
    // 统计线程函数
    void statistics_thread_function(std::chrono::seconds interval) {
        Logger::log(LogLevel::INFO, "Statistics thread started");

        while (is_running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(interval);
            if (is_running_.load(std::memory_order_acquire)
                && enable_stats_logging_.load(std::memory_order_acquire)) {
                print_detailed_statistics();
            }
        }

        Logger::log(LogLevel::INFO, "Statistics thread stopped");
    }

    // 打印详细统计信息
    void print_detailed_statistics() const {
        uint64_t total_submitted, total_completed, total_failed;
        std::array<uint64_t, 3> type_completed, type_failed;
        get_statistics(total_submitted, total_completed, total_failed, type_completed, type_failed);

        auto queue_sizes = const_cast<XXXTaskScheduler*>(this)->get_queue_sizes();

        Logger::log(LogLevel::INFO, "=== Task Scheduler Statistics ===");
        Logger::log(LogLevel::INFO, "Queue Status - TypeA: %zu, TypeB: %zu, TypeC: %zu",
                   queue_sizes[0], queue_sizes[1], queue_sizes[2]);
        Logger::log(LogLevel::INFO, "Total - Submitted: %llu, Completed: %llu, Failed: %llu",
                   total_submitted, total_completed, total_failed);

        for (int i = 0; i < 3; ++i) {
            Logger::log(LogLevel::INFO, "Type%c - Submitted: %llu, Completed: %llu, Failed: %llu",
                       'A' + i, type_counts_[i].load(std::memory_order_acquire),
                       type_completed[i], type_failed[i]);
        }

        Logger::log(LogLevel::INFO, "Performance - Success Rate: %.2f%%, Avg Execution Time: %.2fms",
                   get_success_rate(), get_average_execution_time());
        Logger::log(LogLevel::INFO, "================================");
    }

    // 打印最终统计信息
    void print_final_statistics() const {
        Logger::log(LogLevel::INFO, "=== Final Task Scheduler Statistics ===");
        print_detailed_statistics();
        Logger::log(LogLevel::INFO, "=====================================");
    }
};

// 测试用的任务处理函数
namespace TaskHandlers {
    void handle_task_a_simple(int value, const std::string& text) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100 + value % 500));
        Logger::log(LogLevel::DEBUG, "TaskA processed: value=%d, text='%s'", value, text.c_str());
    }

    void handle_task_a_complex(int value, const std::string& text) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200 + value % 800));
        int result = 0;
        for (int i = 0; i < value % 1000; ++i) {
            result += i;
        }
        Logger::log(LogLevel::DEBUG, "TaskA complex processed: value=%d, text='%s', result=%d",
                   value, text.c_str(), result);
    }

    void handle_task_b_math(double multiplier, const std::vector<int>& numbers) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        double sum = 0.0;
        for (int num : numbers) {
            sum += num * multiplier;
        }
        Logger::log(LogLevel::DEBUG, "TaskB math processed: multiplier=%.2f, count=%zu, result=%.2f",
                   multiplier, numbers.size(), sum);
    }

    void handle_task_b_analysis(double threshold, const std::vector<int>& data) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        int above_threshold = 0;
        for (int value : data) {
            if (value > threshold) above_threshold++;
        }
        Logger::log(LogLevel::DEBUG, "TaskB analysis processed: threshold=%.2f, above_count=%d/%zu",
                   threshold, above_threshold, data.size());
    }

    void handle_task_c_validation(const std::string& data, bool strict_mode, int max_length) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        bool valid = data.length() <= static_cast<size_t>(max_length);
        if (strict_mode && !data.empty()) {
            valid = valid && std::isalpha(data[0]);
        }
        Logger::log(LogLevel::DEBUG, "TaskC validation processed: data='%s', strict=%s, max_len=%d, valid=%s",
                   data.c_str(), strict_mode ? "true" : "false", max_length, valid ? "true" : "false");
    }

    void handle_task_c_processing(const std::string& input, bool transform, int iterations) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::string result = input;
        if (transform) {
            for (int i = 0; i < iterations && !result.empty(); ++i) {
                std::transform(result.begin(), result.end(), result.begin(), ::toupper);
            }
        }
        Logger::log(LogLevel::DEBUG, "TaskC processing processed: input='%s', transform=%s, iterations=%d",
                   input.c_str(), transform ? "true" : "false", iterations);
    }
}

// 主函数
#ifndef UNIT_TEST
int main() {
    // 设置日志级别
    Logger::set_log_level(LogLevel::DEBUG);

    std::cout << "=== XXXTaskScheduler Thread-Safe Test ===\n";

    try {
        // 配置调度器
        XXXTaskScheduler::Config config;
        config.worker_count = 4;
        config.enable_stats_logging = true;
        config.stats_interval = std::chrono::seconds(3);
        config.worker_poll_interval = std::chrono::milliseconds(50);

        // 创建并启动调度器
        XXXTaskScheduler scheduler(config);

        std::cout << "\n=== Submitting Test Tasks ===\n";

        // 提交测试任务
        for (int i = 0; i < 20; ++i) {
            size_t worker_id = i % config.worker_count;

            scheduler.submit_task_a(i * 100, "TestA_" + std::to_string(i),
                                  TaskHandlers::handle_task_a_simple,
                                  "TestTaskA_" + std::to_string(i), worker_id);

            if (i % 2 == 0) {
                std::vector<int> data = {i * 10, i * 10 + 1, i * 10 + 2};
                scheduler.submit_task_b(i * 1.1, data,
                                      TaskHandlers::handle_task_b_math,
                                      "TestTaskB_" + std::to_string(i), worker_id);
            }

            if (i % 3 == 0) {
                scheduler.submit_task_c("TestData_" + std::to_string(i), i % 2 == 0, i * 5,
                                      TaskHandlers::handle_task_c_validation,
                                      "TestTaskC_" + std::to_string(i), worker_id);
            }
        }

        std::cout << "\n=== Waiting for Task Completion ===\n";
        scheduler.wait_for_completion(std::chrono::seconds(30));

        std::cout << "\n=== Final Statistics ===\n";
        scheduler.print_status();

        std::cout << "\n=== Test Completed Successfully ===\n";

    } catch (const std::exception& e) {
        Logger::log(LogLevel::ERROR, "Test failed with exception: %s", e.what());
        return 1;
    }

    return 0;
}
#endif
