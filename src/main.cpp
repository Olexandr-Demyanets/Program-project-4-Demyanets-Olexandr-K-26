// Компілятор: g++ 15.2.0 (MSYS2 UCRT64), C++20
// Програмний проєкт 4, варіант 5: вимірювання часу роботи структури ThreeFields
// на згенерованих файлах (сценарії а, б, в) в 1, 2 і 3 потоках.
//
// Використання: benchmark [каталог з файлами = tests] [кількість вимірів = 10]
//               benchmark --compare [каталог з файлами = tests] [кількість вимірів = 5]
//               (порівняння з альтернативними схемами захисту)
// Спочатку слід згенерувати файли програмою generator.
#include "three_fields.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

struct Scenario
{
    char id;
    const char* title;
};

constexpr Scenario kScenarios[] = {
    {'a', "frequencies of variant 5"},
    {'b', "all frequencies equal"},
    {'c', "far from variant, not equal"},
};
constexpr int kScenarioCount = 3;
constexpr int kMaxThreads = 3;

string testFileName(char scenario, int threads, int index)
{
    return string(1, scenario) + "_t" + to_string(threads) + "_" + to_string(index) + ".txt";
}

enum class OpType : uint8_t { Read, Write, String };

struct Operation
{
    OpType type;
    uint8_t field;
    int value;
};

using OpSequence = vector<Operation>;

int splitWords(string_view line, string_view words[], int maxWords)
{
    int count = 0;
    size_t pos = 0;
    while (true)
    {
        pos = line.find_first_not_of(" \t", pos);
        if (pos == string_view::npos) return count;
        const size_t end = min(line.find_first_of(" \t", pos), line.size());
        if (count == maxWords) return maxWords + 1;
        words[count++] = line.substr(pos, end - pos);
        pos = end;
    }
}

bool parseInt(string_view text, int& value)
{
    const auto [ptr, ec] = from_chars(text.data(), text.data() + text.size(), value);
    return ec == errc() && ptr == text.data() + text.size();
}

fs::path directoryFromArg(const char* arg)
{
    try
    {
        return fs::path(arg);
    }
    catch (const fs::filesystem_error&)
    {
        throw invalid_argument("directory name in the argument must consist of ASCII characters only");
    }
}

int parseRepeats(const char* text)
{
    const string s = text;
    int value = 0;
    const auto [ptr, ec] = from_chars(s.data(), s.data() + s.size(), value);
    if (ec != errc() || ptr != s.data() + s.size() || value <= 0)
        throw invalid_argument("number of repeats must be a positive integer, got \"" + s + "\"");
    return value;
}

OpSequence loadOperations(const fs::path& path, int fieldCount)
{
    ifstream in(path, ios::binary);
    if (!in) throw runtime_error("cannot open file " + path.string());
    string text;
    in.seekg(0, ios::end);
    const streamoff size = in.tellg();
    if (size < 0) throw runtime_error("cannot read file " + path.string());
    text.resize(static_cast<size_t>(size));
    in.seekg(0, ios::beg);
    in.read(text.data(), static_cast<streamsize>(text.size()));
    if (!in) throw runtime_error("cannot read file " + path.string());

    OpSequence ops;
    ops.reserve(static_cast<size_t>(count(text.begin(), text.end(), '\n')) + 1);

    const string_view all = text;
    size_t lineStart = 0;
    long long lineNo = 0;
    while (lineStart < all.size())
    {
        size_t lineEnd = all.find('\n', lineStart);
        if (lineEnd == string_view::npos) lineEnd = all.size();
        string_view line = all.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        string_view w[3];
        const int n = splitWords(line, w, 3);
        if (n == 0) continue;

        auto fail = [&](const string& why) {
            throw runtime_error(path.string() + ":" + to_string(lineNo) + ": " + why + " in line \"" + string(line) + "\"");
        };
        auto parseField = [&](string_view token) {
            int field = 0;
            if (!parseInt(token, field) || field < 0 || field >= fieldCount)
                fail("field number must be in [0, " + to_string(fieldCount) + ")");
            return static_cast<uint8_t>(field);
        };

        if (w[0] == "read" && n == 2)
        {
            ops.push_back({OpType::Read, parseField(w[1]), 0});
        }
        else if (w[0] == "write" && n == 3)
        {
            const uint8_t field = parseField(w[1]);
            int value = 0;
            if (!parseInt(w[2], value)) fail("bad value");
            ops.push_back({OpType::Write, field, value});
        }
        else if (w[0] == "string" && n == 1)
        {
            ops.push_back({OpType::String, 0, 0});
        }
        else
        {
            fail("unknown command or wrong number of arguments");
        }
    }
    return ops;
}

struct TestSet
{
    vector<string> fileNames;
    vector<OpSequence> sequences;
    long long totalOps = 0;
};

TestSet loadTestSet(const fs::path& dir, char scenario, int threads)
{
    TestSet set;
    for (int i = 0; i < threads; ++i)
    {
        const string name = testFileName(scenario, threads, i);
        const fs::path path = dir / name;
        if (!fs::exists(path))
            throw runtime_error("file " + path.string() +
                                " not found; run generator first or pass the directory with test files as an argument");
        set.fileNames.push_back(name);
        set.sequences.push_back(loadOperations(path, ThreeFields::kFieldCount));
        set.totalOps += static_cast<long long>(set.sequences.back().size());
    }
    return set;
}

vector<TestSet> loadScenario(const fs::path& dir, char scenario)
{
    vector<TestSet> sets;
    for (int threads = 1; threads <= kMaxThreads; ++threads)
    {
        sets.push_back(loadTestSet(dir, scenario, threads));
        const long long ops = sets.back().totalOps;
        const long long expected = sets.front().totalOps;
        if (ops == 0)
            throw runtime_error("scenario " + string(1, scenario) + ": files for " + to_string(threads) +
                                " thread(s) contain no operations");
        if (ops != expected)
            throw runtime_error("scenario " + string(1, scenario) + ": files for " + to_string(threads) +
                                " threads contain " + to_string(ops) + " operations in total, but the file for 1 thread contains " +
                                to_string(expected) + "; the total must be the same");
    }
    return sets;
}

template <class Structure>
long long execute(Structure& data, const OpSequence& ops)
{
    long long checksum = 0;
    for (const Operation& op : ops)
    {
        switch (op.type)
        {
        case OpType::Read:
            checksum += data.get(op.field);
            break;
        case OpType::Write:
            data.set(op.field, op.value);
            break;
        case OpType::String:
            checksum += static_cast<long long>(static_cast<string>(data).size());
            break;
        }
    }
    return checksum;
}

struct RunResult
{
    double totalMs = 0;
    vector<double> perThreadMs;
    long long checksum = 0;
};

template <class Structure>
RunResult runInThreads(Structure& data, const vector<OpSequence>& sequences)
{
    using Clock = chrono::steady_clock;
    const size_t n = sequences.size();

    mutex gateMutex;
    condition_variable gateCv;
    size_t ready = 0;
    bool started = false;

    vector<long long> checksums(n, 0);
    vector<Clock::time_point> finishes(n);
    vector<exception_ptr> errors(n);

    auto openGate = [&] {
        {
            lock_guard lock(gateMutex);
            started = true;
        }
        gateCv.notify_all();
    };

    vector<thread> threads;
    threads.reserve(n);
    try
    {
        for (size_t i = 0; i < n; ++i)
        {
            threads.emplace_back([&, i] {
                {
                    unique_lock lock(gateMutex);
                    ++ready;
                    gateCv.notify_all();
                    gateCv.wait(lock, [&] { return started; });
                }
                try
                {
                    checksums[i] = execute(data, sequences[i]);
                }
                catch (...)
                {
                    errors[i] = current_exception();
                }
                finishes[i] = Clock::now();
            });
        }
    }
    catch (...)
    {
        openGate();
        for (thread& t : threads) t.join();
        throw;
    }

    {
        unique_lock lock(gateMutex);
        gateCv.wait(lock, [&] { return ready == n; });
    }
    const Clock::time_point start = Clock::now();
    openGate();
    for (thread& t : threads) t.join();
    const Clock::time_point finish = Clock::now();

    for (const exception_ptr& e : errors)
        if (e) rethrow_exception(e);

    RunResult result;
    result.totalMs = chrono::duration<double, milli>(finish - start).count();
    for (size_t i = 0; i < n; ++i)
    {
        result.perThreadMs.push_back(chrono::duration<double, milli>(finishes[i] - start).count());
        result.checksum += checksums[i];
    }
    return result;
}

struct Measurement
{
    double avgMs = 0, minMs = 0, maxMs = 0;
    vector<double> perThreadAvgMs;
    long long checksum = 0;
};

template <class Structure>
Measurement measure(const vector<OpSequence>& sequences, int repeats)
{
    {
        Structure warmUpData;
        runInThreads(warmUpData, sequences);
    }
    Measurement m;
    m.perThreadAvgMs.assign(sequences.size(), 0.0);
    for (int r = 0; r < repeats; ++r)
    {
        Structure data;
        const RunResult run = runInThreads(data, sequences);
        m.avgMs += run.totalMs;
        m.minMs = r == 0 ? run.totalMs : min(m.minMs, run.totalMs);
        m.maxMs = r == 0 ? run.totalMs : max(m.maxMs, run.totalMs);
        for (size_t i = 0; i < sequences.size(); ++i) m.perThreadAvgMs[i] += run.perThreadMs[i];
        m.checksum += run.checksum;
    }
    m.avgMs /= repeats;
    for (double& t : m.perThreadAvgMs) t /= repeats;
    return m;
}

void checkField(int field)
{
    if (field < 0 || field >= 3) throw out_of_range("field index " + to_string(field) + " is out of range [0, 3)");
}

class TwoMutexes
{
public:
    int get(int field) const
    {
        checkField(field);
        if (field == 1)
        {
            lock_guard lock(mutex1_);
            return v1_;
        }
        lock_guard lock(mutex02_);
        return field == 0 ? v0_ : v2_;
    }
    void set(int field, int value)
    {
        checkField(field);
        if (field == 1)
        {
            lock_guard lock(mutex1_);
            v1_ = value;
            return;
        }
        lock_guard lock(mutex02_);
        (field == 0 ? v0_ : v2_) = value;
    }
    explicit operator string() const
    {
        int v0, v1, v2;
        {
            lock_guard lock02(mutex02_);
            lock_guard lock1(mutex1_);
            v0 = v0_;
            v1 = v1_;
            v2 = v2_;
        }
        return formatFields(v0, v1, v2);
    }

private:
    mutable mutex mutex02_;
    int v0_ = 0, v2_ = 0;
    mutable mutex mutex1_;
    int v1_ = 0;
};

class MutexPerField
{
public:
    int get(int field) const
    {
        checkField(field);
        lock_guard lock(mutexes_[field]);
        return fields_[field];
    }
    void set(int field, int value)
    {
        checkField(field);
        lock_guard lock(mutexes_[field]);
        fields_[field] = value;
    }
    explicit operator string() const
    {
        int v0, v1, v2;
        {
            lock_guard l0(mutexes_[0]), l1(mutexes_[1]), l2(mutexes_[2]);
            v0 = fields_[0];
            v1 = fields_[1];
            v2 = fields_[2];
        }
        return formatFields(v0, v1, v2);
    }

private:
    mutable mutex mutexes_[3];
    int fields_[3] = {};
};

void warmUp(shared_mutex& m)
{
    m.lock();
    m.unlock();
    m.lock_shared();
    m.unlock_shared();
}

class OneSharedMutex
{
public:
    OneSharedMutex() { warmUp(mutex_); }
    int get(int field) const
    {
        checkField(field);
        shared_lock lock(mutex_);
        return fields_[field];
    }
    void set(int field, int value)
    {
        checkField(field);
        unique_lock lock(mutex_);
        fields_[field] = value;
    }
    explicit operator string() const
    {
        int v0, v1, v2;
        {
            shared_lock lock(mutex_);
            v0 = fields_[0];
            v1 = fields_[1];
            v2 = fields_[2];
        }
        return formatFields(v0, v1, v2);
    }

private:
    mutable shared_mutex mutex_;
    int fields_[3] = {};
};

class SharedMutexPerField
{
public:
    SharedMutexPerField()
    {
        for (shared_mutex& m : mutexes_) warmUp(m);
    }
    int get(int field) const
    {
        checkField(field);
        shared_lock lock(mutexes_[field]);
        return fields_[field];
    }
    void set(int field, int value)
    {
        checkField(field);
        unique_lock lock(mutexes_[field]);
        fields_[field] = value;
    }
    explicit operator string() const
    {
        int v0, v1, v2;
        {
            shared_lock l0(mutexes_[0]), l1(mutexes_[1]), l2(mutexes_[2]);
            v0 = fields_[0];
            v1 = fields_[1];
            v2 = fields_[2];
        }
        return formatFields(v0, v1, v2);
    }

private:
    mutable shared_mutex mutexes_[3];
    int fields_[3] = {};
};

template <class Structure>
double averageMs(const vector<OpSequence>& sequences, int repeats)
{
    return measure<Structure>(sequences, repeats).avgMs;
}

template <class Structure>
double averageMsReused(const vector<OpSequence>& sequences, int repeats)
{
    static Structure data;
    runInThreads(data, sequences);
    double sum = 0;
    for (int r = 0; r < repeats; ++r) sum += runInThreads(data, sequences).totalMs;
    return sum / repeats;
}

int compareMain(int argc, char* argv[])
{
    try
    {
        const fs::path dir = argc > 1 ? directoryFromArg(argv[1]) : fs::path("tests");
        const int repeats = argc > 2 ? parseRepeats(argv[2]) : 5;

        cout << "Program project 4, variant 5: comparison of protection schemes\n"
             << "Average time, ms (" << repeats << " runs each), on the same test files as benchmark\n"
             << fixed << setprecision(2);

        for (int s = 0; s < kScenarioCount; ++s)
        {
            const Scenario& sc = kScenarios[s];
            const vector<TestSet> sets = loadScenario(dir, sc.id);

            cout << "\n" << sc.id << ") " << sc.title << "\n"
                 << left << setw(46) << "  scheme" << right << setw(10) << "1 thread" << setw(12) << "2 threads"
                 << setw(12) << "3 threads" << "\n";
            auto row = [&](const char* name, auto averageFn) {
                cout << left << setw(46) << name << right;
                for (int t = 0; t < kMaxThreads; ++t) cout << setw(t == 0 ? 10 : 12) << averageFn(sets[t].sequences, repeats);
                cout << "\n";
            };
            row("  1 std::mutex (chosen, ThreeFields)", averageMs<ThreeFields>);
            row("  2 std::mutex: {field 1}, {fields 0, 2}", averageMs<TwoMutexes>);
            row("  3 std::mutex, one per field", averageMs<MutexPerField>);
            row("  1 std::shared_mutex", averageMsReused<OneSharedMutex>);
            row("  3 std::shared_mutex, one per field", averageMsReused<SharedMutexPerField>);
        }
    }
    catch (const exception& e)
    {
        cerr << "Error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

void printHeader(const fs::path& dir, int repeats)
{
    cout << "Program project 4, variant 5 (m = 3)\n"
         << "Frequencies of operations, %: read0 9, write0 1, read1 40, write1 10, read2 10, write2 10, string 20\n"
         << "Protection scheme: one std::mutex for all three fields (string copies the fields under it\n"
         << "                   and formats the result after unlocking)\n"
#ifdef NDEBUG
         << "Build: optimized (NDEBUG defined)\n"
#else
         << "Build: WARNING - debug build (NDEBUG not defined), times are not representative\n"
#endif
         << "Hardware threads: " << thread::hardware_concurrency() << "\n"
         << "Test directory: " << dir.string() << "\n"
         << "Each case: 1 warm-up run + " << repeats << " measured runs, a new structure for every run;\n"
         << "time is measured from the common start of all threads to the end of the last one\n"
         << "(reading files and creating threads are not included).\n";
}

int benchmarkMain(int argc, char* argv[])
{
    try
    {
        const fs::path dir = argc > 1 ? directoryFromArg(argv[1]) : fs::path("tests");
        const int repeats = argc > 2 ? parseRepeats(argv[2]) : 10;
        printHeader(dir, repeats);

        vector<vector<TestSet>> sets;
        long long totalOps = 0;
        for (int s = 0; s < kScenarioCount; ++s)
        {
            sets.push_back(loadScenario(dir, kScenarios[s].id));
            for (const TestSet& set : sets.back()) totalOps += set.totalOps;
        }
        cout << "Loaded " << kScenarioCount * (kMaxThreads * (kMaxThreads + 1) / 2) << " files, "
             << totalOps << " operations in total.\n";

        double table[kScenarioCount][kMaxThreads] = {};
        long long control = 0;
        cout << fixed << setprecision(2);
        for (int s = 0; s < kScenarioCount; ++s)
        {
            const Scenario& sc = kScenarios[s];
            cout << "\n" << sc.id << ") " << sc.title << "\n";
            for (int threads = 1; threads <= kMaxThreads; ++threads)
            {
                const TestSet& set = sets[s][threads - 1];
                const Measurement m = measure<ThreeFields>(set.sequences, repeats);
                table[s][threads - 1] = m.avgMs;
                control += m.checksum;

                cout << "  " << threads << (threads == 1 ? " thread,  " : " threads, ") << set.totalOps
                     << " ops: avg " << setw(7) << m.avgMs << " ms  (min " << m.minMs << ", max " << m.maxMs << ")\n";
                for (int i = 0; i < threads; ++i)
                    cout << "      " << set.fileNames[i] << ": " << set.sequences[i].size()
                         << " ops, thread finished after avg " << setw(7) << m.perThreadAvgMs[i] << " ms\n";
            }
        }

        cout << "\nAverage time, ms (" << repeats << " runs each)\n"
             << left << setw(36) << "test files" << right << setw(10) << "1 thread" << setw(12) << "2 threads"
             << setw(12) << "3 threads" << "\n";
        for (int s = 0; s < kScenarioCount; ++s)
        {
            cout << left << setw(36) << (string(1, kScenarios[s].id) + ") " + kScenarios[s].title) << right;
            cout << setw(10) << table[s][0] << setw(12) << table[s][1] << setw(12) << table[s][2] << "\n";
        }
        cout << "\n(control value, sum of all reads and string lengths: " << control << ")\n";
    }
    catch (const exception& e)
    {
        cerr << "Error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc > 1 && string(argv[1]) == "--compare") return compareMain(argc - 1, argv + 1);
    return benchmarkMain(argc, argv);
}
