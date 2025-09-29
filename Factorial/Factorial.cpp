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
namespace fs = filesystem;

unsigned int MAX_THREADS = max(2u, thread::hardware_concurrency() ? thread::hardware_concurrency() : 2u); 

const uint64_t MIN_RANGE_FOR_ASYNC = 1024;
const uint64_t SAVE_MIN_RANGE = 8192;
const string CHECKPOINT_DIR = "checkpoints";     

mutex mu;
condition_variable thr_cv; //для ожидания потоков      
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
    return n <= cpp_int(numeric_limits<uint64_t>::max()); 
}

bool load_checkpoint(uint64_t l, uint64_t r, cpp_int& out) {
    fs::path dir(CHECKPOINT_DIR);
    fs::path file = dir / ("prod_" + to_string(l) + "_" + to_string(r) + ".bin");
    if (!fs::exists(file)) return false;
    ifstream ifs(file, ios::binary); 
    if (!ifs) {
        return false;
    }
    vector<unsigned char> data((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
    ifs.close();
    if (data.empty()) return false;
    boost::multiprecision::import_bits(out, data.begin(), data.end(), 8, false);
    return true;
}

void save_checkpoint(uint64_t l, uint64_t r, const cpp_int& val) {
    fs::path dir(CHECKPOINT_DIR);
    try {
        fs::create_directories(dir);
    }
    catch (...) {                   
        return;
    }
    fs::path file = dir / ("prod_" + to_string(l) + "_" + to_string(r) + ".bin");
    vector<unsigned char> data;
    boost::multiprecision::export_bits(val, back_inserter(data), 8, false);
    ofstream ofs(file, ios::binary);
    if (!ofs) return;

    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<streamsize>(data.size())); 
    ofs.close();
}

cpp_int multiply(uint64_t l, uint64_t r) {
    cpp_int res = 1;
    for (uint64_t i = l; i <= r; ++i) {
        res *= cpp_int(i);
    }
    return res;
}

//Если есть слот для асинхронного запуска — запускаем async и увеличиваем active_threads.
//Если слота нет — выполняем f синхронно в текущем потоке
template<typename F>
future<cpp_int> launch_task_with_limit(F f) {
    unique_lock<mutex> lk(mu); 
    if (active_threads < MAX_THREADS) {
        ++active_threads;

        lk.unlock();
        return async(launch::async, [f]() -> cpp_int { //лямбда
            cpp_int result;
            try {
                result = f();
            }
            catch (...) {
                {
                    lock_guard<mutex> g(mu);
                    if (active_threads > 0) {
                        --active_threads;
                    }
                }
                thr_cv.notify_one();
                throw; 
            }
            
            lock_guard<mutex> g(mu);
            if (active_threads > 0) {
                --active_threads;
            }
            
            thr_cv.notify_one();
            return result;
            });
     }
    else {
        lk.unlock();
        promise<cpp_int> prom;  
        try {
            cpp_int val = f();
            prom.set_value(val);
        }
        catch (...) {
           prom.set_exception(current_exception());
        }
        return prom.get_future();
    }
}

cpp_int product_tree_mt(uint64_t l, uint64_t r) {
    if (l > r) return cpp_int(1);

    if ((r - l + 1) >= SAVE_MIN_RANGE) {
        cpp_int cached;
        if (load_checkpoint(l, r, cached)) {
            return cached;
        }
    }

    if (r - l <= SEQ_THRESHOLD) {
        cpp_int res = multiply(l, r);
        if ((r - l + 1) >= SAVE_MIN_RANGE) {
            save_checkpoint(l, r, res);
        }
        return res;
    }

    uint64_t m = l + (r - l) / 2;
    cpp_int left, right;

    if ((r - l + 1) >= MIN_RANGE_FOR_ASYNC) {
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
    }

    return res;
}

//число нулей равно количеству множителей 5 в разложении n!
cpp_int trailing_zeros(cpp_int n) {
    cpp_int cnt = 0;
    cpp_int p = 5;
    while (n >= p) {
        cnt += n / p;
        p *= 5;
    }
    return cnt;
}

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

        if (!is_digits(s)) {
            cout << "Допустимы только цифры\n";
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
