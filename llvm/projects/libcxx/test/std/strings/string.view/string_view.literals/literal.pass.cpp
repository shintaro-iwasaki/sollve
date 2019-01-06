// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: c++98, c++03, c++11
// UNSUPPORTED: clang-3.3, clang-3.4, clang-3.5, clang-3.6, clang-3.7, clang-3.8, clang-3.9
// UNSUPPORTED: apple-clang-6, apple-clang-7, apple-clang-8
// Note: libc++ supports string_view before C++17, but literals were introduced in C++14

#include <string_view>
#include <cassert>

#include "test_macros.h"

int main()
{
    using namespace std::literals::string_view_literals;

    static_assert ( std::is_same<decltype(   "Hi"sv), std::string_view>::value, "" );
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    static_assert ( std::is_same<decltype( u8"Hi"sv), std::string_view>::value, "" );
#endif
    static_assert ( std::is_same<decltype(  L"Hi"sv), std::wstring_view>::value, "" );
    static_assert ( std::is_same<decltype(  u"Hi"sv), std::u16string_view>::value, "" );
    static_assert ( std::is_same<decltype(  U"Hi"sv), std::u32string_view>::value, "" );

    std::string_view foo;
    std::wstring_view Lfoo;
    std::u16string_view ufoo;
    std::u32string_view Ufoo;

    foo  =   ""sv;     assert( foo.size() == 0);
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    foo  = u8""sv;     assert( foo.size() == 0);
#endif
    Lfoo =  L""sv;     assert(Lfoo.size() == 0);
    ufoo =  u""sv;     assert(ufoo.size() == 0);
    Ufoo =  U""sv;     assert(Ufoo.size() == 0);

    foo  =   " "sv;     assert( foo.size() == 1);
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    foo  = u8" "sv;     assert( foo.size() == 1);
#endif
    Lfoo =  L" "sv;     assert(Lfoo.size() == 1);
    ufoo =  u" "sv;     assert(ufoo.size() == 1);
    Ufoo =  U" "sv;     assert(Ufoo.size() == 1);

    foo  =   "ABC"sv;     assert( foo ==   "ABC");   assert( foo == std::string_view   (  "ABC"));
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    foo  = u8"ABC"sv;     assert( foo == u8"ABC");   assert( foo == std::string_view   (u8"ABC"));
#endif
    Lfoo =  L"ABC"sv;     assert(Lfoo ==  L"ABC");   assert(Lfoo == std::wstring_view  ( L"ABC"));
    ufoo =  u"ABC"sv;     assert(ufoo ==  u"ABC");   assert(ufoo == std::u16string_view( u"ABC"));
    Ufoo =  U"ABC"sv;     assert(Ufoo ==  U"ABC");   assert(Ufoo == std::u32string_view( U"ABC"));

    static_assert(  "ABC"sv.size() == 3, "");
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    static_assert(u8"ABC"sv.size() == 3, "");
#endif
    static_assert( L"ABC"sv.size() == 3, "");
    static_assert( u"ABC"sv.size() == 3, "");
    static_assert( U"ABC"sv.size() == 3, "");

    static_assert(noexcept(  "ABC"sv), "");
//	This is changed by P0482 to return a std::u8string - re-enable when we implement that.
#if TEST_STD_VER <= 17
    static_assert(noexcept(u8"ABC"sv), "");
#endif
    static_assert(noexcept( L"ABC"sv), "");
    static_assert(noexcept( u"ABC"sv), "");
    static_assert(noexcept( U"ABC"sv), "");
}