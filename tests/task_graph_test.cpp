#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/task_graph.h"

namespace {

photobridge::RelativePath Path(const char* value)
{
    auto result = photobridge::RelativePath::Parse(value);
    EXPECT_TRUE(result.ok());
    return std::move(result.value());
}

photobridge::TaskSpec Task(const char* id, const char* key)
{
    return photobridge::TaskSpec{
        id,
        key,
        photobridge::TaskType::kMigrateFile,
        std::string("asset-") + id,
        std::string("source-") + id,
        Path((std::string("target-") + id + ".jpg").c_str()),
        0,
        std::nullopt,
    };
}

}  // namespace

TEST(TaskGraphTest, RejectsInvalidAndDuplicateNodesOrEdges)
{
    photobridge::TaskGraph graph;
    EXPECT_TRUE(graph.AddTask(Task("b", "key-b")).ok());
    EXPECT_TRUE(graph.AddTask(Task("a", "key-a")).ok());
    EXPECT_EQ(
        graph.AddTask(Task("a", "key-a-2")).code(),
        photobridge::StatusCode::kAlreadyExists);
    EXPECT_EQ(
        graph.AddTask(Task("c", "key-a")).code(),
        photobridge::StatusCode::kAlreadyExists);

    EXPECT_EQ(
        graph.AddDependency("a", "missing").code(),
        photobridge::StatusCode::kNotFound);
    EXPECT_EQ(
        graph.AddDependency("a", "a").code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_TRUE(graph.AddDependency("b", "a").ok());
    EXPECT_EQ(
        graph.AddDependency("b", "a").code(),
        photobridge::StatusCode::kAlreadyExists);
}

TEST(TaskGraphTest, ReturnsStableInitialReadyAndSerializationOrder)
{
    photobridge::TaskGraph graph;
    ASSERT_TRUE(graph.AddTask(Task("c", "key-c")).ok());
    ASSERT_TRUE(graph.AddTask(Task("a", "key-a")).ok());
    ASSERT_TRUE(graph.AddTask(Task("b", "key-b")).ok());
    ASSERT_TRUE(graph.AddDependency("c", "b").ok());

    EXPECT_EQ(
        graph.InitialReadyTasks(),
        (std::vector<photobridge::TaskId>{"a", "b"}));
    ASSERT_TRUE(graph.ValidateAcyclic().ok());

    const auto tasks = graph.Tasks();
    ASSERT_EQ(tasks.size(), 3U);
    EXPECT_EQ(tasks[0].id, "a");
    EXPECT_EQ(tasks[1].id, "b");
    EXPECT_EQ(tasks[2].id, "c");

    const auto dependencies = graph.Dependencies();
    ASSERT_EQ(dependencies.size(), 1U);
    EXPECT_EQ(dependencies[0].task, "c");
    EXPECT_EQ(dependencies[0].depends_on, "b");
}

TEST(TaskGraphTest, DetectsCyclesWithKahnAlgorithm)
{
    photobridge::TaskGraph graph;
    ASSERT_TRUE(graph.AddTask(Task("a", "key-a")).ok());
    ASSERT_TRUE(graph.AddTask(Task("b", "key-b")).ok());
    ASSERT_TRUE(graph.AddTask(Task("c", "key-c")).ok());
    ASSERT_TRUE(graph.AddDependency("b", "a").ok());
    ASSERT_TRUE(graph.AddDependency("c", "b").ok());
    ASSERT_TRUE(graph.AddDependency("a", "c").ok());

    EXPECT_FALSE(graph.ValidateAcyclic().ok());
    EXPECT_TRUE(graph.InitialReadyTasks().empty());
}
