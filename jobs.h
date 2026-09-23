#pragma once
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <iostream>
#include <atomic>

#if defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

/*!
 * @brief Простая обёртка над неинициализированным значением, позволяющая отложено вызвать
 * конструктор и разместить значение.
 * @tparam T - тип значения
 * @details Так как переменная считается доступной как только ввели её имя, будем использовать
 * этот тип для доступа к ещё не инициализированной переменной в конструкциях вида:
 *
 *      uninit<std::string>&& do_init_variable(uninit<std::string>& var) {
 *          var.emplace("text");
 *          return std::move(var);
 *      }
 *      ...
 *      uninit<std::string> var = do_init_variable(var);
 *      some_func(var->size());
 *      use(*var);
 */
template<typename T>
struct uninit {
    uninit() = default;
    uninit(uninit&& o) noexcept {
        // Так как этот тип предполагается использовать только для передачи неинициализированных
        // переменных в левую часть присваивания/инициализации самому себе же, сделаем конструктор
        // перемещения, в котором ничего не делаем.
        // Только проверим, что вызываемся сами для себя.
        assert(&o == this);
    }
    ~uninit() {
        reinterpret_cast<T*>(value_)->~T();
    }
    // Не копируемый, не присваиваемый.
    uninit(const uninit& o) = delete;
    uninit& operator=(const uninit&) = delete;

    // Конструирование значения
    template<typename...Args> requires std::is_constructible_v<T, Args...>
    T& emplace(Args&&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        std::cout << "Emplace uninit at " << this << " in " << __PRETTY_FUNCTION__ << "\n";
        new (value_) T (std::forward<Args>(args)...);
        return *reinterpret_cast<T*>(value_);
    }

    T& operator*() {
        return *reinterpret_cast<T*>(value_);
    }
    const T& operator*() const {
        return *reinterpret_cast<const T*>(value_);
    }
    T* operator->() {
        return reinterpret_cast<T*>(value_);
    }
    const T* operator->() const {
        return reinterpret_cast<const T*>(value_);
    }
private:
    alignas(T) char value_[sizeof(T)];
};

// Выясним, в каком виде промежуточно хранить возвращаемое корутиной значение.
template<typename T>
using job_ret_type_t = uninit<std::conditional_t<std::is_reference_v<T>, std::remove_cvref_t<T>*, T>>;

// Тип для трюка по конструированию возвращаемого корутиной объекта inplace.
struct result_ready_t{};
constexpr result_ready_t result_ready;

// База для ожидателя корутины
struct job_waiter {
    std::coroutine_handle<> run_after_;
    std::exception_ptr exception_{};
    std::atomic_size_t* done_count_{};
    void check_exception() const {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }
};

// База для промиса корутины
struct job_promise_type_base {
    // Кто нас ждёт
    job_waiter* waiter_{};

    void unhandled_exception() noexcept {
        waiter_->exception_ = std::current_exception();
    }
    // Эта структура работает при завершении корутины
    struct final_awaitable : std::suspend_always {
        // Вызывается при последнем `suspend` корутины. Тут мы можем освободить общие ресурсы, уменьшить счётчик ссылок,
        // добавленный при запуске, решить, какую корутину выполнять дальше
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coroutine) noexcept {
            job_waiter* waiter = coroutine.promise().waiter_;
            coroutine.destroy();
            if (waiter->done_count_) {
                if (waiter->done_count_->fetch_sub(1, std::memory_order_relaxed) != 1) {
                    return std::noop_coroutine();
                }
            }
            return waiter->run_after_; // Возобновим уснувшую на нас корутину
        }
    };
    std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaitable final_suspend() noexcept {return {};}
};

// Промис корутины с типом возвращаемого значения
template<typename T>
struct typed_promise : job_promise_type_base {
    // Куда сохранять результат
    job_ret_type_t<T>* result_{};
    bool result_initiated_{};

    // Это для возвращения значения в co_return.
    template<typename V> requires std::is_convertible_v<V&&, T>
    void return_value(V&& v) noexcept(std::is_nothrow_constructible_v<T, V&&>) {
        assert(!result_initiated_);
        if constexpr (std::is_reference_v<T>) {
            result_->emplace(std::addressof(const_cast<std::remove_cvref_t<T>&>(v)));
        } else {
            result_->emplace(std::forward<V>(v));
        }
        result_initiated_ = true;
    }
    // Это для трюка, когда мы уже инициализировали возвращаемое значение не через return_value,
    // а в co_return что-то указать надо.
    void return_value(const result_ready_t&) {
        assert(result_initiated_);
    }

    void unhandled_exception() noexcept {
        job_promise_type_base::unhandled_exception();
        if constexpr (!std::is_reference_v<T> && !std::is_trivially_destructible_v<T>) {
            if (result_initiated_) {
                // С точки зрения компилятора uninit<T> слева от присваивания всё ещё не инициализирован,
                // но мы уже разместили в нём значение, вызовем деструктор принудительно
                result_->~job_ret_type_t<T>();
            }
        }
    }
};

// Промис void корутины
template<>
struct typed_promise<void> : job_promise_type_base {
    void return_void() noexcept {}
};

// Этот объект будем возвращать из co_emplace;
// Он знает, где лежит неинициализированное возвращаемое корутиной значение, и позволяет инициализировать его
// раньше, чем в co_return, и с нужным конструктором.
template<typename T>
class co_emplace_t {
    uninit<T>& res_;    // Здесь храним ссылку на возвращаемое корутиной значение
    bool& initiated_; // Сюда пишем флаг инициализированности.
public:
    co_emplace_t(uninit<T>& res, bool& initiated) : res_(res), initiated_(initiated){}
    co_emplace_t(const co_emplace_t&) = delete;
    co_emplace_t& operator=(const co_emplace_t&) = delete;

    template<typename...Args> requires std::is_constructible_v<T, Args...>
    T& operator()(Args&&...args) && noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        assert(!initiated_);
        T& ret = res_.emplace(std::forward<Args>(args)...);
        initiated_ = true;
        return ret;
    }
};

// Это awaiter, который возвращает co_emplace_t
template<typename T>
struct await_co_emplace {
    constexpr bool await_ready() const noexcept {return false;}

    uninit<T>* ret_;
    bool* initiated_;

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        P& promise = s.promise();
        assert(!promise.result_initiated_);
        ret_ = promise.result_;
        initiated_ = &promise.result_initiated_;
        return false;
    }
    co_emplace_t<T> await_resume() const {
        return {*ret_, *initiated_};
    }
};

// Этот объект будем возвращать из co_for_emplace.
// Он передаст в вызываемую корутину ссылку на возвращаемое значение, и если она вернётся без исключений
// установит флаг инициализированности.
template<typename T>
class co_for_emplace_t {
    uninit<T>& res_;  // Здесь храним ссылку на возвращаемое корутиной значение
    bool& initiated_; // Сюда пишем флаг инициализированности.
public:
    co_for_emplace_t(uninit<T>& res, bool& initiated) : res_(res), initiated_(initiated){}
    co_for_emplace_t(const co_for_emplace_t&) = delete;
    co_for_emplace_t& operator=(const co_for_emplace_t&) = delete;

    uninit<T>& res()&& {
        assert(!initiated_);
        return res_;
    }
    ~co_for_emplace_t() {
        if (!std::uncaught_exceptions()) {
            initiated_ = true;
        }
    }
};

// Это awaiter, который возвращает co_for_emplace_t
template<typename T>
struct await_co_for_emplace {
    constexpr bool await_ready() const noexcept {return false;}

    uninit<T>* ret_;
    bool* initiated_;

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        P& promise = s.promise();

        assert(!promise.result_initiated_);

        ret_ = promise.result_;
        initiated_ = &promise.result_initiated_;
        return false;
    }
    co_for_emplace_t<T> await_resume() const {
        return {*ret_, *initiated_};
    }
};

template<typename T>
struct co_result_ref_t {
    T& res_;    // Здесь храним ссылку на возвращаемое корутиной значение
    co_result_ref_t(T& res) : res_(res){}
    co_result_ref_t(const co_result_ref_t&) = delete;
    co_result_ref_t& operator=(const co_result_ref_t&) = delete;
};

// Это awaiter, который возвращает co_result_ref_t
template<typename T>
struct await_co_result_ref {
    constexpr bool await_ready() const noexcept {return false;}

    uninit<T>* ret_;

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        P& promise = s.promise();
        assert(promise.result_initiated_);
        ret_ = promise.result_;
        return false;
    }
    co_result_ref_t<T> await_resume() const {
        return {**ret_};
    }
};

template<typename T>
struct co_await_to_t {
    co_await_to_t(uninit<T>& res) : res_(res){}
    decltype(auto) operator()(auto&& j) {
        return std::move(j).to(res_);
    }
    uninit<T>& res_;
};

// Тип для трюка с инициализацией возвращаемого корутиной значения ещё до co_return.
constexpr struct co_emplace_trick{} co_emplace_v;
// Тип для трюка с передачей вызываемой корутине ссылки на своё возвращаемое значение.
constexpr struct co_for_emplace_trick{} co_for_emplace_v;
// Тип для трюка с получением ссылки на своё уже инициированное возвращаемое значение.
constexpr struct co_result_ref_trick{} co_result_ref_v;

// Несколько макросов для более удобного использования
#define co_emplace (co_await co_emplace_v)
#define co_for_emplace (co_await co_for_emplace_v).res()
#define co_result_ref() ((co_await co_result_ref_v).res_)
#define co_await_to(p) co_await co_await_to_t{p}
#define co_done co_return result_ready

// База для awaiter'ов, представляющих корутину
template<typename J>
struct awaitable_base : job_waiter {
    using promise_type = typename J::promise_type;

    std::coroutine_handle<promise_type> my_coro_;

    awaitable_base(J& w) noexcept : my_coro_(w.my_coro_) {
        w.my_coro_ = {};
    }

    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspendedCoro) noexcept {
        // Запомним кого запускать после себя
        run_after_ = suspendedCoro;
        // Своей корутине установим себя как текущего ожидателя
        my_coro_.promise().waiter_ = this;
        // Запустим корутину
        return my_coro_;
    }
};

// Простой типовой ожидатель нашей корутины. В себе хранит неинициализированное возвращаемое корутиной
// значение, которое они инициализирует через co_return, и возвращает его в await_resume.
template<typename J, typename K>
struct awaitable : awaitable_base<J> {
    using awaitable_base<J>::awaitable_base;
    job_ret_type_t<K> result_;  // Здесь лежит возвращаемое корутиной значение

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspendedCoro) noexcept {
        // Укажем корутине, куда сохранять возвращаемое значение
        this->my_coro_.promise().result_ = &result_;
        return awaitable_base<J>::await_suspend(suspendedCoro);
    }

    decltype(auto) await_resume() {
        this->check_exception();
        if constexpr (std::is_reference_v<K>) {
            return static_cast<K>(**result_);
        } else {
            return std::move(*result_);
        }
    }
};

// Специализация для void
template<typename J>
struct awaitable<J, void> : awaitable_base<J> {
    using awaitable_base<J>::awaitable_base;

    void await_resume() {
        this->check_exception();
    }
};

// Ожидатель корутины, инициализируемый внешним неинициализированным значением.
// Хранит ссылку на неинициализированное значение.
template<typename J, typename K>
struct awaitable_transfer : awaitable_base<J> {
    uninit<K>& result_;
    awaitable_transfer(J& w, uninit<K>& r) noexcept : awaitable_base<J>(w), result_(r){}

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspendedCoro) noexcept {
        // Укажем корутине, куда сохранять возвращаемое значение
        this->my_coro_.promise().result_ = &result_;
        return awaitable_base<J>::await_suspend(suspendedCoro);
    }

    uninit<K>&& await_resume() {
        this->check_exception();
        return std::move(result_);
    }
};

// База для запускателя корутины без co_await
template<typename J>
struct exec_base : job_waiter {
    using promise_type = typename J::promise_type;
    std::coroutine_handle<promise_type> my_coro_;

    exec_base(J& w) noexcept : my_coro_(w.my_coro_) {
        w.my_coro_ = {};
        run_after_ = std::noop_coroutine();
        my_coro_.promise().waiter_ = this;
    }
    bool done() const {
        return my_coro_.done();
    }
    void resume() const {
        my_coro_.resume();
        this->check_exception();
    }
};

// Запускатель корутины, который в себе хранит неинициализированное возвращаемое корутиной
// значение, которое она инициализирует через co_return, и возвращает его в result.
template<typename J, typename K>
struct executable : exec_base<J> {
    job_ret_type_t<K> result_;

    executable(J& w) : exec_base<J>(w) {
        // Укажем корутине, куда сохранять значение
        this->my_coro_.promise().result_ = &result_;
        this->resume();
    }
    decltype(auto) result() && {
        if constexpr (std::is_reference_v<K>) {
            return static_cast<K>(**result_);
        } else {
            return std::move(*result_);
        }
    }
};
// Специализация для void
template<typename J>
struct executable<J, void> : exec_base<J> {
    executable(J& w) : exec_base<J>(w) {
        this->resume();
    }
};
// Запускатель корутины, инициализируемый внешним неинициализированным значением.
// Хранит ссылку на неинициализированное значение.
template<typename J, typename K>
struct executable_transfer : exec_base<J> {
    uninit<K>& result_;
    executable_transfer(J& w, uninit<K>& r) : exec_base<J>(w), result_(r) {
        // Укажем корутине, куда сохранять значение
        this->my_coro_.promise().result_ = &result_;
        this->resume();
    }
    uninit<K>&& result() && {
        return std::move(result_);
    }
};

// Тип для простой корутины
template<typename T = void>
struct job {
    using ret_type = T;

    job(const job&) = delete;
    job(job&& other) noexcept : my_coro_(other.my_coro_) {
        other.my_coro_ = {};
    }

    job& operator=(job o) {
        std::swap(my_coro_, o.my_coro_);
        return *this;
    }

    struct promise_type : typed_promise<T> {
        job<T> get_return_object() {
            return job<T>{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Для трюков с co_emplace и т.п.
        template<typename Awaiter>
        decltype(auto) await_transform(Awaiter&& a) {
            using awaiter = std::remove_cvref_t<Awaiter>;
            if constexpr (std::is_same_v<awaiter, co_emplace_trick>) {
                static_assert(!std::is_reference_v<T>);
                return await_co_emplace<T>{};
            } else if constexpr (std::is_same_v<awaiter, co_for_emplace_trick>) {
                static_assert(!std::is_reference_v<T>);
                return await_co_for_emplace<T>{};
            } else if constexpr (std::is_same_v<awaiter, co_result_ref_trick>) {
                static_assert(!std::is_reference_v<T>);
                return await_co_result_ref<T>{};
            } else if constexpr (requires { std::forward<Awaiter>(a).operator co_await(); }) {
                return std::forward<Awaiter>(a).operator co_await();
            } else {
                return std::forward<Awaiter>(a);
            }
        }
    };

    // co_await для нашей корутины
    awaitable<job, T> operator co_await() && noexcept {
        return { *this };
    }

    // Метод для запуска ожидания с возвращением во внешнее неинициализированное значение.
    template<typename K> requires (!std::is_reference_v<T> && std::is_same_v<T, K>)
    awaitable_transfer<job, K> to(const uninit<K>& ret) && {
        return { *this, const_cast<uninit<K>&>(ret) };
    }

    // Запуск корутины
    executable<job, T> exec() && {
        return { *this };
    }

    // Запуск корутины с сохранением результата во внешнем значении
    template<typename K> requires (!std::is_reference_v<T> && std::is_same_v<T, K>)
    executable_transfer<job, K> exec_to(const uninit<K>& ret) && {
        return { *this, const_cast<uninit<K>&>(ret) };
    }

    std::coroutine_handle<promise_type> my_coro_;

    explicit job(std::coroutine_handle<promise_type> c) : my_coro_(c){}

    ~job() {
        if (my_coro_) {
            my_coro_.destroy();
        }
    };
};
