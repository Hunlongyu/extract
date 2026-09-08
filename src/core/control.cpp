#include "core/control.h"

namespace extract::control {
namespace { thread_local HANDLE cancel_event = nullptr; thread_local Observer* observer = nullptr; }
Connection::Connection(HANDLE cancel, Observer* value) noexcept
    : previous_cancel_(cancel_event), previous_observer_(observer) { cancel_event = cancel; observer = value; }
Connection::~Connection() { cancel_event = previous_cancel_; observer = previous_observer_; }
void checkpoint() {
    if (cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0)
        throw Failure(Status::cancelled, L"任务已取消。");
}
void created(const fs::path& path, HANDLE file) { if (observer) observer->created(path, file); }
void committed(const fs::path& path) { if (observer) observer->committed(path); }
}
