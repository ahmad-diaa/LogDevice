/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/common/configuration/nodes/NodesConfiguration.h"

namespace facebook {
  namespace logdevice {
    namespace NodesConfigurationTestUtil {

extern const configuration::nodes::NodeServiceDiscovery::RoleSet seq_role;
extern const configuration::nodes::NodeServiceDiscovery::RoleSet storage_role;
extern const configuration::nodes::NodeServiceDiscovery::RoleSet both_role;

extern const membership::MaintenanceID::Type DUMMY_MAINTENANCE;

struct NodeTemplate {
  node_index_t id;
  configuration::nodes::NodeServiceDiscovery::RoleSet roles{both_role};
  std::string location{};
  double sequencer_weight{1.0};
  shard_size_t num_shards{2};
  bool metadata_node{false};
};

configuration::nodes::NodeServiceDiscovery
genDiscovery(node_index_t n,
             configuration::nodes::NodeServiceDiscovery::RoleSet roles,
             std::string location);

configuration::nodes::NodesConfiguration::Update
initialProvisionUpdate(std::vector<node_index_t> node_idxs);

configuration::nodes::NodesConfiguration::Update
initialProvisionUpdate(std::vector<NodeTemplate> nodes,
                       ReplicationProperty metadata_rep = ReplicationProperty{
                           {NodeLocationScope::NODE, 2}});

std::shared_ptr<const configuration::nodes::NodesConfiguration> provisionNodes(
    configuration::nodes::NodesConfiguration::Update provision_update);

configuration::nodes::NodesConfiguration::Update
addNewNodeUpdate(const configuration::nodes::NodesConfiguration& existing,
                 NodeTemplate node);

// provision a specific LD nodes config with:
// 1) nodes N1, N2, N7, N9, N11, N13
// 2) N1 and N7 have sequencer role; N1, N2, N9, N11, N13 have storage role;
// 3) N2 and N9 are metadata storage nodes, metadata logs replicaton is
//    (rack, 2)
configuration::nodes::NodesConfiguration::Update initialProvisionUpdate();
std::shared_ptr<const configuration::nodes::NodesConfiguration>
provisionNodes();

// provision new nodes N17
configuration::nodes::NodesConfiguration::Update
addNewNodeUpdate(const configuration::nodes::NodesConfiguration& existing);

// NOTE: the base_version below should be storage membership versions
// inside the NC instead of the NC versions.

// start enabling read on N17
configuration::nodes::NodesConfiguration::Update
enablingReadUpdate(membership::MembershipVersion::Type base_version);

// start disabling writes on N11 and N13
configuration::nodes::NodesConfiguration::Update
disablingWriteUpdate(membership::MembershipVersion::Type base_version);

}}} // namespace facebook::logdevice::NodesConfigurationTestUtil
