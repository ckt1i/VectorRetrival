#include <gtest/gtest.h>
#include "vdb/common/status.h"

namespace vdb {
namespace {

TEST(StatusTest, OkStatus) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_EQ(s.ToString(), "OK");
  EXPECT_TRUE(s.message().empty());
}

TEST(StatusTest, ErrorStatuses) {
  Status s1 = Status::InvalidArgument("bad input");
  EXPECT_FALSE(s1.ok());
  EXPECT_TRUE(s1.IsInvalidArgument());
  EXPECT_EQ(s1.code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(s1.message(), "bad input");
  EXPECT_EQ(s1.ToString(), "InvalidArgument: bad input");
  
  Status s2 = Status::NotFound("key not found");
  EXPECT_FALSE(s2.ok());
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(s2.message(), "key not found");
  
  Status s3 = Status::IOError("disk failure");
  EXPECT_FALSE(s3.ok());
  EXPECT_TRUE(s3.IsIOError());
  
  Status s4 = Status::Corruption("bad checksum");
  EXPECT_FALSE(s4.ok());
  EXPECT_TRUE(s4.IsCorruption());
  
  Status s5 = Status::NotSupported("feature X");
  EXPECT_FALSE(s5.ok());
  EXPECT_TRUE(s5.IsNotSupported());
  
  Status s6 = Status::OutOfMemory("allocation failed");
  EXPECT_FALSE(s6.ok());
  EXPECT_TRUE(s6.IsOutOfMemory());
  
  Status s7 = Status::Internal("bug");
  EXPECT_FALSE(s7.ok());
  EXPECT_TRUE(s7.IsInternal());
}

TEST(StatusTest, CopyConstructor) {
  Status original = Status::InvalidArgument("test error");
  Status copy(original);
  
  EXPECT_EQ(copy.code(), original.code());
  EXPECT_EQ(copy.message(), original.message());
}

TEST(StatusTest, MoveConstructor) {
  Status original = Status::InvalidArgument("test error");
  Status moved(std::move(original));
  
  EXPECT_TRUE(moved.IsInvalidArgument());
  EXPECT_EQ(moved.message(), "test error");
}

TEST(StatusTest, CopyAssignment) {
  Status s1 = Status::OK();
  Status s2 = Status::InvalidArgument("error");
  
  s1 = s2;
  EXPECT_TRUE(s1.IsInvalidArgument());
  EXPECT_EQ(s1.message(), "error");
}

TEST(StatusTest, MoveAssignment) {
  Status s1 = Status::OK();
  Status s2 = Status::InvalidArgument("error");
  
  s1 = std::move(s2);
  EXPECT_TRUE(s1.IsInvalidArgument());
  EXPECT_EQ(s1.message(), "error");
}

TEST(StatusTest, SelfAssignment) {
  Status s = Status::InvalidArgument("error");
  s = s;  // Self-copy
  EXPECT_TRUE(s.IsInvalidArgument());
  EXPECT_EQ(s.message(), "error");
}

// Helper function for macro tests
Status TestFunction(bool succeed) {
  if (!succeed) {
    return Status::InvalidArgument("failed");
  }
  return Status::OK();
}

Status TestReturnIfError(bool inner_succeed) {
  VDB_RETURN_IF_ERROR(TestFunction(inner_succeed));
  return Status::OK();
}

TEST(StatusTest, ReturnIfErrorMacro) {
  EXPECT_TRUE(TestReturnIfError(true).ok());
  EXPECT_TRUE(TestReturnIfError(false).IsInvalidArgument());
}

Status TestCheck(int value) {
  VDB_CHECK(value > 0, "value must be positive");
  return Status::OK();
}

TEST(StatusTest, CheckMacro) {
  EXPECT_TRUE(TestCheck(1).ok());
  EXPECT_TRUE(TestCheck(0).IsInvalidArgument());
  EXPECT_EQ(TestCheck(-1).message(), "value must be positive");
}

// StatusOr tests
TEST(StatusOrTest, ValueConstruction) {
  StatusOr<int> result(42);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(result.value(), 42);
}

TEST(StatusOrTest, ErrorConstruction) {
  StatusOr<int> result(Status::NotFound("missing"));
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsNotFound());
}

TEST(StatusOrTest, MoveValue) {
  StatusOr<std::string> result(std::string("hello"));
  EXPECT_TRUE(result.ok());
  std::string s = std::move(*result);
  EXPECT_EQ(s, "hello");
}

TEST(StatusOrTest, ArrowOperator) {
  struct Data {
    int x = 10;
  };
  StatusOr<Data> result(Data{});
  EXPECT_EQ(result->x, 10);
}

TEST(StatusOrTest, BoolConversion) {
  StatusOr<int> success(42);
  StatusOr<int> failure(Status::Internal("error"));
  
  EXPECT_TRUE(static_cast<bool>(success));
  EXPECT_FALSE(static_cast<bool>(failure));
  
  if (success) {
    EXPECT_EQ(*success, 42);
  } else {
    FAIL() << "Should be true";
  }
}

StatusOr<int> ComputeValue(bool succeed) {
  if (!succeed) {
    return Status::InvalidArgument("computation failed");
  }
  return 100;
}

Status UseStatusOr(bool succeed) {
  VDB_ASSIGN_OR_RETURN(int value, ComputeValue(succeed));
  EXPECT_EQ(value, 100);
  return Status::OK();
}

TEST(StatusOrTest, AssignOrReturnMacro) {
  EXPECT_TRUE(UseStatusOr(true).ok());
  EXPECT_TRUE(UseStatusOr(false).IsInvalidArgument());
}

}  // namespace
}  // namespace vdb
