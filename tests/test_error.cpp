#include "freikino/common/error.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using freikino::check_bool;
using freikino::check_hr;
using freikino::check_ptr;
using freikino::hresult_error;
using freikino::throw_hresult;
using freikino::throw_last_error;

// ---------------------------------------------------------------------------
// Feature correctness.

TEST(Error, ConstructorWrapsHresult) {
    const hresult_error e(E_OUTOFMEMORY);
    EXPECT_EQ(e.code(), E_OUTOFMEMORY);
    ASSERT_NE(e.what(), nullptr);
    EXPECT_GT(std::strlen(e.what()), 0u);
    // The formatted message should reference the code.
    EXPECT_NE(std::strstr(e.what(), "HRESULT"), nullptr);
}

TEST(Error, WhatReturnsStablePointer) {
    // `what()` is declared noexcept (override). Its pointer must be
    // stable across calls; no per-call allocation is allowed.
    const hresult_error e(E_POINTER);
    const char* w1 = e.what();
    const char* w2 = e.what();
    EXPECT_EQ(w1, w2);
}

TEST(ErrorCheckHr, SuccessCodesDoNotThrow) {
    EXPECT_NO_THROW(check_hr(S_OK));
    // S_FALSE is success per FAILED()'s sign check.
    EXPECT_NO_THROW(check_hr(S_FALSE));
}

TEST(ErrorCheckHr, FailureCodeThrows) {
    try {
        check_hr(E_FAIL);
        FAIL() << "expected hresult_error";
    } catch (const hresult_error& e) {
        EXPECT_EQ(e.code(), E_FAIL);
    }
}

TEST(ErrorCheckBool, TrueIsPassThrough) {
    EXPECT_NO_THROW(check_bool(TRUE));
}

TEST(ErrorCheckBool, FalseThrowsWithLastError) {
    ::SetLastError(ERROR_ACCESS_DENIED);
    try {
        check_bool(FALSE);
        FAIL() << "expected hresult_error";
    } catch (const hresult_error& e) {
        EXPECT_EQ(e.code(),
                  HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    }
}

TEST(ErrorCheckPtr, NonNullReturnsInput) {
    int x = 0;
    EXPECT_EQ(check_ptr(&x), &x);
}

TEST(ErrorCheckPtr, NullThrows) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    int* null_ptr = nullptr;
    EXPECT_THROW(check_ptr(null_ptr), hresult_error);
}

// ---------------------------------------------------------------------------
// Security: the constructor's formatting is wrapped in try/catch so an
// allocation failure can't leak an exception through what(). Verified
// indirectly — `what()` is noexcept-declared, and the exception itself
// must be throwable under normal conditions.

TEST(ErrorSecurity, WhatHonoursNoexceptDeclaration) {
    const hresult_error e(E_FAIL);
    const auto invoke = [&]() noexcept -> const char* {
        return e.what();
    };
    EXPECT_NE(invoke(), nullptr);
}

// ---------------------------------------------------------------------------
// Security: `throw_last_error` must always produce a *failure* HRESULT,
// even when GetLastError() == 0. Otherwise the thrown exception could
// carry S_OK (== 0), which callers doing `if (FAILED(e.code()))` would
// treat as success and quietly swallow.

TEST(ErrorSecurity, ThrowLastErrorMapsZeroToInvalidFunction) {
    ::SetLastError(0);
    try {
        throw_last_error();
        FAIL() << "expected hresult_error";
    } catch (const hresult_error& e) {
        EXPECT_TRUE(FAILED(e.code()));
        EXPECT_NE(e.code(), S_OK);
        EXPECT_EQ(e.code(),
                  HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION));
    }
}

TEST(ErrorSecurity, ThrowHresultPreservesCode) {
    try {
        throw_hresult(E_INVALIDARG);
        FAIL() << "expected hresult_error";
    } catch (const hresult_error& e) {
        EXPECT_EQ(e.code(), E_INVALIDARG);
    }
}

} // namespace
