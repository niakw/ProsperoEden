    // Native GPU producer waits for a real free slot rather than sleeping
    // 100 us and repeatedly probing the full queue. The stop token wakes
    // the condition variable even if the GPU consumer never pops again.
    // The caller (GPU ThreadManager) already serializes all producers.
    template <typename... Args>
    bool EmplaceWaitWithStopToken(std::stop_token stop_token, Args&&... args) noexcept {
        if (stop_token.stop_requested()) return false;
        const std::size_t write_index = producer.index.load(std::memory_order_relaxed);
        {
            std::unique_lock lock{producer.cv_mutex};
            if (!producer.cv.wait(lock, stop_token, [this, write_index] {
                return (write_index - consumer.index.load(std::memory_order_acquire)) < Capacity;
            }) || stop_token.stop_requested()) return false;
        }
        const std::size_t pos = write_index % Capacity;
        std::construct_at(std::addressof(m_data[pos]), std::forward<Args>(args)...);
        ++producer.index;
        std::scoped_lock lock{consumer.cv_mutex};
        consumer.cv.notify_one();
        return true;
    }
