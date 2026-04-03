#include <gtest/gtest.h>
#include "mokv/cache/cm_sketch.hpp"
#include <iostream>

using cpputil::cache::utils::CMSketch4Bits;

TEST(CMSketchTest, Function) {
    CMSketch4Bits<int, 4> cm_sketch(10); // 1024
    cm_sketch.Increment(10);
    cm_sketch.Increment(12);
    cm_sketch.Increment(10);
    ASSERT_EQ(cm_sketch.Estimate(10), 2);
    ASSERT_EQ(cm_sketch.Estimate(12), 1);
    cm_sketch.Reset();
    ASSERT_EQ(cm_sketch.Estimate(10), 1);
    ASSERT_EQ(cm_sketch.Estimate(12), 0);
}