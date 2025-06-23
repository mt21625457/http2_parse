#include "gtest/gtest.h"

// This file is the entry point for the test executable.
// It initializes the Google Test framework.
// Specific tests will be in other _test.cpp files.

TEST(BasicSetupTest, SanityCheck) {
    // A very basic test to ensure GTest is linked and working.
    SUCCEED() << "GoogleTest framework is running.";
    ASSERT_EQ(1, 1);
    ASSERT_TRUE(true);
}

// You can add more TEST_F or TEST macros here for quick checks,
// but it's generally better to organize tests into separate files
// for different components, e.g., test_hpack_decoder.cpp, test_parser.cpp.

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
