#include <gtest/gtest.h>
#include <iostream>
#include <unistd.h>

#include "mokv/cache/list.hpp"

template <typename T, typename List = cpputil::list::List<T> >
bool check_equal(List& a, std::vector<T>&& b) {
    if (a.size() != b.size()) {
        return false;
    }
    auto ita = a.begin();
    auto itb = b.begin();
    while (ita != a.end()) {
        if (*ita != *itb) {
            return false;
        }
        ++ita;
        ++itb;
    }
    return true;
}

TEST(ListTest, PushAndPop) {
    cpputil::list::List<int> list;
    for (int i = 100; i < 200; i++) {
        list.PushBack(i);
    }
    for (int i = 0; i < 99; i++) {
        list.PopBack();
    }
    ASSERT_EQ(*list.begin(), 100);
    cpputil::list::List<double> double_list;
    for (int i = 100; i < 200; i++) {
        double_list.PushBack(i);
    }
    for (int i = 0; i < 49; i++) {
        double_list.PopBack();
        double_list.PopFront();
    }
    ASSERT_EQ(int(*double_list.begin() + 0.5), 149);
    auto it = double_list.end();
    --it;
    ASSERT_EQ(int(*it + 0.5), 150);
}

TEST(ListTest, For) {
    cpputil::list::List<int> list;
    const int n = 10;
    for (int i = 0; i < n; i++) {
        list.PushBack(i);
    }
    ASSERT_EQ(list.size(), n);
    ASSERT_EQ(check_equal(list, std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), true);

    for (auto& x : list) {
        if (x == 9) {
            x = 114514;
        }
    }
    ASSERT_EQ(check_equal(list, std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 114514}), true);

    for (int i = 0; i < n / 2; i++) {
        list.PopBack();
    }

    ASSERT_EQ(list.size(), n - n / 2);
    ASSERT_EQ(check_equal(list, std::vector<int>{0, 1, 2, 3, 4}), true);
}