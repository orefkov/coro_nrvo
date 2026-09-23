#include "jobs.h"
#include <gtest/gtest.h>
#include <vector>
/*!
 * @brief Простой объект
 */
struct simple_copy_moveable_object {
    int a_, b_, c_;
    simple_copy_moveable_object(int a, int b, int c) : a_(a), b_(b), c_(c) {
        std::cout << "Create simple_copy_moveable_object at " << *this << "\n";
    }
    ~simple_copy_moveable_object() {
        std::cout << "Destruct simple_copy_moveable_object at " << *this << "\n";
    }

    simple_copy_moveable_object(const simple_copy_moveable_object& o) : a_(o.a_), b_(o.b_), c_(o.c_) {
        std::cout << "Copy simple_copy_moveable_object from " << &o << " to " << *this << "\n";
    }

    simple_copy_moveable_object(simple_copy_moveable_object&& o) : a_(o.a_), b_(o.b_), c_(o.c_) {
        o.a_ = o.b_ = o.c_ = 0;
        std::cout << "Move simple_copy_moveable_object from " << &o << " to " << *this << "\n";
    }

    friend std::ostream& operator << (std::ostream& s, simple_copy_moveable_object& o) {
        s << &o << ": [" << o.a_ << ", " << o.b_ << ", " << o.c_ << "]";
        return s;
    }
};

/* Так как результат корутины на самом деле не "return'ится", а передается как аргумент в promise.return_value(),
   то ни RVO, ни NRVO тут не применяются. Внутри promise.return_value() значение должно быть куда-то сохранено,
   а потом передано пользователю корутины. Таким образом, в обычном случае мы получим два перемещения, если
   тип его поддерживает, и две копии, если в типе нет перемещения.
   Если конструктор типа имеет один параметр, а promise.return_value() реализован правильно, мы можем сэкономить
   одно перемещение, если будем конструировать возвращаемое значение сразу "по месту",
   сделав co_return аргумент_конструктора (некое подобие RVO), но NRVO так не получится.
*/
job<simple_copy_moveable_object> simple_return_object() {
    simple_copy_moveable_object obj{1, 2, 3};
    co_return obj;
}

TEST(Job, SimpleReturn) {
    // Здесь будет два перемещения и два промежуточных объекта.
    simple_copy_moveable_object obj = simple_return_object().exec().result();
    std::cout << "Get return object " << obj << "\n";
    EXPECT_TRUE(obj.a_ == 1 && obj.b_ == 2 && obj.c_ == 3);
}

TEST(Job, SimpleReturnTo) {
    // А здесь мы можем сэкономить одно перемещение и один промежуточный объект,
    // послав в корутину ссылку, куда сразу сохранять возвращаемый объект.
    uninit<simple_copy_moveable_object> obj = simple_return_object().exec_to(obj).result();
    std::cout << "Get return object " << *obj << "\n";
    EXPECT_TRUE(obj->a_ == 1 && obj->b_ == 2 && obj->c_ == 3);
}

// А эта корутина сразу создаёт возвращаемый объект там, где нужно, "по месту".
// При этом мы можем использовать конструкторы с любым количеством параметров.
job<simple_copy_moveable_object> simple_emplace_object() {
    // Инициализируем возвращаемое значение нужным конструктором
    auto& res = co_emplace(1, 2, 3);
    // При этом co_emplace возвращает ссылку на созданный объект, и мы можем при желании модифицировать его
    res.a_ += 10;
    // Нам нужно сделать фиктивный co_return, так как возвращаемое значение мы уже установили.
    co_done;
}

TEST(Job, SimpleReturnEmplaced) {
    // Обычный вызов такой корутины тоже экономит одно перемещение.
    simple_copy_moveable_object obj = simple_emplace_object().exec().result();
    std::cout << "Get return object " << obj << "\n";
    EXPECT_TRUE(obj.a_ == 11 && obj.b_ == 2 && obj.c_ == 3);
}

TEST(Job, SimpleReturnEmplacedUninit) {
    // А это комбинация обоих методов - мы передаём корутине ссылку, где размещать возвращаемый объект,
    // а корутина размещаем там результат. Получаем идеальный возврат - нужное значение
    // создаётся сразу "по месту", никаких перемещений или копий
    uninit<simple_copy_moveable_object> obj = simple_emplace_object().exec_to(obj).result();
    std::cout << "Get return object " << *obj << "\n";
    EXPECT_TRUE(obj->a_ == 11 && obj->b_ == 2 && obj->c_ == 3);
}

job<int> await_another_emplace_coro(int k) {
    // Корутина так же может ожидать другую корутину, передав ей ссылку на хранение результата.
    uninit<simple_copy_moveable_object> obj = co_await_to(obj)(simple_emplace_object());
    obj->a_ += k;
    co_return obj->a_ + obj->b_ + obj->c_;
}

TEST(Job, AwaitTo) {
    int r = await_another_emplace_coro(10).exec().result();
    EXPECT_EQ(r, 26);
}

// Идеальный возврат объекта из вложенных вызовов корутины
job<simple_copy_moveable_object> perfect_return() {
    // Передаём ссылку на возвращаемое значение другой корутине
    auto& my_result = *co_await_to(co_for_emplace)(simple_emplace_object());
    // Получаем ссылку на возвращаемый объект (его уже инициализировала вызванная корутина)
    //auto& my_result = co_result_ref();
    std::cout << "Get return from inner coroutine " << my_result << "\n";
    // Модифицируем то, что вернула другая корутина
    my_result.a_ += 10;
    // Готово
    co_done;
}

TEST(Job, PerfectInnerReturn) {
    // Здесь мы получаем результат из вложенного вызова корутины, но по прежнему
    // сразу в нужное место, без копирований и перемещений.
    uninit<simple_copy_moveable_object> obj = perfect_return().exec_to(obj).result();
    std::cout << "Get return object " << *obj << "\n";
    EXPECT_TRUE(obj->a_ == 21 && obj->b_ == 2 && obj->c_ == 3);
}

job<simple_copy_moveable_object> inner_call_with_exception() {
    auto& res = co_emplace(1, 2, 3);
    // При этом co_emplace возвращает ссылку на созданный объект, и мы можем при желании модифицировать его
    res.a_ += 10;
    // Выкинем исключение
    throw 1;
    co_done;
}

// Проверка деструкторов при передаче ссылки на возвращаемое значение во вложенную корутину при исключении в ней.
job<simple_copy_moveable_object> perfect_return_exception() {
    // Передаём ссылку на возвращаемое значение другой корутине
    co_await inner_call_with_exception().to(co_for_emplace);
    // Сюда не должны попасть из-за исключения в вызванной корутине
    // Получаем ссылку на свой результат
    auto& my_result = co_result_ref();
    std::cout << "Get return from inner coroutine " << my_result << "\n";
    // Модифицируем то, что вернула другая корутина
    my_result.a_ += 10;
    // Готово
    co_done;
}

// Проверка деструкторов при передаче ссылки на возвращаемое значение во вложенную корутину при исключении после неё.
job<simple_copy_moveable_object> perfect_return_post_exception() {
    // Передаём ссылку на возвращаемое значение другой корутине
    auto& my_result = *co_await_to(co_for_emplace)(simple_emplace_object());
    std::cout << "Get return from inner coroutine " << my_result << "\n";
    // Модифицируем то, что вернула другая корутина
    my_result.a_ += 10;
    throw 1;
    // Сюда не попадём
    co_done;
}

TEST(Job, PerfectInnerReturnException) {
    try {
        // Здесь мы получаем результат из вложенного вызова корутины, но по прежнему
        // сразу в нужное место, без копирований и перемещений.
        uninit<simple_copy_moveable_object> obj = perfect_return_exception().exec_to(obj).result();
        // Сюда не попадаем из-за исключения
        std::cout << "Get return object " << *obj << "\n";
        EXPECT_TRUE(obj->a_ == 21 && obj->b_ == 2 && obj->c_ == 3);
    } catch (int) {
        std::cout << "Catch int\n";
    }
}

TEST(Job, PerfectInnerReturnPostException) {
    try {
        // Здесь мы получаем результат из вложенного вызова корутины, но по прежнему
        // сразу в нужное место, без копирований и перемещений.
        uninit<simple_copy_moveable_object> obj = perfect_return_post_exception().exec_to(obj).result();
        // Сюда не попадаем из-за исключения
        std::cout << "Get return object " << *obj << "\n";
        EXPECT_TRUE(obj->a_ == 21 && obj->b_ == 2 && obj->c_ == 3);
    } catch (int) {
        std::cout << "Catch int\n";
    }
}

// Объект, который нельзя ни копировать, ни перемещать.
struct no_copy_move_object {
    int a_, b_, c_;
    no_copy_move_object(int a, int b, int c) : a_(a), b_(b), c_(c) {
        std::cout << "Create no_copy_move_object at " << *this << "\n";
    }
    ~no_copy_move_object() {
        std::cout << "Destruct no_copy_move_object at " << *this << "\n";
    }

    no_copy_move_object(const no_copy_move_object& o) = delete;
    no_copy_move_object& operator=(const no_copy_move_object&) = delete;

    friend std::ostream& operator << (std::ostream& s, no_copy_move_object& o) {
        s << &o << ": [" << o.a_ << ", " << o.b_ << ", " << o.c_ << "]";
        return s;
    }
};

no_copy_move_object rvo_return_no_copy_move() {
    // Это не работает
    //no_copy_move_object obj{1, 2, 3};
    //obj.a_ += 10;
    //return obj;
    // Работает только RVO
    return {1, 2, 3};

}

job<no_copy_move_object> return_no_copy_move() {
    // Это не работает, даже просто RVO
    // co_return no_copy_move_object{1, 2, 3};

    // А в корутине с такой техникой мы можем получить и NRVO
    auto& obj = co_emplace(1, 2, 3);
    obj.a_ += 10;
    co_done;
}

TEST(Job, NoCopyMoveRet) {
    no_copy_move_object obj1 = rvo_return_no_copy_move();
    std::cout << "Get no_copy_move_object from function " << obj1 << "\n";
    EXPECT_TRUE(obj1.a_ == 1 && obj1.b_ == 2 && obj1.c_ == 3);

    uninit<no_copy_move_object> obj2 = return_no_copy_move().exec_to(obj2).result();
    std::cout << "Get no_copy_move_object from coroutine " << *obj2 << "\n";
    EXPECT_TRUE(obj2->a_ == 11 && obj2->b_ == 2 && obj2->c_ == 3);
}

job<std::vector<int>> return_vector() {
    auto& result = co_emplace(3, 5);
    result.emplace_back(8);
    co_done;
}

TEST(Job, ReturnVector) {
    uninit<std::vector<int>> vec = return_vector().exec_to(vec).result();
    std::cout << "Get return vector in " << &*vec << "\n";
    EXPECT_EQ(*vec, (std::vector<int>{5, 5, 5, 8}));
}
