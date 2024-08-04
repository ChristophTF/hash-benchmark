#include <iostream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "absl/container/flat_hash_set.h"
#include "absl/container/flat_hash_map.h"
#include "xxhash.h"

using random_bytes_engine = std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned char>;

namespace
{
    template<typename T, bool almost_equal>
    std::vector<T> generate_random_strings(size_t n, size_t m)
    {
        random_bytes_engine rbeFirstPart;

        std::vector<T> result;
        random_bytes_engine rbe;
        std::uniform_int_distribution<char> dist('a', 'z');
        result.reserve(n);
        for(size_t i = 0; i < n; i++)
        {
            auto& str = result.emplace_back();
            str.resize(m);
            if constexpr(almost_equal) {
                std::generate(str.data(), str.data() + str.size() - 4, [&dist, rbeFirstPart]() mutable { return dist(rbeFirstPart); });
                std::generate(str.data() + str.size() - 4, str.data() + str.size(), [&]{ return dist(rbe); });
            } else {
                std::generate(str.data(), str.data() + str.size(), [&]{ return dist(rbe); });
            }
        }
        return result;
    }

    template<bool size_known>
    struct SConstructVector;

    template<>
    struct SConstructVector<true> final {
        template<typename T>
        std::vector<T> operator()(std::span<T const> rng) noexcept {
            return {rng.begin(), rng.end()};
        }
    };

    template<>
    struct SConstructVector<false> final {
        template<typename T>
        std::vector<T> operator()(std::span<T const> rng) noexcept {
            std::vector<T> vec;
            for(auto const& str: rng)
            {
                vec.push_back(str);
            }
            return vec;
        }
    };

    template<typename T, bool size_known>
    bool compare_multiset_sort(std::span<T const> rngstr1, std::span<T const> rngstr2)
    {
        std::vector<T> vecstr1 = SConstructVector<size_known>{}(rngstr1);
        std::sort(vecstr1.begin(), vecstr1.end());
        std::vector<T> vecstr2 = SConstructVector<size_known>{}(rngstr2);
        std::sort(vecstr2.begin(), vecstr2.end());
        return vecstr1 == vecstr2;
    }

    template<typename T, bool size_known, typename Hash = std::hash<T>>
    bool compare_multiset_unordered_multiset(std::span<T const> rngstr1, std::span<T const> rngstr2)
    {
        std::unordered_multiset<T, Hash> set1;
        if (size_known) set1.reserve(rngstr1.size());
        std::copy(rngstr2.begin(), rngstr2.end(), std::inserter(set1, set1.end()));
        for(auto const& str: rngstr1)
        {
            if(auto it = set1.find(str); it != set1.end()) set1.erase(it);
            else return false;
        }
        return true;
    }

    template<typename T, bool size_known, typename Hash = std::hash<T>>
    bool compare_multiset_hashtable_1(std::span<T const> rngstr1, std::span<T const> rngstr2)
    {
        absl::flat_hash_map <T, size_t, Hash> set1;
        if (size_known)
        {
            set1.reserve(rngstr1.size());
        }
        for(auto const& str: rngstr1)
        {
            ++set1[str];
        }
        for(auto const& str: rngstr2)
        {
            if(set1[str]-- == 0) return false;
        }
        return true;
    }
}

template<typename T>
using BenchmarkFunction = bool(*)(std::span<T const>, std::span<T const>);

template<size_t m>
struct boomer_string final : std::array<char, m> {
    void resize(size_t m_new) noexcept {
        assert(m_new == m);
    }
};


struct C_String {
    std::unique_ptr<char[]> str;
    bool operator==(C_String const& other) const {
        return strcmp(str.get(), other.str.get()) == 0;
    }

    bool operator<(C_String const& other) const {
        return strcmp(str.get(), other.str.get()) < 0;
    }

    void resize(size_t m_new) noexcept {
        assert(!str);
        str = std::make_unique<char[]>(m_new + 1);
        str[m_new] = 0;
        assert(m_new == 100);
    }

    char* data() const noexcept {
        return str.get();
    }

    size_t size() const noexcept {
        return 100; // TODO!
    }

    C_String() noexcept = default;

    C_String(C_String const& other) {
        resize(100);
        memcpy(str.get(), other.str.get(), 100);
    }

    C_String& operator=(C_String const& other)
    {
        if(this == &other) return *this;
        if (!str) resize(100);
        memcpy(str.get(), other.str.get(), 100);
        return *this;
    }
};

namespace std {
    template<size_t m>
    struct hash<boomer_string<m>> final {
        size_t operator()(boomer_string<m> const& str) const noexcept {
            return std::_Hash_impl::hash(str.data(), str.size());
        }
    };

    template<>
    struct hash<C_String> final {
        size_t operator()(C_String const& str) const noexcept {
            return std::_Hash_impl::hash(str.data(), str.size());
        }
    };
}

struct XXHash64 final {
    uint64_t operator()(auto const& str) const noexcept {
        return XXH64(str.data(), str.size(), 0);
    }
};


template<typename T>
static void RegisterBenchmarks(char const* prefix)
{
    std::vector<std::tuple<BenchmarkFunction<T>, char const*>> functions;
    auto const append = [prefix, &functions](BenchmarkFunction<T> fn, char const* name) noexcept {
        functions.emplace_back(fn, name);
    };

    append(compare_multiset_sort<T, true>, "sort");
    append(compare_multiset_unordered_multiset<T, true>, "unordered_multiset");
    append(compare_multiset_hashtable_1<T, true>, "flat_hash_map");
    append(compare_multiset_sort<T, false>, "sort-unknown_size");
    append(compare_multiset_unordered_multiset<T, false>, "unordered_multiset-unknown_size");
    append(compare_multiset_hashtable_1<T, false>, "flat_hash_map-unknown_size");

    auto const registerBenchmarks = [&]<bool almost_equal>()
    {
        for(auto& [function, function_name] : functions)
        {
            auto* bench = benchmark::RegisterBenchmark(std::string(function_name).append("/").append(prefix).append("/").append(almost_equal ? "almost_equal" : "random").append("/100").c_str(), [function](benchmark::State& state) {
                state.SetComplexityN(state.range(0));
                auto rngstr1 = generate_random_strings<T, almost_equal>(state.range(0), 100);
                auto rngstr2 = generate_random_strings<T, almost_equal>(state.range(0), 100);
                for(auto _ : state)
                {
                    benchmark::DoNotOptimize(function(rngstr1, rngstr2));
                }
            });
            for(long n = 1024; n <= 32 * 1024 * 1024; n *= 2)
                bench->Arg(n);
            bench->Unit(benchmark::kNanosecond);
        }
    };

    registerBenchmarks.template operator()<false>();
    registerBenchmarks.template operator()<true>();
}

int main(int argc, char** argv)
{
    RegisterBenchmarks<std::string>("std::string");
    RegisterBenchmarks<boomer_string<100>>("boomer_string");
    RegisterBenchmarks<C_String>("C_string");

    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}