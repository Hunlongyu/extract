#pragma once
#include "platform/files.h"

namespace extract::control {
// 内部工作进程的协作取消和创建清单；默认进程不安装此观察器。
class Observer {
public:
    virtual ~Observer() = default;
    virtual void created(const fs::path& path, HANDLE file) = 0;
    virtual void committed(const fs::path& path) = 0;
};
class Connection {
public:
    Connection(HANDLE cancel, Observer* observer) noexcept;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
private:
    HANDLE previous_cancel_;
    Observer* previous_observer_;
};
void checkpoint();
void created(const fs::path& path, HANDLE file);
void committed(const fs::path& path);
}
