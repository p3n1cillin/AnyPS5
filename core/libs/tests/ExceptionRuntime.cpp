#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <typeinfo>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <csignal>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unwind.h>

extern "C" unsigned __cxa_uncaught_exceptions_nid_postfix();
extern "C" void* __cxa_current_primary_exception_nid_postfix();
extern "C" void __cxa_decrement_exception_refcount_nid_postfix(void*);
extern "C" void __cxa_rethrow_primary_exception_nid_postfix(void*);

extern "C" [[noreturn]] void _ZSt14_Xout_of_rangePKc_nid_postfix(const char*);
extern "C" [[noreturn]] void _ZSt13_Xrange_errorPKc_nid_postfix(const char*);
extern "C" [[noreturn]] void _ZNSt8__sce_v219_Xbad_function_callEv_nid_postfix();
extern "C" [[noreturn]] void __cxa_bad_cast_nid_postfix();
extern "C" [[noreturn]] void _ZSt19_Throw_bad_weak_ptrv_nid_postfix();
extern "C" [[noreturn]] void _ZNKSt9exception6_RaiseEv_nid_postfix(const void*);
extern "C" void* __cxa_vec_new3_nid_postfix(std::size_t, std::size_t, std::size_t, void(*)(void*), void(*)(void*), void*(*)(std::size_t), void(*)(void*, std::size_t));
extern "C" void __cxa_vec_delete3_nid_postfix(void*, std::size_t, std::size_t, void(*)(void*), void(*)(void*, std::size_t));
extern "C" _Unwind_Reason_Code _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn, void*);
extern "C" _Unwind_Reason_Code _Unwind_ForcedUnwind_nid_postfix(_Unwind_Exception*, _Unwind_Stop_Fn, void*);
extern "C" std::uintptr_t _Unwind_GetIP_nid_postfix(_Unwind_Context*);
extern "C" void (*_ZSt13set_terminatePFvvE_nid_postfix(void(*)()))();
extern "C" const unsigned char _ZTVN10__cxxabiv117__class_type_infoE_nid_postfix[];
extern "C" const unsigned char _ZTVN10__cxxabiv120__si_class_type_infoE_nid_postfix[];
extern "C" const unsigned char _ZTVN10__cxxabiv119__pointer_type_infoE_nid_postfix[];
extern "C" const unsigned char _ZTVN10__cxxabiv120__function_type_infoE_nid_postfix[];

thread_local int destroyed = 0;
struct Guard {
    ~Guard() { assert(__cxa_uncaught_exceptions_nid_postfix() > 0); ++destroyed; }
};
thread_local int counted = 0;
struct Counted {
    ~Counted() { ++counted; }
};
struct Base { virtual ~Base() = default; int value = 7; };
struct Other { virtual ~Other() = default; int padding = 9; };
struct Derived : Other, Base {};
struct Virtual : virtual Base {};

[[gnu::noinline]] void ThrowInt() { Guard guard; throw 42; }
[[gnu::noinline]] void ThrowClass() { Guard guard; throw Derived(); }

struct Left : Base {};
struct Right : Base {};
struct Repeated : Left, Right {};
struct LeftVirtual : virtual Base {};
struct RightVirtual : virtual Base {};
struct Diamond : LeftVirtual, RightVirtual {};
struct PrivateDerived : private Derived {
    Base* source() { return this; }
    Derived* target() { return this; }
};
[[gnu::noinline]] Derived* ToDerived(Base* p) { return dynamic_cast<Derived*>(p); }
[[gnu::noinline]] Repeated* ToRepeated(Base* p) { return dynamic_cast<Repeated*>(p); }
void CheckRtti() {
    Repeated repeated;
    Base* left = static_cast<Left*>(&repeated);
    assert(ToRepeated(left) == &repeated);
    assert(dynamic_cast<Right*>(left) == static_cast<Right*>(&repeated));
    PrivateDerived hidden;
    assert(ToDerived(hidden.source()) == hidden.target());
    try { throw static_cast<Repeated*>(nullptr); }
    catch (Base*) { assert(false); }
    catch (Repeated* p) { assert(p == nullptr); }
    try { throw static_cast<Diamond*>(nullptr); }
    catch (Base* p) { assert(p == nullptr); }
    try { throw nullptr; }
    catch (Base* p) { assert(p == nullptr); }
    int value = 1; int* pointer = &value;
    try { throw &pointer; }
    catch (const int**) { assert(false); }
    catch (int** p) { assert(p == &pointer); }
    try { throw &pointer; }
    catch (const int* const* p) { assert(**p == value); }
}
void CheckGuestPointerTypeInfo() {
    struct TypeRecord { const void* vtable; const char* name; };
    struct SingleRecord { TypeRecord type; const TypeRecord* base; };
    struct PointerRecord { TypeRecord type; unsigned flags; const TypeRecord* pointee; };
    const auto* classTable = _ZTVN10__cxxabiv117__class_type_infoE_nid_postfix;
    const auto* singleTable = _ZTVN10__cxxabiv120__si_class_type_infoE_nid_postfix;
    const auto* pointerTable = _ZTVN10__cxxabiv119__pointer_type_infoE_nid_postfix;
    const auto* functionTable = _ZTVN10__cxxabiv120__function_type_infoE_nid_postfix;
    const unsigned char* tables[] {pointerTable, functionTable};
    const char* names[] {"N10__cxxabiv119__pointer_type_infoE", "N10__cxxabiv120__function_type_infoE"};
    using KindQuery = bool (*)(const void*);
    for (std::size_t i = 0; i < 2; ++i) {
        const std::type_info* category;
        std::memcpy(&category, tables[i] + sizeof(void*), sizeof(category));
        assert(std::strcmp(category->name(), names[i]) == 0);
        KindQuery isPointer, isFunction;
        std::memcpy(&isPointer, tables[i] + 4 * sizeof(void*), sizeof(isPointer));
        std::memcpy(&isFunction, tables[i] + 5 * sizeof(void*), sizeof(isFunction));
        assert(isPointer(nullptr) == (i == 0) && isFunction(nullptr) == (i == 1));
    }
    TypeRecord base {classTable + 2 * sizeof(void*), "4Base"};
    TypeRecord other {classTable + 2 * sizeof(void*), "5Other"};
    TypeRecord voidType {classTable + 2 * sizeof(void*), "v"};
    SingleRecord single {{singleTable + 2 * sizeof(void*), "6Single"}, &base};
    TypeRecord function {functionTable + 2 * sizeof(void*), "FvvE"};
    PointerRecord basePointer {{pointerTable + 2 * sizeof(void*), "P4Base"}, 0, &base};
    PointerRecord otherPointer {{pointerTable + 2 * sizeof(void*), "P5Other"}, 0, &other};
    PointerRecord singlePointer {{pointerTable + 2 * sizeof(void*), "P6Single"}, 0, &single.type};
    PointerRecord voidPointer {{pointerTable + 2 * sizeof(void*), "Pv"}, 0, &voidType};
    PointerRecord functionPointer {{pointerTable + 2 * sizeof(void*), "PFvvE"}, 0, &function};
    using CatchType = bool (*)(const void*, const void*, void**, unsigned);
    CatchType catchType;
    std::memcpy(&catchType, pointerTable + 6 * sizeof(void*), sizeof(catchType));
    unsigned char storage[16] {};
    void* object = storage;
    assert(catchType(&basePointer, &singlePointer, &object, 0) && object == storage);
    object = storage;
    assert(!catchType(&otherPointer, &singlePointer, &object, 0));
    object = storage;
    assert(catchType(&voidPointer, &singlePointer, &object, 0));
    object = storage;
    assert(!catchType(&voidPointer, &functionPointer, &object, 0));
}

int foreignDeleted;
void CheckForeign() {
    auto* exception = new _Unwind_Exception {};
    exception->exception_class = 0x54455354464f5200;
    exception->exception_cleanup = [](_Unwind_Reason_Code reason, _Unwind_Exception* p) {
        assert(reason == _URC_FOREIGN_EXCEPTION_CAUGHT); ++foreignDeleted; delete p;
    };
    try {
        try { _Unwind_RaiseException(exception); assert(false); }
        catch (...) { assert(__cxa_current_primary_exception_nid_postfix() == nullptr); throw; }
    } catch (...) {}
    assert(foreignDeleted == 1);
}

int staticAttempts = 0;
[[gnu::noinline]] int StaticValue() {
    static int value = [] { if (++staticAttempts == 1) throw 91; return 37; }();
    return value;
}
void CheckStaticInitialization() {
    try { StaticValue(); assert(false); } catch (int value) { assert(value == 91); }
    assert(StaticValue() == 37 && StaticValue() == 37 && staticAttempts == 2);
}

int arrayConstructed, arrayDestroyed, arrayFreed;
bool failArray;
void ArrayConstruct(void* pointer) {
    if (failArray && arrayConstructed == 2) throw 73;
    *static_cast<int*>(pointer) = arrayConstructed++;
}
void ArrayDestroy(void* pointer) {
    assert(*static_cast<int*>(pointer) == arrayConstructed - 1 - arrayDestroyed);
    ++arrayDestroyed;
}
void ArrayFree(void* pointer, std::size_t size) {
    assert(size == 4 * sizeof(int) + sizeof(std::size_t));
    ++arrayFreed;
    std::free(pointer);
}
void CheckArrays() {
    failArray = true;
    try {
        __cxa_vec_new3_nid_postfix(4, sizeof(int), sizeof(std::size_t), ArrayConstruct, ArrayDestroy, std::malloc, ArrayFree);
        assert(false);
    } catch (int value) { assert(value == 73); }
    assert(arrayConstructed == 2 && arrayDestroyed == 2 && arrayFreed == 1);
    arrayConstructed = arrayDestroyed = arrayFreed = 0;
    failArray = false;
    void* array = __cxa_vec_new3_nid_postfix(4, sizeof(int), sizeof(std::size_t), ArrayConstruct, ArrayDestroy, std::malloc, ArrayFree);
    __cxa_vec_delete3_nid_postfix(array, sizeof(int), sizeof(std::size_t), ArrayDestroy, ArrayFree);
    assert(arrayConstructed == 4 && arrayDestroyed == 4 && arrayFreed == 1);
}
void* ThreadTest(void*) {
    for (int i = 0; i < 100; ++i) {
        try { ThrowInt(); } catch (int value) { assert(value == 42); }
        assert(__cxa_uncaught_exceptions_nid_postfix() == 0);
    }
    assert(destroyed == 100);
    return nullptr;
}
void CheckThreads() {
    pthread_t threads[4];
    for (auto& thread : threads) assert(pthread_create(&thread, nullptr, ThreadTest, nullptr) == 0);
    for (auto& thread : threads) assert(pthread_join(thread, nullptr) == 0);
}
[[gnu::noinline]] void UncaughtThrow() { throw 19; }
[[gnu::noinline]] void NoexceptThrow() noexcept { UncaughtThrow(); }
void CheckTerminate(void (*function)()) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _ZSt13set_terminatePFvvE_nid_postfix([] { _exit(61); });
        function();
        _exit(62);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 61);
}
void CheckDefaultTerminateMessage() {
    const std::string message = std::string(3000, 'w') + " end of message";
    int channel[2];
    assert(pipe(channel) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(channel[0]);
        dup2(channel[1], 2);
        throw std::runtime_error(message);
    }
    close(channel[1]);
    std::string output;
    char buffer[4096];
    for (ssize_t count; (count = read(channel[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, static_cast<std::size_t>(count));
    close(channel[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(output.find("  what():  " + message + "\n") != std::string::npos);
}
struct ForcedGuard { ~ForcedGuard() { ++destroyed; } };
[[gnu::noinline]] void ForceUnwind() {
    ForcedGuard guard;
    auto* exception = new _Unwind_Exception {};
    _Unwind_ForcedUnwind_nid_postfix(exception,
        [](int, _Unwind_Action actions, std::uint64_t, _Unwind_Exception*, _Unwind_Context*, void*) {
            if (actions & _UA_END_OF_STACK) _exit(destroyed == 1 ? 63 : 64);
            return _URC_NO_REASON;
        }, nullptr);
    _exit(65);
}
void CheckForcedUnwind() {
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) { destroyed = 0; ForceUnwind(); }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 63);
}

int main() {
    CheckGuestPointerTypeInfo();
    try { ThrowInt(); assert(false); } catch (int value) { assert(value == 42); }
    assert(destroyed == 1);
    try { ThrowClass(); assert(false); } catch (const Base& value) { assert(value.value == 7); }
    assert(destroyed == 2);
    try {
        try { throw 13; } catch (int value) { assert(value == 13); throw; }
    } catch (int value) { assert(value == 13); }
    assert(__cxa_uncaught_exceptions_nid_postfix() == 0);
    void* retained = nullptr;
    try { throw 27; } catch (...) { retained = __cxa_current_primary_exception_nid_postfix(); }
    try { __cxa_rethrow_primary_exception_nid_postfix(retained); assert(false); }
    catch (int value) { assert(value == 27); }
    __cxa_decrement_exception_refcount_nid_postfix(retained);
    {
        auto made = std::make_exception_ptr(Counted());
        try { std::rethrow_exception(made); assert(false); } catch (const Counted&) {}
    }
    assert(counted == 2);
    Derived object;
    Base* pointer = &object;
    try { throw &object; } catch (Base* value) { assert(value == pointer); }
    assert(dynamic_cast<Derived*>(pointer) == &object);
    assert(dynamic_cast<Other*>(pointer) == static_cast<Other*>(&object));
    try { throw Virtual(); } catch (Base& value) { assert(value.value == 7); }
    try { _ZSt14_Xout_of_rangePKc_nid_postfix("test message"); }
    catch (const std::logic_error& value) { assert(std::strcmp(value.what(), "test message") == 0); }
    try { _ZSt13_Xrange_errorPKc_nid_postfix("range message"); }
    catch (const std::range_error& value) { if (std::strcmp(value.what(), "range message") != 0) std::abort(); }
    try { _ZSt13_Xrange_errorPKc_nid_postfix(nullptr); }
    catch (const std::runtime_error& value) { if (std::strcmp(value.what(), "") != 0) std::abort(); }
    try { _ZSt13_Xrange_errorPKc_nid_postfix("not a logic error"); }
    catch (const std::logic_error&) { std::abort(); }
    catch (const std::range_error&) {}
    bool badFunctionCall = false;
    try { _ZNSt8__sce_v219_Xbad_function_callEv_nid_postfix(); }
    catch (const std::bad_function_call&) { badFunctionCall = true; }
    if (!badFunctionCall) std::abort();
    try { __cxa_bad_cast_nid_postfix(); }
    catch (const std::exception& value) { assert(value.what() != nullptr); }
    try { _ZSt19_Throw_bad_weak_ptrv_nid_postfix(); std::abort(); }
    catch (const std::bad_weak_ptr& value) { if (std::strcmp(value.what(), "bad_weak_ptr") != 0) std::abort(); }
    try { _ZNKSt9exception6_RaiseEv_nid_postfix(nullptr); assert(false); }
    catch (const std::invalid_argument&) {}
    try { _ZNKSt9exception6_RaiseEv_nid_postfix(&object); assert(false); }
    catch (const std::exception& value) { assert(std::strcmp(value.what(), "std::exception") == 0); }
    CheckRtti();
    CheckForeign();
    CheckStaticInitialization();
    CheckArrays();
    CheckThreads();
    CheckTerminate(UncaughtThrow);
    CheckTerminate(NoexceptThrow);
    CheckDefaultTerminateMessage();
    CheckForcedUnwind();
    int frames = 0;
    auto backtrace = _Unwind_Backtrace_nid_postfix([](_Unwind_Context* context, void* argument) {
        assert(_Unwind_GetIP_nid_postfix(context) != 0);
        ++*static_cast<int*>(argument);
        return _URC_NO_REASON;
    }, &frames);
    assert(backtrace == _URC_END_OF_STACK && frames >= 2);
    std::puts("exception runtime tests passed");
}
