// Copyright 2017 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef COBALT_ANALYZER_STORE_OBSERVATION_STORE_ABSTRACT_TEST_H_
#define COBALT_ANALYZER_STORE_OBSERVATION_STORE_ABSTRACT_TEST_H_

#include "analyzer/store/observation_store.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "analyzer/store/observation_store_internal.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

// This file contains type-parameterized tests of the ObservationStore.
//
// We use C++ templates along with the macros TYPED_TEST_CASE_P and
// TYPED_TEST_P in order to define test templates that may be instantiated to
// to produce concrete tests that use various implementations of Datastore.
//
// See observation_store_test.cc and observation_store_emulator_test.cc for the
// concrete instantiations.
//
// NOTE: If you add a new test to this file you must add its name to the
// invocation REGISTER_TYPED_TEST_CASE_P macro at the bottom of this file.

namespace cobalt {
namespace analyzer {
namespace store {

// ObservationStoreAbstractTest is templatized on the parameter
// |StoreFactoryClass| which must be the name of a class that contains the
// following methd: static DataStore* NewStore()
// See MemoryStoreFactory in memory_store_test.cc and
// BigtableStoreEmulatorFactory in bigtable_store_emulator_test.cc.
template <class StoreFactoryClass>
class ObservationStoreAbstractTest : public ::testing::Test {
 protected:
  ObservationStoreAbstractTest()
      : data_store_(StoreFactoryClass::NewStore()),
        observation_store_(new ObservationStore(data_store_)) {
    EXPECT_EQ(kOK, data_store_->DeleteAllRows(DataStore::kObservations));
  }

  // If board_name is non-empty then we add a SystemProfile to the
  // ObservationMetadata with the given board_name, otherwise we do not
  // add a SystemProfile to the ObservationMetadata.
  void AddObservationBatch(uint32_t metric_id, uint32_t day_index,
                           size_t num_parts, size_t num_observations,
                           const std::string& board_name) {
    ObservationMetadata metadata;
    metadata.set_customer_id(kCustomerId);
    metadata.set_project_id(kProjectId);
    metadata.set_metric_id(metric_id);
    metadata.set_day_index(day_index);
    if (!board_name.empty()) {
      metadata.mutable_system_profile()->set_board_name(board_name);
    }
    std::vector<Observation> observations;
    for (size_t i = 0; i < num_observations; i++) {
      Observation observation;
      for (size_t part_index = 0; part_index < num_parts; part_index++) {
        std::string part_name = PartName(part_index);
        ObservationPart observation_part;
        switch (part_index % 3) {
          case 0:
            observation_part.mutable_forculus()->set_ciphertext(part_name);
            break;
          case 1:
            observation_part.mutable_rappor()->set_data(part_name);
            break;
          default:
            observation_part.mutable_basic_rappor()->set_data(part_name);
            break;
        }
        (*observation.mutable_parts())[part_name].Swap(&observation_part);
      }
      observations.emplace_back();
      observations.back().Swap(&observation);
    }

    EXPECT_EQ(kOK,
              observation_store_->AddObservationBatch(metadata, observations));
  }

  // If board_name is non-empty then we add a SystemProfile to the
  // ObservationMetadata with the given board_name, otherwise we do not
  // add a SystemProfile to the ObservationMetadata.
  void AddObservations(uint32_t metric_id, uint32_t first_day_index,
                       uint32_t last_day_index, int num_per_day, int num_parts,
                       const std::string& board_name) {
    for (uint32_t day_index = first_day_index; day_index <= last_day_index;
         day_index++) {
      AddObservationBatch(metric_id, day_index, num_parts, num_per_day,
                          board_name);
    }
  }

  // Repeatedly invokes QueryObservations using the given data until all of
  // the results have been obtained. Returns the full list of results.
  std::vector<ObservationStore::QueryResult> QueryFullResults(
      uint32_t metric_id, uint32_t first_day_index, int32_t last_day_index,
      int num_parts, const SystemProfileFields& system_profile_fields,
      size_t max_results) {
    std::vector<std::string> parts;
    for (int part_index = 0; part_index < num_parts; part_index++) {
      parts.push_back(PartName(part_index));
    }
    std::vector<ObservationStore::QueryResult> full_results;
    std::string pagination_token = "";
    do {
      ObservationStore::QueryResponse query_response =
          observation_store_->QueryObservations(
              kCustomerId, kProjectId, metric_id, first_day_index,
              last_day_index, parts, system_profile_fields, max_results,
              pagination_token);
      EXPECT_EQ(kOK, query_response.status);
      for (auto& result : query_response.results) {
        EXPECT_EQ(kCustomerId, result.metadata.customer_id());
        EXPECT_EQ(kProjectId, result.metadata.project_id());
        EXPECT_EQ(metric_id, result.metadata.metric_id());
        full_results.emplace_back(std::move(result));
      }
      pagination_token = std::move(query_response.pagination_token);
    } while (!pagination_token.empty());
    return full_results;
  }

  void CheckFullResults(
      const std::vector<ObservationStore::QueryResult>& full_results,
      size_t expected_num_results, size_t expected_num_results_per_day,
      size_t expected_num_parts, uint32_t expected_first_day_index,
      const std::string& expected_board_name) {
    EXPECT_EQ(expected_num_results, full_results.size());
    uint result_index = 0;
    uint32_t expected_day_index = expected_first_day_index;
    for (const auto& result : full_results) {
      EXPECT_EQ(expected_day_index, result.metadata.day_index());
      EXPECT_EQ(expected_num_parts, result.observation.parts().size());
      if (expected_board_name.empty()) {
        EXPECT_FALSE(result.metadata.has_system_profile());
      } else {
        EXPECT_TRUE(result.metadata.has_system_profile());
        EXPECT_EQ(expected_board_name,
                  result.metadata.system_profile().board_name());
      }
      for (size_t part_index = 0; part_index < expected_num_parts;
           part_index++) {
        std::string expected_part_name = PartName(part_index);
        switch (part_index % 3) {
          case 0:
            EXPECT_TRUE(result.observation.parts()
                            .at(expected_part_name)
                            .has_forculus());
            break;
          case 1:
            EXPECT_TRUE(
                result.observation.parts().at(expected_part_name).has_rappor());
            break;
          default:
            EXPECT_TRUE(result.observation.parts()
                            .at(expected_part_name)
                            .has_basic_rappor());
            break;
        }
      }
      if (++result_index == expected_num_results_per_day) {
        result_index = 0;
        expected_day_index++;
      }
    }
  }

  store::Status DeleteAllForMetric(uint32_t metric_id) {
    return observation_store_->DeleteAllForMetric(kCustomerId, kProjectId,
                                                  metric_id);
  }

  // Generates a part name with the given index.
  std::string static PartName(int index) {
    std::string out(15, 100);
    std::snprintf(&out[0], out.size(), "part%.10d", index);
    return out;
  }
  static const uint32_t kCustomerId;
  static const uint32_t kProjectId;

  std::shared_ptr<DataStore> data_store_;
  std::unique_ptr<ObservationStore> observation_store_;
};

template <class StoreFactoryClass>
const uint32_t ObservationStoreAbstractTest<StoreFactoryClass>::kCustomerId = 1;

template <class StoreFactoryClass>
const uint32_t ObservationStoreAbstractTest<StoreFactoryClass>::kProjectId = 1;

TYPED_TEST_CASE_P(ObservationStoreAbstractTest);

TYPED_TEST_P(ObservationStoreAbstractTest, AddAndQuery) {
  // For metric 1, add 100 observations with 2 parts each for each day in the
  // range [100, 109]. We also specify a board_name which will cause the
  // Observations to be stored in the ObservationStore with a SystemProfile
  // that contains that vendor name.
  uint32_t metric_id = 1;
  std::string board_name = "fake board name";
  this->AddObservations(metric_id, 100, 109, 100, 2, board_name);

  // For metric 2, add 200 observations with 1 part each for each day in the
  // range [101, 110]. Here we specify an empty board_name which will cause
  // the Observations to be stored in the ObservationStore without a
  // SystemProfile.
  metric_id = 2;
  board_name = "";
  this->AddObservations(metric_id, 101, 110, 200, 1, board_name);

  /////////////////////////////////////////////////////////////////
  // Queries for metric 1
  /////////////////////////////////////////////////////////////////
  metric_id = 1;

  // Query for observations for days in the range [50, 150].
  // Ask for 2 parts.
  // Impose a maximum of 100 results.
  int requested_num_parts = 2;
  // We specify that the SystemProfile should not be returned by the query.
  // By setting expected_board_name = "" we indicate that we expect the
  // QueryResult to not contain a SystemProfile.
  SystemProfileFields system_profile_fields;
  std::string expected_board_name = "";
  std::vector<ObservationStore::QueryResult> full_results =
      this->QueryFullResults(metric_id, 50, 150, requested_num_parts,
                             system_profile_fields, 100);

  // Expect to find 1000 results as 100 results per day for 10 days starting
  // with day 100.
  // Expect to find 2 parts.
  int expected_num_results = 1000;
  int expected_num_results_per_day = 100;
  int expected_first_day_index = 100;
  int expected_num_parts = 2;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(metric_id, 50, 150, requested_num_parts,
                                        system_profile_fields, 100);

  // Expect the same results as above except with a SystemProfile.
  expected_board_name = "fake board name";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  //------------------------------------------------------------

  // Query for observations for days in the range [0, UINT32_MAX].
  full_results =
      this->QueryFullResults(metric_id, 0, UINT32_MAX, requested_num_parts,
                             system_profile_fields, 100);

  // Expect the same results as above.
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Query for observations for days in the range [100, 105].
  full_results = this->QueryFullResults(
      metric_id, 100, 105, requested_num_parts, system_profile_fields, 100);

  // Expect to find 600 results as 100 results per day for 6 days.
  expected_num_results = 600;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 500 results as 100 results per day for 5 days starting
  // with day 105.
  expected_num_results = 500;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Test that it works to not specify any requested parts. We should get
  // all of the parts.
  requested_num_parts = 0;

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 500 results as 100 results per day for 5 days starting
  // with day 105.
  expected_num_results = 500;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect the same results as above except with a SystemProfile.
  expected_board_name = "fake board name";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  //------------------------------------------------------------

  // Test that it works to request 1 part when there are two. We should
  // receive only 1.
  requested_num_parts = 1;
  expected_num_parts = 1;

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 500 results as 100 results per day for 5 days starting
  // with day 105.
  expected_num_results = 500;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect the same results as above except with a SystemProfile.
  expected_board_name = "fake board name";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  /////////////////////////////////////////////////////////////////
  // Queries for metric 2
  /////////////////////////////////////////////////////////////////
  metric_id = 2;

  // Query for observations for days in the range [50, 150].
  full_results = this->QueryFullResults(metric_id, 50, 150, requested_num_parts,
                                        system_profile_fields, 100);

  // Expect to find 2000 results as 200 results per day for 10 days starting
  // with day 101.
  // Expect to find 1 part.
  expected_num_results = 2000;
  expected_num_results_per_day = 200;
  expected_num_parts = 1;
  expected_first_day_index = 101;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(metric_id, 50, 150, requested_num_parts,
                                        system_profile_fields, 100);

  // Expect the same results including *no* SystemProfile because we never
  // wrote one to the database.
  expected_board_name = "";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  //------------------------------------------------------------

  // Query for observations for days in the range [0, UINT32_MAX].
  full_results =
      this->QueryFullResults(metric_id, 0, UINT32_MAX, requested_num_parts,
                             system_profile_fields, 100);

  // Expect the same results as above.
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Query for observations for days in the range [100, 105].
  full_results = this->QueryFullResults(
      metric_id, 100, 105, requested_num_parts, system_profile_fields, 100);

  // Expect to find 1000 results as 200 results per day for 5 days.
  expected_num_results = 1000;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 1200 results as 200 results per day for 6 days starting
  // with day 105.
  expected_num_results = 1200;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Test that it works to not specify any requested parts. We should get
  // all of the parts.
  requested_num_parts = 0;

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 1200 results as 200 results per day for 6 days starting
  // with day 105.
  expected_num_results = 1200;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect the same results including *no* SystemProfile because we never
  // wrote one to the database.
  expected_board_name = "";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  //------------------------------------------------------------

  // Test that it works to request 1 part when there is one part.
  requested_num_parts = 1;

  // Query for observations for days in the range [105, 110].
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect to find 1200 results as 200 results per day for 6 days starting
  // with day 105.
  expected_num_results = 1200;
  expected_first_day_index = 105;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  //------------------------------------------------------------

  // Same query as above except set system_profile_fields = true.
  system_profile_fields.Add(SystemProfileField::BOARD_NAME);
  full_results = this->QueryFullResults(
      metric_id, 105, 110, requested_num_parts, system_profile_fields, 100);

  // Expect the same results including *no* SystemProfile because we never
  // wrote one to the database.
  expected_board_name = "";
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);

  // Reset the values
  system_profile_fields = {};
  expected_board_name = "";

  /////////////////////////////////////////////////////////////////
  // Queries for metric 3
  /////////////////////////////////////////////////////////////////

  // For metric 3 expect to find 0 results.
  metric_id = 3;
  full_results =
      this->QueryFullResults(metric_id, 0, UINT32_MAX, requested_num_parts,
                             system_profile_fields, 100);
  EXPECT_EQ(0u, full_results.size());

  /////////////////////////////////////////////////////////////////
  // Queries for metric 0
  /////////////////////////////////////////////////////////////////

  // For metric 0 expect to find 0 results.
  metric_id = 0;
  full_results =
      this->QueryFullResults(metric_id, 0, UINT32_MAX, requested_num_parts,
                             system_profile_fields, 100);
  EXPECT_EQ(0u, full_results.size());

  /////////////////////////////////////////////////////////////////
  // Test the method DeleteAllForMetric.
  /////////////////////////////////////////////////////////////////
  metric_id = 1;
  EXPECT_EQ(kOK, this->DeleteAllForMetric(metric_id));
  // For metric 1 expect to find 0 results.
  full_results =
      this->QueryFullResults(metric_id, 0, UINT32_MAX, requested_num_parts,
                             system_profile_fields, 100);
  EXPECT_EQ(0u, full_results.size());

  // For metric 2 the results should be the same as above.
  metric_id = 2;

  // Query for observations for days in the range [50, 150].
  full_results = this->QueryFullResults(metric_id, 50, 150, requested_num_parts,
                                        system_profile_fields, 100);

  // Expect to find 2000 results as 200 results per day for 10 days starting
  // with day 101.
  // Expect to find 1 part.
  expected_num_results = 2000;
  expected_num_results_per_day = 200;
  expected_num_parts = 1;
  expected_first_day_index = 101;
  this->CheckFullResults(full_results, expected_num_results,
                         expected_num_results_per_day, expected_num_parts,
                         expected_first_day_index, expected_board_name);
}

TYPED_TEST_P(ObservationStoreAbstractTest, QueryWithInvalidArguments) {
  uint32_t customer_id = this->kCustomerId;
  uint32_t project_id = this->kProjectId;
  uint32_t metric_id = 1;
  uint32_t first_day_index = 42;
  uint32_t last_day_index = 42;

  // Try to use a pagination token that corresponds to a day index that
  // is too small. Expect kInvalidArguments.
  std::string pagination_token = internal::RowKey(
      customer_id, project_id, metric_id, first_day_index - 1, 0, 0);

  std::vector<std::string> parts;
  ObservationStore::QueryResponse query_response =
      this->observation_store_->QueryObservations(
          customer_id, project_id, metric_id, first_day_index, last_day_index,
          parts, {}, 0, pagination_token);

  EXPECT_EQ(kInvalidArguments, query_response.status);

  // Switch to a pagination token that corresponds to first_day_index.
  // Expect kOK.
  pagination_token = internal::RowKey(customer_id, project_id, metric_id,
                                      first_day_index, 0, 0);
  query_response = this->observation_store_->QueryObservations(
      customer_id, project_id, metric_id, first_day_index, last_day_index,
      parts, {}, 100, pagination_token);
  EXPECT_EQ(kOK, query_response.status);

  // Try to use a last_day_index < first_day_index. Expect kInvalidArguments.
  last_day_index = first_day_index - 1;
  query_response = this->observation_store_->QueryObservations(
      customer_id, project_id, metric_id, first_day_index, last_day_index,
      parts, {}, 100, "");
  EXPECT_EQ(kInvalidArguments, query_response.status);

  // Switch to last_day_index = first_day_index. Expect kOK.
  last_day_index = first_day_index;
  query_response = this->observation_store_->QueryObservations(
      customer_id, project_id, metric_id, first_day_index, last_day_index,
      parts, {}, 100, "");
  EXPECT_EQ(kOK, query_response.status);
}

REGISTER_TYPED_TEST_CASE_P(ObservationStoreAbstractTest, AddAndQuery,
                           QueryWithInvalidArguments);

}  // namespace store
}  // namespace analyzer
}  // namespace cobalt

#endif  // COBALT_ANALYZER_STORE_OBSERVATION_STORE_ABSTRACT_TEST_H_
