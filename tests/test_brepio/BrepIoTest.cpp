#include <gtest/gtest.h>

#include <vine/brepio/BrepLoader.hpp>

using vn::brepio::BrepLoader;

TEST(BrepIoTest, BrepIsSupportedFormat)
{
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.stp"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.step"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.igs"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.iges"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.IGS"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.stl"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.obj"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.txt"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part"));
}

TEST(BrepIoTest, BrepDefaultInstanceIsSingleton)
{
    EXPECT_EQ(&BrepLoader::defaultInstance(), &BrepLoader::defaultInstance());
}

TEST(BrepIoTest, BrepOptionsRoundTrip)
{
    // The option interface is usable even though loading is not wired in yet.
    BrepLoader          loader;
    BrepLoader::Options options;
    options.placeholder = 'x';
    loader.setOptions(options);
    EXPECT_EQ(loader.options().placeholder, 'x');
    EXPECT_TRUE(loader.options() == options);
}
