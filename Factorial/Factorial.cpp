#include <iostream>
#include <string>
#include <cmath>
#include <limits>
#include <vector>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <future>
#include <thread>
#include <filesystem>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/integer.hpp>

using namespace std;
using boost::multiprecision::cpp_int;
using boost::multiprecision::cpp_dec_float_50;

const uint64_t MAX_EXACT = 50000;
const uint64_t SEQ_THRESHOLD = 64;

// параллелизм / чекпоинты — настраиваемые параметры
unsigned int MAX_THREADS = std::max(2u, std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2u);
// минимальная длина диапазона для запуска задачи асинхронно
const uint64_t MIN_RANGE_FOR_TASK = 1024;
// минимальная длина диапазона для сохранения чекпоинта
const uint64_t SAVE_MIN_RANGE = 8192;
// директория чекпоинтов
const std::string CHECKPOINT_DIR = "checkpoints";

// отладочные логи (false для обычного запуска)
const bool VERBOSE = false;

// глобальная синхронизация для ограничения числа активных задач
std::mutex thr_mu;
std::condition_variable thr_cv;
unsigned int active_threads = 0;

bool is_digits(const string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

cpp_int parse_cpp_int(const string& s) {
    cpp_int x = 0;
    for (char c : s) {
        x *= 10;
        x += cpp_int(c - '0');
    }
    return x;
}

bool fits_uint64(const cpp_int& n) {
    return n <= cpp_int(std::numeric_limits<uint64_t>::max());
}

// сериализация / десериализация
bool load_checkpoint(uint64_t l, uint64_t r, cpp_int& out) {
    namespace fs = std::filesystem;
    fs::path dir(CHECKPOINT_DIR);
    fs::path file = dir / ("prod_" + to_string(l) + "_" + to_string(r) + ".bin");
    if (!fs::exists(file)) return false;
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    if (data.empty()) return false;
    boost::multiprecision::import_bits(out, data.begin(), data.end(), 8, false);
    return true;
}

void save_checkpoint(uint64_t l, uint64_t r, const cpp_int& val) {
    namespace fs = std::filesystem;
    fs::path dir(CHECKPOINT_DIR);
    try {
        fs::create_directories(dir);
    }
    catch (...) {
        return;
    }
    fs::path file = dir / ("prod_" + to_string(l) + "_" + to_string(r) + ".bin");
    std::vector<unsigned char> data;
    boost::multiprecision::export_bits(val, std::back_inserter(data), 8, false);
    std::ofstream ofs(file, std::ios::binary);
    if (!ofs) return;
    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    ofs.close();
}

cpp_int multiply(uint64_t l, uint64_t r) {
    cpp_int res = 1;
    for (uint64_t i = l; i <= r; ++i) {
        res *= cpp_int(i);
    }
    return res;
}

// ---------- Управление задачами с ограничением parallelism ----------
// Запуск: если есть слот для асинхронного запуска — запускаем std::async и увеличиваем active_threads.
// Если слота нет — выполняем f синхронно в текущем потоке (чтобы избежать дедлока).
// Гарантируем корректное уменьшение active_threads при завершении асинхронной задачи.
template<typename F>
std::future<cpp_int> launch_task_with_limit(F f) {
    std::unique_lock<std::mutex> lk(thr_mu);
    if (active_threads < MAX_THREADS) {
        ++active_threads;
        if (VERBOSE) {
            std::cerr << "[launch] starting async task, active_threads=" << active_threads << "\n";
        }
        lk.unlock();

        // Запускаем асинхронно и гарантируем уменьшение active_threads при завершении
        return std::async(std::launch::async, [f]() -> cpp_int {
            cpp_int result;
            try {
                result = f();
            }
            catch (...) {
                // при исключении нужно корректно уменьшить счётчик и пробудить ожидающих
                {
                    std::lock_guard<std::mutex> g(thr_mu);
                    if (active_threads > 0) --active_threads;
                }
                thr_cv.notify_one();
                throw; // пробросим исключение дальше
            }
            {
                std::lock_guard<std::mutex> g(thr_mu);
                if (active_threads > 0) --active_threads;
            }
            thr_cv.notify_one();
            if (VERBOSE) {
                std::cerr << "[launch] async task finished, active_threads=" << active_threads << "\n";
            }
            return result;
            });
    }
    else {
        // Нет свободных слотов — выполняем inline, чтобы не блокировать рабочие потоки и избежать дедлока.
        if (VERBOSE) {
            std::cerr << "[launch] no slot available, executing inline in current thread\n";
        }
        lk.unlock();
        std::promise<cpp_int> prom;
        try {
            cpp_int val = f();
            prom.set_value(val);
        }
        catch (...) {
            try { prom.set_exception(std::current_exception()); }
            catch (...) {}
        }
        return prom.get_future();
    }
}

// ---------- Product tree (mt) ----------
cpp_int product_tree_mt(uint64_t l, uint64_t r) {
    if (l > r) return cpp_int(1);

    // пробуем загрузить чекпоинт для больших диапазонов
    if ((r - l + 1) >= SAVE_MIN_RANGE) {
        cpp_int cached;
        if (load_checkpoint(l, r, cached)) {
            if (VERBOSE) std::cerr << "[checkpoint] loaded " << l << "-" << r << "\n";
            return cached;
        }
    }

    if (r - l <= SEQ_THRESHOLD) {
        cpp_int res = multiply(l, r);
        if ((r - l + 1) >= SAVE_MIN_RANGE) {
            save_checkpoint(l, r, res);
            if (VERBOSE) std::cerr << "[checkpoint] saved leaf " << l << "-" << r << "\n";
        }
        return res;
    }

    uint64_t m = l + (r - l) / 2;
    cpp_int left, right;

    if ((r - l + 1) >= MIN_RANGE_FOR_TASK) {
        // запускаем левую ветку как задачу с ограничением, правую в текущем потоке
        auto fut = launch_task_with_limit([l, m]() {
            return product_tree_mt(l, m);
            });
        right = product_tree_mt(m + 1, r);
        left = fut.get();
    }
    else {
        left = product_tree_mt(l, m);
        right = product_tree_mt(m + 1, r);
    }

    cpp_int res = left * right;

    if ((r - l + 1) >= SAVE_MIN_RANGE) {
        save_checkpoint(l, r, res);
        if (VERBOSE) std::cerr << "[checkpoint] saved " << l << "-" << r << "\n";
    }

    return res;
}

// ---------- trailing zeros ----------
cpp_int trailing_zeros(cpp_int n) {
    cpp_int cnt = 0;
    cpp_int p = 5;
    while (n >= p) {
        cnt += n / p;
        p *= 5;
    }
    return cnt;
}

// ---------- Stirling estimate (использует cpp_dec_float_50 для точности) ----------
void stirling(const string& s, int K = 10) {
    int L = (int)s.size();
    int Kmant = min(18, L);
    string pref = s.substr(0, Kmant);
    cpp_dec_float_50 pref_val = 0;
    for (char c : pref) pref_val = pref_val * 10 + cpp_dec_float_50(c - '0');
    cpp_dec_float_50 mant = pref_val / pow(cpp_dec_float_50(10), Kmant - 1);
    cpp_dec_float_50 n_dec = mant * pow(cpp_dec_float_50(10), L - 1);

    cpp_dec_float_50 log10_n = log10(n_dec);
    cpp_dec_float_50 pi = acos(cpp_dec_float_50(-1));
    cpp_dec_float_50 log10_2pi = log10(cpp_dec_float_50(2) * pi);
    cpp_dec_float_50 log10_e = log10(exp(cpp_dec_float_50(1)));

    cpp_dec_float_50 A = cpp_dec_float_50(0.5) * (log10_2pi + log10_n) + n_dec * (log10_n - log10_e);
    cpp_dec_float_50 digits_ld = floor(A) + 1;

    cout << "Приближённое кол-во цифр в n!: " << scientific << setprecision(6) << digits_ld << " (в науч. форме)\n";

    cpp_dec_float_50 frac = A - floor(A);
    cpp_dec_float_50 lead_val = pow(cpp_dec_float_50(10), frac + (K - 1));
    unsigned long long leading = (unsigned long long)(lead_val + 0.5);
    string lead_str = to_string(leading);
    if ((int)lead_str.size() < K) lead_str = string(K - lead_str.size(), '0') + lead_str;
    cout << "Первые " << K << " цифр n! (приближённо): " << lead_str << "\n";
}

// ---------- main ----------
int main() {
    setlocale(LC_ALL, "Russian");
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << "Если n > " << MAX_EXACT << ", выдам аналитические данные (нулей, кол-во цифр, первые 10 цифр).\n";
    cout << "Введите q для выхода.\n";

    while (true) {
        cout << "\nВведите n (или q): ";
        string s;
        if (!(cin >> s)) break;
        if (s == "q" || s == "Q") break;

        if (s.size() > 50) {
            cout << "Ошибка: длина больше 50 цифр\n";
            continue;
        }
        if (!is_digits(s)) {
            cout << "Ошибка: допустимы только цифры\n";
            continue;
        }
        if (s == "0" || s == "1") {
            cout << "1\n";
            continue;
        }

        cpp_int n_big = parse_cpp_int(s);

        if (fits_uint64(n_big)) {
            uint64_t n = n_big.convert_to<uint64_t>();
            if (n <= MAX_EXACT) {
                // Можно отключить параллельность установкой MAX_THREADS = 1 перед запуском
                cpp_int res = product_tree_mt(1, n);
                cout << res << "\n";
                continue;
            }
        }

        cpp_int zeros = trailing_zeros(n_big);
        cout << "Кол-во завершающих нулей: " << zeros << "\n";
        stirling(s, 10);
    }

    cout << "Выход.\n";
    return 0;
}
