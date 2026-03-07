#ifndef FIRMIUS_SHARED_EVENT_QUEUE_HPP
#define FIRMIUS_SHARED_EVENT_QUEUE_HPP

#include <mutex>
#include <vector>

namespace firmius::shared {

template <typename T>
class EventQueue {
public:
    void push(T event) {
        std::lock_guard lock(mtx_);
        incoming_.push_back(std::move(event));
    }

    void drainInto(std::vector<T>& out) {
        out.clear();
        std::lock_guard lock(mtx_);
        out.swap(incoming_);
    }

    [[nodiscard]] std::vector<T> drainAll() {
        std::vector<T> local;
        {
            std::lock_guard lock(mtx_);
            local.swap(incoming_);
        }
        return local;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mtx_);
        return incoming_.empty();
    }

private:
    std::vector<T> incoming_;
    mutable std::mutex mtx_;
};

} // namespace firmius::shared

#endif
