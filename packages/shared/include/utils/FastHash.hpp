#ifndef FIRMIUS_SHARED_UTILS_FAST_HASH_HPP
#define FIRMIUS_SHARED_UTILS_FAST_HASH_HPP

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <functional>

namespace firmius::shared::utils {

/**
 * @brief A high-performance, generic hash map using Robin Hood hashing.
 * 1. Flat memory layout (vector of entries) for cache locality.
 * 2. Robin Hood hashing (swap-on-distance) to minimize probe variance.
 */
template <typename K, typename V>
class FastHash {
public:
    struct Entry {
        K first;  // key compatibility with std::pair
        V second; // value compatibility with std::pair
        size_t probe_distance = 0;
        bool occupied = false;

        Entry() = default;
        Entry(K k, V v, size_t dist, bool occ)
            : first(std::move(k)), second(std::move(v)), probe_distance(dist), occupied(occ) {}
    };

    explicit FastHash(size_t initial_capacity = 16) {
        entries_.assign(initial_capacity, Entry());
    }

    void set(const K& key, V value) {
        if (load_factor() > 0.7) {
            resize(entries_.size() * 2);
        }

        size_t h = hash(key);
        size_t idx = h % entries_.size();
        
        Entry new_entry(key, std::move(value), 0, true);

        while (true) {
            if (!entries_[idx].occupied) {
                entries_[idx] = std::move(new_entry);
                size_++;
                return;
            }

            if (entries_[idx].first == key) {
                entries_[idx].second = std::move(new_entry.second);
                return;
            }

            if (new_entry.probe_distance > entries_[idx].probe_distance) {
                std::swap(new_entry, entries_[idx]);
            }

            idx = (idx + 1) % entries_.size();
            new_entry.probe_distance++;
        }
    }

    V* get(const K& key) {
        if (entries_.empty()) return nullptr;
        size_t h = hash(key);
        size_t idx = h % entries_.size();
        size_t dist = 0;

        while (entries_[idx].occupied) {
            if (dist > entries_[idx].probe_distance) return nullptr;
            if (entries_[idx].first == key) return &entries_[idx].second;
            idx = (idx + 1) % entries_.size();
            dist++;
        }
        return nullptr;
    }
    const V* get(const K& key) const {
        if (entries_.empty()) return nullptr;
        size_t h = hash(key);
        size_t idx = h % entries_.size();
        size_t dist = 0;

        while (entries_[idx].occupied) {
            if (dist > entries_[idx].probe_distance) return nullptr;
            if (entries_[idx].first == key) return &entries_[idx].second;
            idx = (idx + 1) % entries_.size();
            dist++;
        }
        return nullptr;
    }

    bool contains(const K& key) const {
        return const_cast<FastHash*>(this)->get(key) != nullptr;
    }

    void clear() {
        for (auto& e : entries_) e.occupied = false;
        size_ = 0;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    V& operator[](const K& key) {
        V* existing = get(key);
        if (existing) return *existing;
        set(key, V());
        return *get(key);
    }

    void erase(const K& key) {
        size_t h = hash(key);
        size_t idx = h % entries_.size();
        size_t dist = 0;

        while (entries_[idx].occupied) {
            if (dist > entries_[idx].probe_distance) return;
            if (entries_[idx].first == key) {
                entries_[idx].occupied = false;
                size_--;
                return;
            }
            idx = (idx + 1) % entries_.size();
            dist++;
        }
    }

    struct Iterator {
        typename std::vector<Entry>::iterator current;
        typename std::vector<Entry>::iterator end;

        Iterator& operator++() {
            do { ++current; } while (current != end && !current->occupied);
            return *this;
        }
        bool operator==(const Iterator& other) const { return current == other.current; }
        bool operator!=(const Iterator& other) const { return current != other.current; }
        Entry& operator*() { return *current; }
        Entry* operator->() { return &(*current); }
    };

    Iterator begin() {
        auto it = entries_.begin();
        while (it != entries_.end() && !it->occupied) ++it;
        return {it, entries_.end()};
    }

    Iterator erase(Iterator it) {
        if (it == end()) return end();
        it.current->occupied = false;
        size_--;
        auto next = it.current;
        while (next != entries_.end() && !next->occupied) ++next;
        return {next, entries_.end()};
    }
    Iterator end() { return {entries_.end(), entries_.end()}; }

    Iterator find(const K& key) {
        if (entries_.empty()) return end();
        size_t h = hash(key);
        size_t idx = h % entries_.size();
        size_t dist = 0;
        while (entries_[idx].occupied) {
            if (dist > entries_[idx].probe_distance) return end();
            if (entries_[idx].first == key) return {entries_.begin() + idx, entries_.end()};
            idx = (idx + 1) % entries_.size();
            dist++;
        }
        return end();
    }

private:
    std::vector<Entry> entries_;
    size_t size_ = 0;
    float load_factor() const { return entries_.empty() ? 1.0f : (float)size_ / entries_.size(); }
    static size_t hash(const K& key) { return std::hash<K>{}(key); }
    void resize(size_t new_capacity) {
        std::vector<Entry> old = std::move(entries_);
        entries_.assign(new_capacity, Entry());
        size_ = 0;
        for (auto& e : old) if (e.occupied) set(std::move(e.first), std::move(e.second));
    }
};

} // namespace firmius::shared::utils
#endif
