//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/tools/execute_query/execute_query_tool.h"

#include "zetasql/base/path.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/base/testing/status_matchers.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/testdata/test_schema.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "zetasql/base/statusor.h"

namespace zetasql {

using testing::IsEmpty;
using zetasql_base::testing::StatusIs;
using ToolMode = ExecuteQueryConfig::ToolMode;

TEST(SetToolModeFromFlags, ToolMode) {
  auto CheckFlag = [](absl::string_view name, ToolMode expected_mode) {
    absl::SetFlag(&FLAGS_mode, name);
    ExecuteQueryConfig config;
    ZETASQL_EXPECT_OK(SetToolModeFromFlags(config));
    EXPECT_EQ(config.tool_mode(), expected_mode);
  };
  CheckFlag("parse", ToolMode::kParse);
  CheckFlag("resolve", ToolMode::kResolve);
  CheckFlag("explain", ToolMode::kExplain);
  CheckFlag("execute", ToolMode::kExecute);
}

TEST(SetDescriptorPoolFromFlags, DescriptorPool) {
  {
    absl::SetFlag(&FLAGS_descriptor_pool, "none");
    ExecuteQueryConfig config;
    ZETASQL_EXPECT_OK(SetDescriptorPoolFromFlags(config));

    const Type* type = nullptr;
    ZETASQL_EXPECT_OK(config.mutable_catalog().GetType("zetasql_test.KitchenSinkPB",
                                               &type));
    EXPECT_EQ(type, nullptr);
  }
}

TEST(SetToolModeFromFlags, BadToolMode) {
  absl::SetFlag(&FLAGS_mode, "bad-mode");
  ExecuteQueryConfig config;
  EXPECT_THAT(SetToolModeFromFlags(config),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

static std::string TestDataDir() {
  return zetasql_base::JoinPath(
      getenv("TEST_SRCDIR"),
      "com_google_zetasql/zetasql/tools/execute_query/testdata");
}

static void VerifyDataMatches(
    const Table& table, const std::vector<std::vector<Value>>& expected_table) {
  std::vector<int> all_columns;
  for (int i = 0; i < table.NumColumns(); ++i) {
    all_columns.push_back(i);
  }

  zetasql_base::StatusOr<std::unique_ptr<EvaluatorTableIterator>> iterator_or =
      table.CreateEvaluatorTableIterator(all_columns);
  ZETASQL_EXPECT_OK(iterator_or);
  EvaluatorTableIterator* iter = iterator_or->get();

  for (int row = 0; row < expected_table.size(); ++row) {
    const std::vector<Value>& expected_row = expected_table[row];
    EXPECT_TRUE(iter->NextRow());
    EXPECT_EQ(iter->NumColumns(), expected_row.size());
    for (int col = 0; col < expected_row.size(); ++col) {
      EXPECT_EQ(iter->GetValue(col), expected_row[col]);
    }
  }
  EXPECT_FALSE(iter->NextRow()) << "Unexpected Extra rows";
}

static std::string CsvFilePath() {
  return zetasql_base::JoinPath(TestDataDir(), "test.csv");
}

TEST(MakeTableFromCsvFile, NoFile) {
  const std::string missing_file_path =
      zetasql_base::JoinPath(TestDataDir(), "nothing_here.csv");
  EXPECT_THAT(MakeTableFromCsvFile("ignored", missing_file_path),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(MakeTableFromCsvFile, Read) {
  zetasql_base::StatusOr<std::unique_ptr<const Table>> table_or =
      MakeTableFromCsvFile("great-table-name", CsvFilePath());
  ZETASQL_EXPECT_OK(table_or);
  const Table& table = **table_or;
  EXPECT_EQ(table.Name(), "great-table-name");
  EXPECT_EQ(table.NumColumns(), 3);
  const Column* col1 = table.GetColumn(0);
  const Column* col2 = table.GetColumn(1);
  const Column* col3 = table.GetColumn(2);
  EXPECT_EQ(col1->Name(), "col1");
  EXPECT_TRUE(col1->GetType()->IsString());

  EXPECT_EQ(col2->Name(), "col2");
  EXPECT_TRUE(col2->GetType()->IsString());

  EXPECT_EQ(col3->Name(), "col3");
  EXPECT_TRUE(col3->GetType()->IsString());

  VerifyDataMatches(table, {{Value::String("hello"), Value::String("45"),
                             Value::String("123.456")},
                            {Value::String("goodbye"), Value::String("90"),
                             Value::String("867.5309")}});
}

TEST(AddTablesFromFlags, BadFlags) {
  auto ExpectTableSpecIsInvalid = [](absl::string_view table_spec) {
    ExecuteQueryConfig config;
    absl::SetFlag(&FLAGS_table_spec, table_spec);
    EXPECT_FALSE(AddTablesFromFlags(config).ok());
  };

  ExpectTableSpecIsInvalid("===");
  ExpectTableSpecIsInvalid("BadTable=bad_format:ff");
  ExpectTableSpecIsInvalid("BadTable=csv:");  // empty path
  ExpectTableSpecIsInvalid("BadTable=csv:too:many_args");

  // SSTable
  ExpectTableSpecIsInvalid("BadTable=sstable::");  // empty path
  ExpectTableSpecIsInvalid("BadTable=sstable:too:many:args");
}

TEST(AddTablesFromFlags, GoodFlags) {
  ExecuteQueryConfig config;
  config.mutable_catalog().SetDescriptorPool(
      google::protobuf::DescriptorPool::generated_pool());

  absl::SetFlag(&FLAGS_table_spec,
                absl::StrCat(
                    "CsvTable=csv:",
                    CsvFilePath()));
  ZETASQL_EXPECT_OK(AddTablesFromFlags(config));

  absl::flat_hash_set<const Table*> tables;
  ZETASQL_EXPECT_OK(config.catalog().GetTables(&tables));
  EXPECT_EQ(tables.size(),  //
            1);

  const Table* csv_table = nullptr;
  ZETASQL_EXPECT_OK(config.mutable_catalog().GetTable("CsvTable", &csv_table));
  EXPECT_NE(csv_table, nullptr);
  EXPECT_EQ(csv_table->NumColumns(), 3);
}

const ProtoType* AddKitchenSink(ExecuteQueryConfig& config) {
  config.mutable_catalog().SetDescriptorPool(
      google::protobuf::DescriptorPool::generated_pool());
  const zetasql::Type* type = nullptr;
  ZETASQL_EXPECT_OK(
      config.mutable_catalog().GetType("zetasql_test.KitchenSinkPB", &type));
  return type->AsProto();
}

TEST(ExecuteQuery, ReadCsvTableFileEndToEnd) {
  ExecuteQueryConfig config;
  config.mutable_catalog().SetDescriptorPool(
      google::protobuf::DescriptorPool::generated_pool());

  absl::SetFlag(&FLAGS_table_spec,
                absl::StrCat("CsvTable=csv:", CsvFilePath()));
  ZETASQL_EXPECT_OK(AddTablesFromFlags(config));
  std::ostringstream output;
  ZETASQL_EXPECT_OK(
      ExecuteQuery("SELECT col1 FROM CsvTable ORDER BY col1", config, output));
  EXPECT_EQ(output.str(), R"(+---------+
| col1    |
+---------+
| goodbye |
| hello   |
+---------+

)");
}

TEST(ExecuteQuery, Parse) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kParse);
  std::ostringstream output;
  ZETASQL_EXPECT_OK(ExecuteQuery("select 1", config, output));
  EXPECT_EQ(output.str(), R"(QueryStatement [0-8]
  Query [0-8]
    Select [0-8]
      SelectList [7-8]
        SelectColumn [7-8]
          IntLiteral(1) [7-8]

)");
}

TEST(ExecuteQuery, Resolve) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kResolve);
  std::ostringstream output;
  ZETASQL_EXPECT_OK(ExecuteQuery("select 1", config, output));
  EXPECT_EQ(output.str(), R"(QueryStmt
+-output_column_list=
| +-$query.$col1#1 AS `$col1` [INT64]
+-query=
  +-ProjectScan
    +-column_list=[$query.$col1#1]
    +-expr_list=
    | +-$col1#1 := Literal(type=INT64, value=1)
    +-input_scan=
      +-SingleRowScan

)");
}

TEST(ExecuteQuery, Explain) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kExplain);
  std::ostringstream output;
  ZETASQL_EXPECT_OK(ExecuteQuery("select 1", config, output));
  EXPECT_EQ(output.str(), R"(RootOp(
+-input: ComputeOp(
  +-map: {
  | +-$col1 := ConstExpr(1)},
  +-input: EnumerateOp(ConstExpr(1))))
)");
}

TEST(ExecuteQuery, Execute) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kExecute);
  std::ostringstream output;
  ZETASQL_EXPECT_OK(ExecuteQuery("select 1", config, output));
  EXPECT_EQ(output.str(), R"(+---+
|   |
+---+
| 1 |
+---+

)");
}

TEST(ExecuteQuery, ExecuteError) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kExecute);
  std::ostringstream output;
  EXPECT_THAT(ExecuteQuery("select a", config, output),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ExecuteQuery, ExamineResolvedASTCallback) {
  ExecuteQueryConfig config;
  config.set_tool_mode(ToolMode::kExecute);
  config.set_examine_resolved_ast_callback(
      [](const ResolvedNode*) -> absl::Status {
        return absl::Status(absl::StatusCode::kFailedPrecondition, "");
      });

  std::ostringstream output;
  EXPECT_THAT(ExecuteQuery("select 1", config, output),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(output.str(), IsEmpty());
}

}  // namespace zetasql