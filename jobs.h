#pragma once
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <iostream>
#include <atomic>
#include <optional>

#if defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#define no_unique_address msvc::no_unique_address
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
template<typename T, bool AutoDestruct = !std::is_trivially_destructible_v<T>>
struct uninit {
    uninit() = default;
    uninit(uninit&& o) noexcept {
        // Так как этот тип предполагается использовать только для передачи неинициализированных
        // переменных в левую часть присваивания/инициализации самому себе же, сделаем конструктор
        // перемещения, в котором ничего не делаем.
        // Только проверим, что вызываемся сами для себя.
        assert(&o == this);
    }
    uninit(uninit<T, !AutoDestruct>&& o) noexcept {
        // Так как этот тип предполагается использовать только для передачи неинициализированных
        // переменных в левую часть присваивания/инициализации самому себе же, сделаем конструктор
        // перемещения, в котором ничего не делаем.
        // Только проверим, что вызываемся сами для себя.
        assert((uintptr_t)&o == (uintptr_t)this);
    }
    ~uninit() {
        if constexpr (AutoDestruct) {
            dtor();
        }
    }
    void dtor() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            reinterpret_cast<T*>(value_)->~T();
        }
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
template<typename T, bool AutoDestruct = false>
using job_store_type_t = uninit<std::conditional_t<std::is_reference_v<T>, std::remove_cvref_t<T>*, T>, AutoDestruct>;

/*!
 * @brief Базовый класс хранилища результата корутины
 */
struct coro_result_store {
    void* result_{};                // Куда сохранить результат.
    std::exception_ptr exception_;  // Произошедшее исключение.
    bool done_{};                   // Корутина была завершена.
    bool has_result_{};             // В корутине был установлен результат.
};

/*!
 * @brief Типизированная обёртка хранилища результата корутины
 * @tparam T - тип результата
 */
template<typename T>
struct typed_coro_result_store : coro_result_store {
    job_store_type_t<T>& result() {
        return *reinterpret_cast<job_store_type_t<T>*>(result_);
    }
};

/*!
 * @brief Дефолтное хранилище результата, в котором результат лежит тут же.
 * @tparam T - тип результата
 */
template<typename T>
struct typed_coro_result : typed_coro_result_store<T> {
    typed_coro_result() {
        this->result_ = &result_store_;
    }
    ~typed_coro_result() {
        if (this->has_result_) {
            result_store_.dtor();
        }
    }
    [[no_unique_address]]
    job_store_type_t<T> result_store_;
};


// Тип для трюка по конструированию возвращаемого корутиной объекта inplace.
struct result_ready_t{};
constexpr result_ready_t result_ready;

// База для ожидателя корутины
struct simple_job_waiter {
    std::coroutine_handle<> run_after_;
    std::atomic_size_t* done_count_{};
};

// База для промиса корутины
struct promise_type_base {
    // Кто нас ждёт
    simple_job_waiter* waiter_{};
    coro_result_store* result_place_{};     // Куда помещать результат

    void check_result_store() {
        assert(result_place_ != nullptr && "Не установлено хранилище результата!");
    }

    void unhandled_exception() noexcept {
        check_result_store();
        result_place_->exception_ = std::current_exception();
    }
    // Эта структура работает при завершении корутины
    struct final_awaitable : std::suspend_always {
        // Вызывается при последнем `suspend` корутины. Тут мы можем освободить общие ресурсы, уменьшить счётчик ссылок,
        // добавленный при запуске, решить, какую корутину выполнять дальше
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coroutine) noexcept {
            auto& promise = coroutine.promise();
            promise.check_result_store();
            promise.result_place_->done_ = true;

            simple_job_waiter* waiter = promise.waiter_;
            coroutine.destroy();
            if (waiter) {
                if (waiter->done_count_) {
                    if (waiter->done_count_->fetch_sub(1, std::memory_order_relaxed) != 1) {
                        return std::noop_coroutine();
                    }
                }
                return waiter->run_after_; // Возобновим уснувшую на нас корутину
            }
            return std::noop_coroutine();
        }
    };
    std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaitable final_suspend() noexcept {return {};}
};

// Промис корутины с типом возвращаемого значения
template<typename T>
struct typed_promise : promise_type_base {
    // Куда сохранять результат
    typed_coro_result_store<T>& result_store() {
        check_result_store();
        return *static_cast<typed_coro_result_store<T>*>(result_place_);
    };

    // Это для возвращения значения в co_return.
    template<typename V> requires std::is_convertible_v<V&&, T>
    void return_value(V&& v) noexcept(std::is_nothrow_constructible_v<T, V&&>) {
        auto& store = result_store();
        assert(!store.has_result_ && "Результат уже установлен!");

        if constexpr (std::is_reference_v<T>) {
            store.result().emplace(std::addressof(const_cast<std::remove_cvref_t<T>&>(v)));
        } else {
            store.result().emplace(std::forward<V>(v));
        }
        store.has_result_ = true;

    }
    // Это для трюка, когда мы уже инициализировали возвращаемое значение не через return_value,
    // а в co_return что-то указать надо.
    void return_value(const result_ready_t&) {
        assert(result_store().has_result_ && "Вызов co_done без установленного результата!");
    }

    void unhandled_exception() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            if (auto& store = result_store(); store.has_result_) {
                // С точки зрения компилятора uninit<T> слева от присваивания всё ещё не инициализирован,
                // но мы уже разместили в нём значение, вызовем деструктор принудительно
                store.result().dtor();
                store.has_result_ = false;
            }
        }
        promise_type_base::unhandled_exception();
    }
};

// Промис void корутины
template<>
struct typed_promise<void> : promise_type_base {
    void return_void() noexcept {
        check_result_store();
        result_place_->has_result_ = true;

    }
};
// Этот объект будем возвращать из co_emplace;
// Он знает, где лежит неинициализированное возвращаемое корутиной значение, и позволяет инициализировать его
// раньше, чем в co_return, и с нужным конструктором.
template<typename T>
class co_emplace_t {
    typed_coro_result_store<T>* res_;
public:
    co_emplace_t(typed_coro_result_store<T>* res) : res_(res){}
    co_emplace_t(const co_emplace_t&) = delete;
    co_emplace_t& operator=(const co_emplace_t&) = delete;

    template<typename...Args> requires std::is_constructible_v<T, Args...>
    T& operator()(Args&&...args) && noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        assert(!res_->has_result_);
        T& ret = res_->result().emplace(std::forward<Args>(args)...);
        res_->has_result_ = true;
        return ret;
    }
};

// Это awaiter, который возвращает co_emplace_t
template<typename T> requires (!std::is_void_v<T>)
struct await_co_emplace {
    constexpr bool await_ready() const noexcept {return false;}

    typed_coro_result_store<T>* res_{};

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        promise_type_base& promise = s.promise();
        promise.check_result_store();
        res_ = static_cast<typed_coro_result_store<T>*>(promise.result_place_);
        assert(!res_->has_result_ && "Результат уже установлен!");
        return false;
    }
    co_emplace_t<T> await_resume() const {
        return { res_ };
    }
};

// Этот объект будем возвращать из co_for_emplace.
// Он передаст в вызываемую корутину ссылку на возвращаемое значение, и если она вернётся без исключений
// установит флаг инициализированности.
template<typename T>
class co_for_emplace_t {
    typed_coro_result_store<T>* res_{};
public:
    co_for_emplace_t(typed_coro_result_store<T>* res) : res_(res){}
    co_for_emplace_t(const co_for_emplace_t&) = delete;
    co_for_emplace_t& operator=(const co_for_emplace_t&) = delete;

    job_store_type_t<T>& res() && {
        assert(!res_->has_result_);
        return res_->result();
    }
    ~co_for_emplace_t() {
        if (!std::uncaught_exceptions()) {
            res_->has_result_ = true;
        }
    }
};

// Это awaiter, который возвращает co_for_emplace_t
template<typename T> requires (!std::is_void_v<T>)
struct await_co_for_emplace {
    constexpr bool await_ready() const noexcept {return false;}

    typed_coro_result_store<T>* res_{};

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        promise_type_base& promise = s.promise();
        promise.check_result_store();
        res_ = static_cast<typed_coro_result_store<T>*>(promise.result_place_);
        assert(!res_->has_result_ && "Результат уже установлен!");
        return false;
    }
    co_for_emplace_t<T> await_resume() const {
        return { res_ };
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
template<typename T> requires (!std::is_void_v<T>)
struct await_co_result_ref {
    constexpr bool await_ready() const noexcept {return false;}

    job_store_type_t<T>* ret_;

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> s) {
        promise_type_base& promise = s.promise();
        promise.check_result_store();
        auto res = static_cast<typed_coro_result_store<T>*>(promise.result_place_);
        assert(res->has_result_ && "Результат ещё не установлен!");
        ret_ = &res->result();
        return false;
    }
    co_result_ref_t<T> await_resume() const {
        return { **ret_ };
    }
};

template<typename T> requires (!std::is_void_v<T>)
struct co_await_to_t {
    co_await_to_t(uninit<T, true>& res) : res_(reinterpret_cast<job_store_type_t<T>&>(res)){}
    co_await_to_t(uninit<T, false>& res) : res_(reinterpret_cast<job_store_type_t<T>&>(res)){}
    decltype(auto) operator()(auto&& j) {
        return std::move(j).to(res_);
    }
    job_store_type_t<T>& res_;
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



/*!
 * @brief База для самых простых awaiter'ов, которые возвращает самая обычная корутина, просто запускаемая в потоке
 * и возобновляющая после завершения вызывающую корутину.
 * @tparam J - тип корутины
 */
template<typename J>
struct simple_awaitable_base : simple_job_waiter {
    using promise_type = typename J::promise_type;

    std::coroutine_handle<promise_type> my_coro_;

    // Этот ожидатель работает для нерасшариваемых корутин, которых не могут ждать несколько ожидателей,
    // и не могут быть запускаемы из нескольких мест. Поэтому применимы только для prvalue джобов,
    // крадя их корутину.
    simple_awaitable_base(J&& w) noexcept : my_coro_(w.my_coro_) {
        w.my_coro_ = {};
    }

    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspended) noexcept {
        // Запомним кого запускать после себя
        run_after_ = suspended;
        // Своей корутине установим себя как текущего ожидателя
        auto& promise = my_coro_.promise();
        promise.waiter_ = this;
        // Запустим корутину
        return my_coro_;
    }
};

// Простой типовой ожидатель нашей корутины. В себе хранит неинициализированное возвращаемое корутиной
// значение, которое они инициализирует через co_return, и возвращает его в await_resume.
template<typename J, typename K>
struct simple_awaitable : simple_awaitable_base<J> {
    using simple_awaitable_base<J>::simple_awaitable_base;
    typed_coro_result<K> result_store_;  // Здесь лежит возвращаемое корутиной значение

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspended) noexcept {
        // Укажем корутине, куда сохранять возвращаемое значение
        this->my_coro_.promise().result_place_ = &result_store_;
        return simple_awaitable_base<J>::await_suspend(suspended);
    }

    decltype(auto) await_resume() {
        if (result_store_.exception_) {
            std::rethrow_exception(result_store_.exception_);
        }

        assert(result_store_.has_result_ && result_store_.done_ && "Результат не установлен или корутина не завершена!");
        if constexpr (std::is_void_v<K>) {
            return;
        } else if constexpr (std::is_reference_v<K>) {
            return static_cast<K>(**result_store_.result_store_);
        } else {
            return std::move(*result_store_.result_store_);
        }
    }
};

// Ожидатель корутины, инициализируемый внешним неинициализированным значением.
// Хранит ссылку на неинициализированное значение.
// В отличии от simple_awaitable мы не вызываем деструктор возвращаемого значения, так как
// не владеем им, а только передаём ссылку.
template<typename J, typename K>
struct awaitable_transfer : simple_awaitable_base<J> {
    typed_coro_result_store<K> result_store_;  // Здесь лежит возвращаемое корутиной значение

    awaitable_transfer(J&& w, job_store_type_t<K>& r) noexcept : simple_awaitable_base<J>(std::move(w)) {
        result_store_.result_ = &r;
    }

    template<typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> suspended) noexcept {
        // Укажем корутине, куда сохранять возвращаемое значение
        this->my_coro_.promise().result_place_ = &result_store_;
        return simple_awaitable_base<J>::await_suspend(suspended);
    }

    job_store_type_t<K>&& await_resume() {
        if (result_store_.exception_) {
            std::rethrow_exception(result_store_.exception_);
        }

        assert(result_store_.has_result_ && result_store_.done_ && "Результат не установлен или корутина не завершена!");
        return std::move(result_store_.result());
    }
};

// База для запускателя корутины без co_await
template<typename J>
struct exec_base {
    using promise_type = typename J::promise_type;
    std::coroutine_handle<promise_type> my_coro_;

    exec_base(J& j) noexcept : my_coro_(j.my_coro_) {
        j.my_coro_ = {};
    }
};

// Запускатель корутины, который в себе хранит неинициализированное возвращаемое корутиной
// значение, которое она инициализирует через co_return, и возвращает его в result.
template<typename J, typename K>
struct executable : exec_base<J> {
    typed_coro_result<K> result_store_;

    executable(J& j) : exec_base<J>(j) {
        // Укажем корутине, куда сохранять значение
        this->my_coro_.promise().result_place_ = &result_store_;
        resume();
    }
    void resume() const {
        assert(!done());
        this->my_coro_.resume();
        if (result_store_.exception_) {
            std::rethrow_exception(result_store_.exception_);
        }
    }
    bool done() const {
        return result_store_.done_;
    }
    decltype(auto) result() && {
        if (!(result_store_.has_result_ && result_store_.done_)) {
            throw std::bad_optional_access{};
        }
        if constexpr (std::is_void_v<K>) {
            return;
        }
        if constexpr (std::is_reference_v<K>) {
            return static_cast<K>(**result_store_.result());
        } else {
            return std::move(*result_store_.result());
        }
    }
};

// Запускатель корутины, инициализируемый внешним неинициализированным значением.
// Хранит ссылку на неинициализированное значение.
template<typename J, typename K>
struct executable_transfer : exec_base<J> {
    typed_coro_result_store<K> result_store_;

    executable_transfer(J& j, job_store_type_t<K>& r) : exec_base<J>(j) {
        result_store_.result_ = &r;
        // Укажем корутине, куда сохранять значение
        this->my_coro_.promise().result_place_ = &result_store_;
        resume();
    }
    void resume() const {
        assert(!done());
        this->my_coro_.resume();
        if (result_store_.exception_) {
            std::rethrow_exception(result_store_.exception_);
        }
    }
    bool done() const {
        return result_store_.done_;
    }
    job_store_type_t<K>&& result() {
        if (!(result_store_.has_result_ && result_store_.done_)) {
            throw std::bad_optional_access{};
        }
        return std::move(result_store_.result());
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
                return await_co_emplace<T>{};
            } else if constexpr (std::is_same_v<awaiter, co_for_emplace_trick>) {
                return await_co_for_emplace<T>{};
            } else if constexpr (std::is_same_v<awaiter, co_result_ref_trick>) {
                return await_co_result_ref<T>{};
            } else if constexpr (requires { std::forward<Awaiter>(a).operator co_await(); }) {
                return std::forward<Awaiter>(a).operator co_await();
            } else {
                return std::forward<Awaiter>(a);
            }
        }
        static void* operator new(size_t size) {
            return ::operator new(size);
        }
        static void operator delete(void* ptr, size_t size) {
            ::operator delete(ptr, size);
        }
    };
    // простой co_await для нашей корутины
    simple_awaitable<job, T> operator co_await() && noexcept {
        return { std::move(*this) };
    }

    // Метод для запуска ожидания с возвращением во внешнее неинициализированное значение.
    awaitable_transfer<job, T> to(const job_store_type_t<T, true>& ret) && {
        return { std::move(*this), reinterpret_cast<job_store_type_t<T>&>(const_cast<job_store_type_t<T, true>&>(ret)) };
    }

    // Метод для запуска ожидания с возвращением во внешнее неинициализированное значение.
    awaitable_transfer<job, T> to(const job_store_type_t<T, false>& ret) && {
        return { std::move(*this), const_cast<job_store_type_t<T, false>&>(ret) };
    }

    // Запуск корутины
    executable<job, T> exec() && {
        return { *this };
    }

    // Запуск корутины с сохранением результата во внешнем значении
    template<typename K, bool AD> requires (!std::is_reference_v<T> && std::is_same_v<T, K>)
    executable_transfer<job, K> exec_to(const uninit<K, AD>& ret) && {
        return { *this, reinterpret_cast<uninit<K, false>&>(const_cast<uninit<K, AD>&>(ret)) };
    }

    std::coroutine_handle<promise_type> my_coro_;

    explicit job(std::coroutine_handle<promise_type> c) : my_coro_(c){}

    ~job() {
        if (my_coro_) {
            my_coro_.destroy();
        }
    };
};
