#include <iostream>
#include <string>
#include <cmath>
#include <limits>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>

using namespace std;
using boost::multiprecision::cpp_int;
using boost::multiprecision::cpp_dec_float_50;

const uint64_t MAX_EXACT = 20000;
const uint64_t SEQ_THRESHOLD = 64;

const int j;

//строка содержит только цифры
bool is_digits(const string& s) {
    if (s.empty()) return false;
    for (char c : s){
        if (c < '0' || c > '9'){ 
            return false; 
        }
    }
    return true;
}

//парсинг строки в cpp_int
cpp_int parse_cpp_int(const string& s) {
    cpp_int x = 0;
    for (char c : s){ 
        x *= 10; 
        x += cpp_int(c - '0');
    }
    return x;
}

//проверка, помещается ли n в uint64_t
bool fits_uint64(const cpp_int& n) {
    return n <= cpp_int(std::numeric_limits<uint64_t>::max());     //ПОЧЕМУ ТАК
}

//просто произведение
cpp_int multiply(uint64_t l, uint64_t r) {
    cpp_int res = 1;
        for (uint64_t i = l; i <= r; ++i) {
            res *= cpp_int(i);
        }
    return res;
}

//разделение пополам, перемножение и рекурсия 
cpp_int half_multiply(uint64_t l, uint64_t r) {
    if (l > r) return cpp_int(1);
    if (l == r) return cpp_int(l);
    if (r - l <= SEQ_THRESHOLD) return multiply(l, r);
    uint64_t m = l + (r - l) / 2; //для избежания переполнения
    cpp_int left = half_multiply(l, m);
    cpp_int right = half_multiply(m + 1, r);
    return left * right;
}

//число завершающих нулей: sum_{k>=1} floor(n / 5^k)
//завершающие нули в десятичной записи числа образуются парой множителей 2*5. В факториале количество двоек обычно больше, 
// чем чисел-пятёрок, поэтому      !!!число нулей равно количеству множителей 5 в разложении n!.!!!
//n = 100:
//floor(100 / 5) = 20, floor(100 / 25) = 4, floor(100 / 125) = 0 → суммарно 24.
//Значит 100!заканчивается 24 нулями.
cpp_int trailing_zeros(cpp_int n) {
    cpp_int cnt = 0;
    cpp_int p = 5;
    while (n >= p) {
        cnt += n / p;
        p *= 5;
    }
    return cnt;
}

//приближённая информация по Стирлингу: количество цифр и первые K цифр
void stirling(const string& s, int K = 10) {
    //используем первые 18 значимых цифр как мантиссу - значимую часть числа для представления через степень 10-ки
    //18 берем, чтобы не терять точность с учетом 50-значных чисел
    int L = (int)s.size();
    int Kmant = min(18, L);
    string pref = s.substr(0, Kmant); //берем первые Kmant символов и кладем в pref
    cpp_dec_float_50 pref_val = 0;
    for (char c : pref){
        pref_val = pref_val * 10 + cpp_dec_float_50(c - '0');
    }
    cpp_dec_float_50  mant = pref_val / pow(10, Kmant - 1);//10^Kmant-1

    cpp_dec_float_50 n_dec = mant * pow(cpp_dec_float_50(10), L - 1);

    cpp_dec_float_50 log10_n = log10(n_dec); //log10(n)
    cpp_dec_float_50 pi = acos(cpp_dec_float_50(-1));

    cpp_dec_float_50 log10_2pi = log10(cpp_dec_float_50(2) * pi);
    cpp_dec_float_50 log10_e = log10(exp(cpp_dec_float_50(1)));

    cpp_dec_float_50 A = cpp_dec_float_50(0.5) * (log10_2pi + log10_n) + n_dec * (log10_n - log10_e); //log10(n!)
    cpp_dec_float_50 digits_ld = floor(A) + 1;//кол-во десятичных цифр числа А

    //выводим количество цифр (приближённо)
    cout << "Приближённое кол-во цифр в n!: " << scientific << setprecision(6) << digits_ld << " (в науч. форме)\n";

    //первые K цифр: берем дробную часть frac = A - floor(A)
    cpp_dec_float_50 frac = A - floor(A);
    cpp_dec_float_50 lead_val = pow(cpp_dec_float_50(10), frac + (K - 1));

    // округлим и выведем
    unsigned long long leading = (unsigned long long)(lead_val + 0.5);
    string lead_str = to_string(leading);
    if ((int)lead_str.size() < K) {
        lead_str = string(K - lead_str.size(), '0') + lead_str;
    }
    cout << "Первые " << K << " цифр n! (приближённо): " << lead_str << "\n";
}

int main() {
    setlocale(0, "RUSSIAN");
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << "Если n > " << MAX_EXACT << ", выдам аналитические данные (кол-во завершающих нулей, кол-во цифр, первые 10 цифр).\n"
        << "Введите q для выхода.\n";

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
                //cout << "Вычисляю точно n! для n = " << n << " ...\n";
                cpp_int res = half_multiply(1, n);
                cout << res << "\n";
                continue;
            }
        }

        // для больших n:
        cpp_int zeros = trailing_zeros(n_big);
        cout << "Кол-во завершающих нулей: " << zeros << "\n";

        // оценка Стирлинга
        stirling(s, 10);
    }
    cout << "Выход.\n";
    return 0;
}
