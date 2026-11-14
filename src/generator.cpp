#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

constexpr int kKindCount = 7;
constexpr const char* kKindNames[kKindCount] = {
    "read 0", "write 0", "read 1", "write 1", "read 2", "write 2", "string"};

struct Scenario
{
    char id;
    const char* title;
    int weights[kKindCount];
};

constexpr Scenario kScenarios[] = {
    {'a', "frequencies of variant 5", {9, 1, 40, 10, 10, 10, 20}},
    {'b', "all frequencies equal", {1, 1, 1, 1, 1, 1, 1}},
    {'c', "far from variant, not equal", {1, 40, 2, 5, 2, 45, 5}},
};
constexpr int kScenarioCount = 3;

constexpr int kMaxThreads = 3;
constexpr long long kDefaultTotalOps = 1'200'000;
constexpr long long kMaxTotalOps = 120'000'000;

std::string testFileName(char scenario, int threads, int index)
{
    return std::string(1, scenario) + "_t" + std::to_string(threads) + "_" + std::to_string(index) + ".txt";
}

using namespace std;
namespace fs = std::filesystem;

namespace {

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

long long parsePositive(const char* text, const char* what)
{
    const string s = text;
    long long value = 0;
    const auto [ptr, ec] = from_chars(s.data(), s.data() + s.size(), value);
    if (ec != errc() || ptr != s.data() + s.size() || value <= 0)
        throw invalid_argument(string(what) + " must be a positive integer, got \"" + s + "\"");
    return value;
}

vector<long long> kindCounts(const Scenario& sc, long long n)
{
    long long weightSum = 0;
    for (int w : sc.weights) weightSum += w;

    vector<long long> counts(kKindCount);
    vector<long long> remainders(kKindCount);
    long long assigned = 0;
    for (int k = 0; k < kKindCount; ++k)
    {
        counts[k] = n * sc.weights[k] / weightSum;
        remainders[k] = n * sc.weights[k] % weightSum;
        assigned += counts[k];
    }
    vector<int> order(kKindCount);
    for (int k = 0; k < kKindCount; ++k) order[k] = k;
    stable_sort(order.begin(), order.end(), [&](int x, int y) { return remainders[x] > remainders[y]; });
    for (int i = 0; assigned < n; ++i, ++assigned) ++counts[order[i]];
    return counts;
}

string generateFile(const Scenario& sc, long long n, uint64_t seed)
{
    const vector<long long> counts = kindCounts(sc, n);
    vector<uint8_t> kinds;
    kinds.reserve(static_cast<size_t>(n));
    for (int k = 0; k < kKindCount; ++k) kinds.insert(kinds.end(), static_cast<size_t>(counts[k]), static_cast<uint8_t>(k));

    mt19937_64 rng(seed);
    shuffle(kinds.begin(), kinds.end(), rng);
    uniform_int_distribution<int> valueDist(0, 99);

    string text;
    text.reserve(static_cast<size_t>(n) * 11);
    for (uint8_t kind : kinds)
    {
        if (kind == kKindCount - 1)
        {
            text += "string\n";
            continue;
        }
        const char field = static_cast<char>('0' + kind / 2);
        if (kind % 2 == 0)
        {
            text += "read ";
            text += field;
        }
        else
        {
            text += "write ";
            text += field;
            text += ' ';
            text += to_string(valueDist(rng));
        }
        text += '\n';
    }
    return text;
}

void writeFile(const fs::path& path, const string& text)
{
    ofstream out(path, ios::binary);
    if (!out) throw runtime_error("cannot create file " + path.string());
    out.write(text.data(), static_cast<streamsize>(text.size()));
    out.close();
    if (!out) throw runtime_error("cannot write file " + path.string());
}

}

int main(int argc, char* argv[])
{
    try
    {
        const fs::path dir = argc > 1 ? directoryFromArg(argv[1]) : fs::path("tests");
        const long long total = argc > 2 ? parsePositive(argv[2], "number of operations") : kDefaultTotalOps;
        const uint64_t seed = argc > 3 ? static_cast<uint64_t>(parsePositive(argv[3], "seed")) : 2026;
        if (total % 6 != 0)
            throw invalid_argument("number of operations must be divisible by 1, 2 and 3 (that is, by 6)");
        if (total > kMaxTotalOps)
            throw invalid_argument("number of operations must not exceed " + to_string(kMaxTotalOps));

        fs::create_directories(dir);
        cout << "Program project 4, variant 5: test file generator\n"
             << "Directory: " << dir.string() << "\n"
             << "Operations per case (1, 2 or 3 threads): " << total << ", seed: " << seed << "\n";

        for (int s = 0; s < kScenarioCount; ++s)
        {
            const Scenario& sc = kScenarios[s];
            const vector<long long> share = kindCounts(sc, 10000);
            cout << "\n" << sc.id << ") " << sc.title << ":";
            for (int k = 0; k < kKindCount; ++k)
                cout << (k ? ", " : " ") << kKindNames[k] << " " << fixed << setprecision(2) << static_cast<double>(share[k]) / 100.0 << "%";
            cout << "\n";

            for (int threads = 1; threads <= kMaxThreads; ++threads)
            {
                const long long perThread = total / threads;
                cout << "  " << threads << (threads == 1 ? " thread: " : " threads:");
                for (int i = 0; i < threads; ++i)
                {
                    const uint64_t fileSeed = seed * 1000 + static_cast<uint64_t>(s * 100 + threads * 10 + i);
                    const string name = testFileName(sc.id, threads, i);
                    writeFile(dir / name, generateFile(sc, perThread, fileSeed));
                    cout << " " << name << " (" << perThread << " ops)";
                }
                cout << "\n";
            }
        }
        cout << "\nDone.\n";
    }
    catch (const exception& e)
    {
        cerr << "Error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
