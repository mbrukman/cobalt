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

#ifndef COBALT_ANALYZER_STORE_BIGTABLE_NAMES_H_
#define COBALT_ANALYZER_STORE_BIGTABLE_NAMES_H_

#include <string>
#include <utility>

namespace cobalt {
namespace analyzer {
namespace store {

const char kDataColumnFamilyName[] = "data";
const char kObservationsTableId[] = "observations";
const char kReportsTableId[] = "reports";
const char kCloudBigtableUri[] = "bigtable.googleapis.com";
const char kCloudBigtableAdminUri[] = "bigtableadmin.googleapis.com";

class BigtableNames {
 public:
  static std::string TableParentName(const std::string& project_name,
                                     const std::string& instance_name) {
    return "projects/" + project_name + "/instances/" + instance_name;
  }

  static std::string ObservationsTableName(const std::string& project_name,
                                           const std::string& instance_name) {
    return FullTableName(project_name, instance_name, kObservationsTableId);
  }

  static std::string ReportsTableName(const std::string& project_name,
                                      const std::string& instance_name) {
    return FullTableName(project_name, instance_name, kReportsTableId);
  }

  static std::string FullTableName(const std::string& project_name,
                                   const std::string& instance_name,
                                   const std::string& table_id) {
    return TableParentName(project_name, instance_name) + "/tables/" + table_id;
  }
};

}  // namespace store
}  // namespace analyzer
}  // namespace cobalt

#endif  // COBALT_ANALYZER_STORE_BIGTABLE_NAMES_H_
