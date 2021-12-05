#include "kimera_dsg_builder/dsg_lcd_matching.h"

namespace kimera {
namespace lcd {

using Dsg = DynamicSceneGraph;
using DsgNode = DynamicSceneGraphNode;

float computeDistanceHist(const Descriptor& lhs,
                          const Descriptor& rhs,
                          const std::function<float(float, float)>& distance_func) {
  CHECK_EQ(lhs.values.rows(), rhs.values.rows());

  float score = 0.0;
  for (int r = 0; r < lhs.values.rows(); ++r) {
    // see https://ieeexplore.ieee.org/document/1641018 for derivation,
    // but this needs to match the BoW based distance
    if (lhs.values(r, 0) == 0.0 || rhs.values(r, 0) == 0.0) {
      continue;
    }

    score += distance_func(lhs.values(r, 0), rhs.values(r, 0));
  }

  return score;
}

float computeDistanceBow(const Descriptor& lhs,
                         const Descriptor& rhs,
                         const std::function<float(float, float)>& distance_func) {
  CHECK_EQ(lhs.values.rows(), lhs.words.rows());
  CHECK_EQ(rhs.values.rows(), rhs.words.rows());
  float score = 0.0;
  int r1 = 0;
  int r2 = 0;
  while (r1 < lhs.values.rows() && r2 < rhs.values.rows()) {
    const uint32_t word1 = lhs.words(r1, 0);
    const uint32_t word2 = rhs.words(r2, 0);

    if (word1 == word2) {
      score += distance_func(lhs.values(r1, 0), rhs.values(r2, 0));
      ++r1;
      ++r2;
    } else if (word1 < word2) {
      ++r1;
    } else {
      ++r2;
    }
  }

  return score;
}

float computeDistance(const Descriptor& lhs,
                      const Descriptor& rhs,
                      const std::function<float(float, float)>& distance_func) {
  if (!lhs.words.size() && !rhs.words.size()) {
    return computeDistanceHist(lhs, rhs, distance_func);
  } else {
    return computeDistanceBow(lhs, rhs, distance_func);
  }
}

float computeDescriptorScore(const Descriptor& lhs,
                             const Descriptor& rhs,
                             DescriptorScoreType type) {
  switch (type) {
    case DescriptorScoreType::COSINE:
      // map [-1, 1] to [0, 1]
      return 0.5f * computeCosineDistance(lhs, rhs) + 0.5f;
    case DescriptorScoreType::L1:
    default:
      // map [2, 0] to [0, 1]
      return 1.0f - 0.5f * computeL1Distance(lhs, rhs);
  }
}

LayerSearchResults searchDescriptors(
    const Descriptor& descriptor,
    const DescriptorMatchConfig& match_config,
    const std::set<NodeId>& valid_matches,
    const DescriptorCache& descriptors,
    const std::map<NodeId, std::set<NodeId>>& root_leaf_map,
    NodeId query_id) {
  float best_score = 0.0f;
  std::vector<std::pair<NodeId, float>> new_valid_match_scores;
  std::set<NodeId> new_valid_matches;
  for (const auto& valid_id : valid_matches) {
    if (root_leaf_map.at(valid_id).count(query_id)) {
      continue;
    }

    const Descriptor& other_descriptor = *descriptors.at(valid_id);
    std::chrono::duration<double> diff_s =
        descriptor.timestamp - other_descriptor.timestamp;
    if (diff_s.count() < match_config.min_time_separation_s) {
      continue;
    }

    const float curr_score =
        computeDescriptorScore(descriptor, other_descriptor, match_config.type);
    if (curr_score > best_score) {
      best_score = curr_score;
    }

    if (curr_score > match_config.min_score) {
      new_valid_matches.insert(valid_id);
      new_valid_match_scores.push_back({valid_id, curr_score});
    }
  }

  std::sort(
      new_valid_match_scores.begin(),
      new_valid_match_scores.end(),
      [](const std::pair<NodeId, float>& a, const std::pair<NodeId, float>& b) {
        return a.second > b.second;
      });
  std::vector<std::set<NodeId>> match_nodes;
  std::vector<NodeId> matches;
  std::vector<float> match_scores;
  for (const auto& id_score : new_valid_match_scores) {
    if (id_score.second < match_config.min_registration_score) {
      break;
    }

    if (id_score.second > best_score * match_config.min_score_ratio) {
      bool spatialy_distinct = true;
      for (const auto& m : matches) {
        if ((descriptors.at(id_score.first)->root_position -
             descriptors.at(m)->root_position)
                .norm() < match_config.min_match_separation_m) {
          spatialy_distinct = false;
          break;
        }
      }

      if (!spatialy_distinct) {
        continue;
      }

      match_nodes.push_back(descriptors.at(id_score.first)->nodes);
      matches.push_back(id_score.first);
      match_scores.push_back(id_score.second);
    }
    if (matches.size() == match_config.max_registration_matches) {
      break;
    }
  }

  return {match_scores,
          new_valid_matches,
          descriptor.nodes,
          match_nodes,
          descriptor.root_node,
          matches};
}

LayerSearchResults searchLeafDescriptors(const Descriptor& descriptor,
                                         const DescriptorMatchConfig& match_config,
                                         const std::set<NodeId>& valid_matches,
                                         const DescriptorCacheMap& leaf_cache_map,
                                         NodeId query_id) {
  float best_score = 0.0f;
  NodeId best_node = 0;  // gets overwritten on valid match
  NodeId best_root = 0;  // gets overwritten on valid match
  bool found_match = false;
  for (const auto& valid_id : valid_matches) {
    const DescriptorCache& leaf_cache = leaf_cache_map.at(valid_id);

    for (const auto& id_desc_pair : leaf_cache) {
      if (id_desc_pair.first == query_id) {
        continue;  // disallow self matches even if they probably can't happen
      }

      const Descriptor& other_descriptor = *id_desc_pair.second;
      std::chrono::duration<double> diff_s =
          descriptor.timestamp - other_descriptor.timestamp;
      if (diff_s.count() < match_config.min_time_separation_s) {
        continue;
      }

      const float curr_score =
          computeDescriptorScore(descriptor, other_descriptor, match_config.type);

      if (curr_score > best_score) {
        best_score = curr_score;
        best_node = id_desc_pair.first;
        best_root = other_descriptor.root_node;
        found_match = true;
      }
    }
  }

  if (!found_match) {
    return {{}, {}, {}, {}, descriptor.root_node, {}};
  }

  return {{best_score},
          {best_node},
          descriptor.nodes,
          {{best_node}},
          descriptor.root_node,
          {best_root}};
}

}  // namespace lcd
}  // namespace kimera
