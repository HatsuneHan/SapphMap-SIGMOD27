#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include "ohash_tiers.hpp"
#include "oram_iterator.hpp"

#ifndef H2O2RAM_DIAGNOSTICS
#define H2O2RAM_DIAGNOSTICS 0
#endif

namespace h2o2ram_ns {

    template <typename IndexType,
              typename ValueType>
    class ObliviousRAM
    {
    public:
        using value_type = ValueType;
        using reference = ValueType &;
        using const_reference = const ValueType &;
        using size_type = IndexType;
        using difference_type = typename std::make_signed<size_type>::type;
        using iterator = ObliviousRAMIterator<size_type, ValueType>;
        using const_iterator = const ObliviousRAMIterator<size_type, ValueType>;
        using reverse_iterator = ObliviousRAMIteratorReverse<size_type, ValueType>;
        using const_reverse_iterator = const ObliviousRAMIteratorReverse<size_type, ValueType>;

        struct Stats
        {
            uint64_t logical_reads = 0;
            uint64_t logical_inserts = 0;
            uint64_t logical_erases = 0;
            uint64_t explicit_flushes = 0;
            uint64_t level_probes = 0;
            uint64_t rebuilds = 0;
            uint64_t rebuilt_blocks = 0;
        };

        struct Diagnostics
        {
            uint64_t logical_size = 0;
            uint64_t logical_capacity = 0;
            uint64_t block_bytes = 0;
            uint64_t buffer_items = 0;
            uint64_t buffer_capacity = 0;
            uint64_t total_levels = 0;
            uint64_t occupied_levels = 0;
            uint64_t total_level_capacity = 0;
            uint64_t occupied_level_capacity = 0;
        };

    protected:
        using BlockType = Block<size_type, sizeof(ValueType) + sizeof(size_type)>;
        // This buffer is only 2 * LINEAR_SCAN_THRESHOLD blocks.  Keeping it
        // local matches upstream and avoids a complete external-vector cache
        // allocation (and unstable cached references) for a few KiB of data.
        using LinearBuffer = std::vector<BlockType>;

    private:
        size_type _size;
        size_type _capacity;
        iterator _begin;
        iterator _end;
        mutable std::vector<ObliviousBin<size_type, sizeof(ValueType) + sizeof(size_type)>>
            hash_tables;
#if !CACHE_OBLIVIOUS
        mutable std::unique_ptr<LinearBuffer> _linear_scan_buffer;
#else
        mutable std::map<IndexType, BlockType> _linear_scan_buffer;
#endif
        mutable IndexType _dummy_ctr;
        mutable size_type _buffer_cnt;
        mutable std::mt19937 gen;
        mutable Stats stats_;

        bool synthetic_compact_ = false;
        uint64_t synthetic_seed_ = 0;
        size_t synthetic_reset_levels_ = 0;

        size_type linear_scan_threshold;
        size_type delta_inv_log2;

        static size_type checked_double(size_type value)
        {
            if (value > std::numeric_limits<size_type>::max() / 2)
                throw std::length_error("H2O2RAM capacity overflow");
            return static_cast<size_type>(value * 2);
        }

        static size_type checked_mul(size_type a, size_type b)
        {
            if (a != 0 && b > std::numeric_limits<size_type>::max() / a)
                throw std::length_error("H2O2RAM merge-size overflow");
            return static_cast<size_type>(a * b);
        }

        std::vector<uint8_t> generate_flags(size_t n) const
        {
            std::vector<uint8_t> flags(n);
            size_t x = n / 2;
            size_t y = n / 2;
            for (size_t i = 0; i < n; i++)
            {
                double p = static_cast<double>(y) / (x + y);
                std::bernoulli_distribution dist(p);
                flags[i] = dist(gen);
                y = oblivious_select(y, y - 1, flags[i]);
                x = oblivious_select(x, x - 1, !flags[i]);
            }
            return flags;
        }

        void clear_buffer_if_full() const
        {
            if (_buffer_cnt == linear_scan_threshold)
                [[unlikely]]
            {
    #if !CACHE_OBLIVIOUS
                std::vector<BlockType> extracted_data(_buffer_cnt);
                for (size_t i = 0; i < _buffer_cnt; i++)
                    extracted_data[i] = (*_linear_scan_buffer)[i];
                uint32_t L = 0;
                for (; L < hash_tables.size() && !hash_tables[L].empty(); L++)
                    ;
                if (L >= std::numeric_limits<size_type>::digits)
                    throw std::length_error("H2O2RAM level overflow");
                extracted_data.resize(checked_mul(
                    linear_scan_threshold, static_cast<size_type>(size_type(1) << L)));
                auto tmp_ptr = extracted_data.data();
                for (uint32_t i = 0; i < L; ++i)
                {
                    auto &current_data = hash_tables[i].extract();
                    const auto cur_size = current_data.size();
                    for (size_t ci = 0; ci < cur_size; ++ci)
                        tmp_ptr[ci + cur_size] = current_data[ci];
                }
                if (L == hash_tables.size())
                {
                    stateless_osorter(extracted_data.data(), extracted_data.size(),
                                      [](const BlockType &a, const BlockType &b)
                                      {
                                          const bool different_dummy =
                                              a.dummy() != b.dummy();
                                          const bool real_first = !a.dummy();
                                          const bool id_order = a.id < b.id;
                                          return (different_dummy & real_first) |
                                                 (!different_dummy & id_order);
                                      });
                    --L;
                }
#if H2O2RAM_DIAGNOSTICS
                ++stats_.rebuilds;
                stats_.rebuilt_blocks += extracted_data.size();
#endif
                hash_tables[L].build(extracted_data.data());
                _buffer_cnt = 0;
    #else
                assert(_linear_scan_buffer.size() == linear_scan_threshold);
                std::vector<BlockType> extracted_data;
                for (auto &p : _linear_scan_buffer)
                    extracted_data.emplace_back(p.second);
                _linear_scan_buffer.clear();
                _dummy_ctr = 0;
                uint32_t L = 0;
                for (; L < hash_tables.size() && !hash_tables[L].empty(); L++)
                    ;
                const size_t merge_buf_size = linear_scan_threshold * (1ULL << L);
                extracted_data.resize(merge_buf_size);
                auto tmp_ptr = extracted_data.data();
                for (uint32_t i = 0; i < L; ++i)
                {
                    auto &current_data = hash_tables[i].extract();
                    const auto cur_size = current_data.size();
                    for (size_t ci = 0; ci < cur_size; ++ci)
                        tmp_ptr[ci + cur_size] = current_data[ci];
                }
                if (L == hash_tables.size())
                {
                    stateless_osorter(extracted_data.data(), extracted_data.size(),
                                      [](const BlockType &a, const BlockType &b)
                                      {
                                          const bool different_dummy =
                                              a.dummy() != b.dummy();
                                          const bool real_first = !a.dummy();
                                          const bool id_order = a.id < b.id;
                                          return (different_dummy & real_first) |
                                                 (!different_dummy & id_order);
                                      });
                    --L;
                }
#if H2O2RAM_DIAGNOSTICS
                ++stats_.rebuilds;
                stats_.rebuilt_blocks += extracted_data.size();
#endif
                hash_tables[L].build(extracted_data.data());
                _buffer_cnt = 0;
    #endif
            }
        }

    public:
        ObliviousRAM(size_type _size = 0,
                     value_type val = value_type(),
                     size_type linear_scan_threshold = LINEAR_SCAN_THRESHOLD,
                     size_type delta_inv_log2 = DELTA_INV_LOG2)
            : _size(_size),
              _begin(this, 0),
              _end(this, _size),
              _dummy_ctr(0),
              _buffer_cnt(0),
              gen(H2O2RAM::secure_seed()),
              linear_scan_threshold(linear_scan_threshold),
              delta_inv_log2(delta_inv_log2)

        {
            hash_tables.reserve(64);
            _capacity = linear_scan_threshold;
            while (_capacity < std::max(_size, (size_type)MIN_CAPACITY))
            {
                hash_tables.emplace_back(_capacity, _capacity, delta_inv_log2);
                if (hash_tables.back().is_linear_scan())
                {
                    hash_tables.pop_back();
                    this->linear_scan_threshold = _capacity;
                }
                _capacity = checked_double(_capacity);
            }
            this->linear_scan_threshold = checked_double(this->linear_scan_threshold);
            hash_tables.emplace_back(_capacity, _capacity, delta_inv_log2);
            _linear_scan_buffer = std::make_unique<LinearBuffer>(this->linear_scan_threshold,
                                                                  BlockType());
            if (_size == 0)
                return;
            BlockType *blocks = new BlockType[_capacity];
            for (size_t i = 0; i < _capacity; i++)
            {
                blocks[i].id = i;
                std::memcpy(&blocks[i].value, &val, sizeof(value_type));
            }
            hash_tables.back().build(blocks);
            delete[] blocks;
        }

        template <typename _InputIt>
        ObliviousRAM(_InputIt first, _InputIt last,
                     size_type linear_scan_threshold = LINEAR_SCAN_THRESHOLD,
                     size_type delta_inv_log2 = DELTA_INV_LOG2)
            : _dummy_ctr(0),
              _buffer_cnt(0),
              gen(H2O2RAM::secure_seed()),
              linear_scan_threshold(linear_scan_threshold),
              delta_inv_log2(delta_inv_log2)
        {
            hash_tables.reserve(64);
            _size = std::distance(first, last);
            _begin = iterator(this, 0);
            _end = iterator(this, _size);
            _capacity = linear_scan_threshold;
            while (_capacity < std::max(_size, (size_type)MIN_CAPACITY))
            {
                hash_tables.emplace_back(_capacity, _capacity, delta_inv_log2);
                if (hash_tables.back().is_linear_scan())
                {
                    hash_tables.pop_back();
                    this->linear_scan_threshold = _capacity;
                }
                _capacity = checked_double(_capacity);
            }
            this->linear_scan_threshold = checked_double(this->linear_scan_threshold);
            hash_tables.emplace_back(_capacity, _capacity, delta_inv_log2);
            _linear_scan_buffer = std::make_unique<LinearBuffer>(this->linear_scan_threshold,
                                                                  BlockType());
            BlockType *blocks = new BlockType[_capacity];
            size_t idx = 0;
            for (auto it = first; it != last; ++it, ++idx)
            {
                blocks[idx] = BlockType(*it, idx);
            }
            for (size_t i = _size; i < _capacity; i++)
                blocks[i] = BlockType();

            hash_tables.back().build(blocks);
            delete[] blocks;
        }

        // implement copy constructor and copy assignment operator
        ObliviousRAM(const ObliviousRAM &other)
            : _size(other._size),
              _capacity(other._capacity),
              _begin(this, 0),
              _end(this, other._size),
              hash_tables(other.hash_tables),
              _dummy_ctr(other._dummy_ctr),
              _buffer_cnt(other._buffer_cnt),
              gen(H2O2RAM::secure_seed()),
              stats_(other.stats_),
              linear_scan_threshold(other.linear_scan_threshold),
              delta_inv_log2(other.delta_inv_log2)
        {
            auto other_buf_size = other._linear_scan_buffer->size();
            _linear_scan_buffer = std::make_unique<LinearBuffer>(other_buf_size,
                                                                  BlockType());
            for (size_t i = 0; i < other_buf_size; i++)
                (*_linear_scan_buffer)[i] = (*other._linear_scan_buffer)[i];
        }

        reference operator[](size_type index) const
        {
#if H2O2RAM_DIAGNOSTICS
            ++stats_.logical_reads;
#endif
            clear_buffer_if_full();
            BlockType res;
#if !CACHE_OBLIVIOUS
            for (size_t i = 0; i < _buffer_cnt; i++)
            {
                auto &_ = (*_linear_scan_buffer)[i];
                CMOV(_.id == index, res, _);
                CMOV(_.id == index, _.id, IndexType(-1));
            }
#else
            auto it = _linear_scan_buffer.find(index);
            if (it == _linear_scan_buffer.end())
            {
                res = BlockType();
            }
            else
            {
                auto &_ = it->second;
                res = _;
                _.id = IndexType(-1);
            }
#endif
            for (size_t ti = 0; ti < hash_tables.size(); ++ti)
            {
                auto &table = hash_tables[ti];
                if (table.empty())
                    continue;
#if H2O2RAM_DIAGNOSTICS
                ++stats_.level_probes;
#endif
                IndexType cur_idx = index;
                CMOV(!res.dummy(), cur_idx, IndexType(-1));
                auto cur_res = table[cur_idx];
                CMOV(res.dummy(), res, cur_res);
            }
#if !CACHE_OBLIVIOUS
            (*_linear_scan_buffer)[_buffer_cnt++] = res;
            return *(value_type *)(&((*_linear_scan_buffer)[_buffer_cnt - 1].value));
#else
            _buffer_cnt++;
            if (it != _linear_scan_buffer.end() || index == IndexType(-1))
            {
                it->second = std::move(res);
                _linear_scan_buffer[--_dummy_ctr];
                return *(value_type *)(&it->second.value);
            }
            else
            {
                _linear_scan_buffer[index] = std::move(res);
                return *(value_type *)(&(_linear_scan_buffer[index].value));
            }
#endif
        }

        ObliviousRAM &operator=(const ObliviousRAM &other)
        {
            ObliviousRAM tmp(other);
            swap(tmp);
            return *this;
        }

        // implement move constructor and move assignment operator
        ObliviousRAM(ObliviousRAM &&other) noexcept
            : _size(std::move(other._size)),
              _capacity(std::move(other._capacity)),
              _begin(this, 0),
              _end(this, other._size),
              hash_tables(std::move(other.hash_tables)),
              _linear_scan_buffer(std::move(other._linear_scan_buffer)),
              _dummy_ctr(std::move(other._dummy_ctr)),
              _buffer_cnt(std::move(other._buffer_cnt)),
              gen(std::move(other.gen)),
              stats_(std::move(other.stats_)),
              linear_scan_threshold(std::move(other.linear_scan_threshold)),
              delta_inv_log2(std::move(other.delta_inv_log2))
        {
        }

        iterator push(const ValueType &value)
        {
            return insert(_size, value);
        }

        iterator insert(IndexType index, const ValueType &value)
        {
#if H2O2RAM_DIAGNOSTICS
            ++stats_.logical_inserts;
#endif
            if (_size == _capacity)
            {
                _capacity = checked_double(_capacity);
                hash_tables.emplace_back(_capacity, _capacity, delta_inv_log2);
            }
            clear_buffer_if_full();
            _size++;
            (*_linear_scan_buffer)[_buffer_cnt++] = {value, index};
            _end = iterator(this, _size);
            return iterator(this, index);
        }

        // erase
        void erase(IndexType index)
        {
#if H2O2RAM_DIAGNOSTICS
            ++stats_.logical_erases;
#endif
            clear_buffer_if_full();
            BlockType res;
            for (size_t i = 0; i < _buffer_cnt; i++)
            {
                auto &_ = (*_linear_scan_buffer)[i];
                CMOV(_.id == index, res, _);
                CMOV(_.id == index, _.id, IndexType(-1));
            }
            for (auto &table : hash_tables)
            {
                if (table.empty())
                    continue;
#if H2O2RAM_DIAGNOSTICS
                ++stats_.level_probes;
#endif
                IndexType cur_idx = index;
                CMOV(!res.dummy(), cur_idx, IndexType(-1));
                auto cur_res = table[cur_idx];
                CMOV(res.dummy(), res, cur_res);
            }
        }

        // front
        void flush()
        {
#if H2O2RAM_DIAGNOSTICS
            ++stats_.explicit_flushes;
#endif
            if (_buffer_cnt == 0)
                return;
#if !CACHE_OBLIVIOUS
            for (size_t i = _buffer_cnt; i < linear_scan_threshold; ++i)
                (*_linear_scan_buffer)[i] = BlockType();
#else
            while (_linear_scan_buffer.size() < linear_scan_threshold)
                _linear_scan_buffer[--_dummy_ctr] = BlockType();
#endif
            _buffer_cnt = linear_scan_threshold;
            clear_buffer_if_full();
        }

        // front
        const_reference front() const
        {
            return operator[](0);
        }

        // pop_back
        void pop_back()
        {
            if (_size == 0)
                return;
            _size--;
            _end = iterator(this, _size);
        }

        ObliviousRAM &operator=(ObliviousRAM &&other) noexcept
        {
            swap(other);
            return *this;
        }

        iterator begin()
        {
            return _begin;
        }
        iterator end()
        {
            return _end;
        }

        const_iterator cbegin() const
        {
            return _begin;
        }

        const_iterator cend() const
        {
            return _end;
        }

        reverse_iterator rbegin()
        {
            return reverse_iterator(this, _size - 1);
        }

        reverse_iterator rend()
        {
            return reverse_iterator(this, size_type(-1));
        }

        bool operator==(const ObliviousRAM &other) const
        {
            if (_size != other._size || _capacity != other._capacity || _buffer_cnt != other._buffer_cnt)
                return false;
            for (size_t i = 0; i < _buffer_cnt; i++)
                if ((*_linear_scan_buffer)[i] != (*other._linear_scan_buffer)[i])
                    return false;
            return hash_tables == other.hash_tables;
        }

        bool operator!=(const ObliviousRAM &other) const
        {
            return !(*this == other);
        }

        void swap(ObliviousRAM &other)
        {
            std::swap(_size, other._size);
            std::swap(_capacity, other._capacity);
            std::swap(hash_tables, other.hash_tables);
            std::swap(_linear_scan_buffer, other._linear_scan_buffer);
            std::swap(_dummy_ctr, other._dummy_ctr);
            std::swap(_buffer_cnt, other._buffer_cnt);
            std::swap(gen, other.gen);
            std::swap(stats_, other.stats_);
            std::swap(linear_scan_threshold, other.linear_scan_threshold);
            std::swap(delta_inv_log2, other.delta_inv_log2);
            _begin = iterator(this, 0);
            _end = iterator(this, _size);
            other._begin = iterator(&other, 0);
            other._end = iterator(&other, other._size);
        }

        size_type maxsize() const
        {
            return std::numeric_limits<size_type>::max();
        }

        size_type size() const
        {
            return _size;
        }

        Stats stats() const
        {
            return stats_;
        }

        void reset_stats() const
        {
            stats_ = Stats{};
        }

        Diagnostics diagnostics() const
        {
            Diagnostics d;
            d.logical_size = static_cast<uint64_t>(_size);
            d.logical_capacity = static_cast<uint64_t>(_capacity);
            d.block_bytes = sizeof(BlockType);
            d.buffer_items = static_cast<uint64_t>(_buffer_cnt);
            d.buffer_capacity = static_cast<uint64_t>(linear_scan_threshold);
            d.total_levels = hash_tables.size();
            for (const auto &table : hash_tables)
            {
                const uint64_t capacity = static_cast<uint64_t>(table.capacity());
                d.total_level_capacity += capacity;
                if (!table.empty())
                {
                    ++d.occupied_levels;
                    d.occupied_level_capacity += capacity;
                }
            }
            return d;
        }

        size_t level_count() const
        {
            return hash_tables.size();
        }

        uint64_t level_capacity(size_t level) const
        {
            if (level >= hash_tables.size())
                throw std::out_of_range("H2O2RAM level index");
            return static_cast<uint64_t>(hash_tables[level].capacity());
        }

        bool level_occupied(size_t level) const
        {
            if (level >= hash_tables.size())
                throw std::out_of_range("H2O2RAM level index");
            return !hash_tables[level].empty();
        }

        bool empty() const
        {
            return _size == 0;
        }

        /**
         * Construct the exact compact topology reached after flushing 2^k
         * inserts, without replaying those inserts.  Empty lower levels retain
         * the same extracted shape as the real structure; the occupied top
         * level contains only authenticated dummy pages.
         */
        void prepare_synthetic_compact(size_type logical_size,
                                       const size_type* query_keys,
                                       size_t query_count,
                                       uint64_t deterministic_seed)
        {
            if (logical_size < static_cast<size_type>(MIN_CAPACITY) ||
                (logical_size & (logical_size - 1)) != 0)
                throw std::invalid_argument(
                    "H2O2RAM synthetic compact size must be a power of two >= MIN_CAPACITY");

            H2O2RAM::set_deterministic_rng(deterministic_seed);
            hash_tables.clear();
            hash_tables.reserve(64);

            const size_type buffer_capacity = linear_scan_threshold;
            for (size_type level_capacity = buffer_capacity;;
                 level_capacity = checked_double(level_capacity))
            {
                hash_tables.emplace_back(level_capacity, level_capacity,
                                         delta_inv_log2);
                if (level_capacity == logical_size)
                    break;
                if (level_capacity > logical_size)
                    throw std::logic_error(
                        "H2O2RAM synthetic level capacity overshot target");
            }

            for (size_t i = 0; i + 1 < hash_tables.size(); ++i)
                hash_tables[i].prepare_synthetic_extracted();
            hash_tables.back().prepare_synthetic_occupied();

            _size = logical_size;
            _capacity = logical_size;
            _begin = iterator(this, 0);
            _end = iterator(this, _size);
            _dummy_ctr = 0;
            _buffer_cnt = 0;
            std::fill(_linear_scan_buffer->begin(),
                      _linear_scan_buffer->end(), BlockType());

            // Dry-probe only the occupied top hash. With late initialization,
            // this creates valid encrypted disk pages exactly for the fixed
            // synthetic trace while avoiding any ORAM buffer/rebuild work.
            for (size_t i = 0; i < query_count; ++i)
                (void)hash_tables.back()[query_keys[i]];
            hash_tables.back().flush_cache(true);
            hash_tables.back().reset_synthetic_query_state();

            const uint64_t flush_count =
                (query_count + static_cast<uint64_t>(buffer_capacity) - 1) /
                static_cast<uint64_t>(buffer_capacity);
            synthetic_reset_levels_ = 0;
            uint64_t carries = flush_count;
            while (carries > 1) {
                ++synthetic_reset_levels_;
                carries = (carries + 1) / 2;
            }
            if (!hash_tables.empty())
                synthetic_reset_levels_ = std::min(
                    synthetic_reset_levels_, hash_tables.size() - 1);

            synthetic_compact_ = true;
            synthetic_seed_ = deterministic_seed;
            reset_stats();
            // Every timed repetition starts the lower-level PRF stream at the
            // same point; the already-built top PRFs remain unchanged.
            H2O2RAM::set_deterministic_rng(synthetic_seed_);
        }

        /** Restore the same compact snapshot before a repeated PQ run. */
        void reset_synthetic_compact()
        {
            if (!synthetic_compact_ || hash_tables.empty())
                throw std::logic_error(
                    "H2O2RAM synthetic compact state was not prepared");

            for (auto& table : hash_tables)
                table.flush_cache(true);

            for (size_t i = 0; i + 1 < hash_tables.size(); ++i) {
                if (i <= synthetic_reset_levels_)
                    hash_tables[i].prepare_synthetic_extracted();
                else
                    hash_tables[i].mark_synthetic_empty();
            }
            hash_tables.back().mark_synthetic_occupied();

            _dummy_ctr = 0;
            _buffer_cnt = 0;
            std::fill(_linear_scan_buffer->begin(),
                      _linear_scan_buffer->end(), BlockType());
            reset_stats();
            H2O2RAM::set_deterministic_rng(synthetic_seed_);
        }
    };

} // namespace h2o2ram_ns

namespace std
{
    template <typename IndexType,
              typename ValueType>
    inline void swap(h2o2ram_ns::ObliviousRAM<IndexType, ValueType> &a,
                     h2o2ram_ns::ObliviousRAM<IndexType, ValueType> &b)
    {
        a.swap(b);
    }
}
