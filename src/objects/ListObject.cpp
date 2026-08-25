#include "mini_redis/objects/ListObject.hpp"

#include <algorithm>
#include <iterator>

ListObject::ListObject()
    : RedisObject(ObjectType::LIST),
      encoding_(ListEncoding::ZIPLIST),
      storage_(std::vector<std::string>{}) {}

std::vector<std::string>& ListObject::get_ziplist() {
    return std::get<std::vector<std::string>>(storage_);
}

std::deque<std::string>& ListObject::get_linkedlist() {
    return std::get<std::deque<std::string>>(storage_);
}

const std::vector<std::string>& ListObject::get_ziplist() const {
    return std::get<std::vector<std::string>>(storage_);
}

const std::deque<std::string>& ListObject::get_linkedlist() const {
    return std::get<std::deque<std::string>>(storage_);
}

bool ListObject::should_use_ziplist() const {
    if (encoding_ != ListEncoding::ZIPLIST) {
        return false;
    }

    const auto& values = get_ziplist();
    if (values.size() > ZIPLIST_MAX_ENTRIES) {
        return false;
    }

    return std::all_of(values.begin(), values.end(), [](const std::string& value) {
        return value.size() <= ZIPLIST_MAX_ENTRY_SIZE;
    });
}

void ListObject::promote_to_linkedlist() {
    if (encoding_ != ListEncoding::ZIPLIST) {
        return;
    }

    const auto& old_values = get_ziplist();
    storage_ = std::deque<std::string>(old_values.begin(), old_values.end());
    encoding_ = ListEncoding::LINKEDLIST;
}

std::optional<size_t> ListObject::normalize_index(long long index, size_t size) {
    if (size == 0) {
        return std::nullopt;
    }

    long long normalized = index;
    if (normalized < 0) {
        normalized += static_cast<long long>(size);
    }
    if (normalized < 0 || normalized >= static_cast<long long>(size)) {
        return std::nullopt;
    }
    return static_cast<size_t>(normalized);
}

std::optional<std::pair<size_t, size_t>> ListObject::normalize_range(
    long long start, long long stop, size_t size) {
    if (size == 0) {
        return std::nullopt;
    }

    const long long last = static_cast<long long>(size) - 1;
    if (start < 0) {
        start += static_cast<long long>(size);
    }
    if (stop < 0) {
        stop += static_cast<long long>(size);
    }

    if (start < 0) {
        start = 0;
    }
    if (stop < 0 || start > last || start > stop) {
        return std::nullopt;
    }
    if (stop > last) {
        stop = last;
    }

    return std::make_pair(static_cast<size_t>(start), static_cast<size_t>(stop));
}

void ListObject::push_front(std::string value) {
    if (encoding_ == ListEncoding::ZIPLIST &&
        (get_ziplist().size() + 1 > ZIPLIST_MAX_ENTRIES ||
         value.size() > ZIPLIST_MAX_ENTRY_SIZE)) {
        promote_to_linkedlist();
    }

    if (encoding_ == ListEncoding::ZIPLIST) {
        get_ziplist().insert(get_ziplist().begin(), std::move(value));
    } else {
        get_linkedlist().push_front(std::move(value));
    }
}

void ListObject::push_back(std::string value) {
    if (encoding_ == ListEncoding::ZIPLIST &&
        (get_ziplist().size() + 1 > ZIPLIST_MAX_ENTRIES ||
         value.size() > ZIPLIST_MAX_ENTRY_SIZE)) {
        promote_to_linkedlist();
    }

    if (encoding_ == ListEncoding::ZIPLIST) {
        get_ziplist().push_back(std::move(value));
    } else {
        get_linkedlist().push_back(std::move(value));
    }
}

bool ListObject::pop_front(std::string& out) {
    if (size() == 0) {
        return false;
    }

    if (encoding_ == ListEncoding::ZIPLIST) {
        auto& values = get_ziplist();
        out = std::move(values.front());
        values.erase(values.begin());
    } else {
        auto& values = get_linkedlist();
        out = std::move(values.front());
        values.pop_front();
    }
    return true;
}

bool ListObject::pop_back(std::string& out) {
    if (size() == 0) {
        return false;
    }

    if (encoding_ == ListEncoding::ZIPLIST) {
        auto& values = get_ziplist();
        out = std::move(values.back());
        values.pop_back();
    } else {
        auto& values = get_linkedlist();
        out = std::move(values.back());
        values.pop_back();
    }
    return true;
}

bool ListObject::get(long long index, std::string& out) const {
    const auto position = normalize_index(index, size());
    if (!position) {
        return false;
    }

    out = encoding_ == ListEncoding::ZIPLIST ? get_ziplist()[*position]
                                               : get_linkedlist()[*position];
    return true;
}

std::vector<std::string> ListObject::range(long long start, long long stop) const {
    const auto positions = normalize_range(start, stop, size());
    if (!positions) {
        return {};
    }

    const auto [first, last] = *positions;
    if (encoding_ == ListEncoding::ZIPLIST) {
        const auto& values = get_ziplist();
        return {values.begin() + first, values.begin() + last + 1};
    }

    const auto& values = get_linkedlist();
    return {values.begin() + first, values.begin() + last + 1};
}

long long ListObject::remove(long long count, const std::string& value) {
    auto remove_from = [&](auto& values) {
        long long removed = 0;
        if (count >= 0) {
            const long long limit = count == 0 ? -1 : count;
            for (auto it = values.begin(); it != values.end() &&
                 (limit < 0 || removed < limit);) {
                if (*it == value) {
                    it = values.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        } else {
            const long long limit = -count;
            for (auto it = values.rbegin(); it != values.rend() && removed < limit;) {
                if (*it == value) {
                    it = std::make_reverse_iterator(values.erase(std::next(it).base()));
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        return removed;
    };

    return encoding_ == ListEncoding::ZIPLIST ? remove_from(get_ziplist())
                                               : remove_from(get_linkedlist());
}

void ListObject::trim(long long start, long long stop) {
    const auto positions = normalize_range(start, stop, size());
    if (!positions) {
        if (encoding_ == ListEncoding::ZIPLIST) {
            get_ziplist().clear();
        } else {
            get_linkedlist().clear();
        }
        return;
    }

    const auto [first, last] = *positions;
    if (encoding_ == ListEncoding::ZIPLIST) {
        auto& values = get_ziplist();
        std::vector<std::string> trimmed(values.begin() + first, values.begin() + last + 1);
        values = std::move(trimmed);
    } else {
        auto& values = get_linkedlist();
        std::deque<std::string> trimmed(values.begin() + first, values.begin() + last + 1);
        values = std::move(trimmed);
    }
}

bool ListObject::insert(const std::string& pivot, const std::string& value,
                        ListInsertPosition position) {
    if (encoding_ == ListEncoding::ZIPLIST &&
        (get_ziplist().size() + 1 > ZIPLIST_MAX_ENTRIES ||
         value.size() > ZIPLIST_MAX_ENTRY_SIZE)) {
        promote_to_linkedlist();
    }

    if (encoding_ == ListEncoding::ZIPLIST) {
        auto& values = get_ziplist();
        const auto it = std::find(values.begin(), values.end(), pivot);
        if (it == values.end()) {
            return false;
        }
        values.insert(position == ListInsertPosition::BEFORE ? it : std::next(it), value);
    } else {
        auto& values = get_linkedlist();
        const auto it = std::find(values.begin(), values.end(), pivot);
        if (it == values.end()) {
            return false;
        }
        values.insert(position == ListInsertPosition::BEFORE ? it : std::next(it), value);
    }
    return true;
}

std::vector<std::string> ListObject::values() const {
    if (encoding_ == ListEncoding::ZIPLIST) {
        return get_ziplist();
    }
    const auto& linkedlist = get_linkedlist();
    return {linkedlist.begin(), linkedlist.end()};
}

size_t ListObject::size() const {
    return encoding_ == ListEncoding::ZIPLIST ? get_ziplist().size()
                                               : get_linkedlist().size();
}

size_t ListObject::memory_usage() const {
    size_t total = sizeof(*this);
    for (const auto& value : values()) {
        total += sizeof(std::string) + value.capacity();
    }
    return total;
}
