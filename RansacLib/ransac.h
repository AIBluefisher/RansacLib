// Copyright (c) 2019, Torsten Sattler
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of Torsten Sattler nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// author: Torsten Sattler, torsten.sattler.de@googlemail.com

#ifndef RANSACLIB_RANSACLIB_RANSAC_H_
#define RANSACLIB_RANSACLIB_RANSAC_H_

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <RansacLib/sampling.h>
#include <RansacLib/utils.h>

namespace ransac_lib {

class RansacOptions {
 public:
  RansacOptions(): min_num_iterations_(100u), max_num_iterations_(10000u),
                   success_probability_(0.9999), squared_inlier_threshold_(1.0),
                   random_seed_(0u) {}
  uint32_t min_num_iterations_;
  uint32_t max_num_iterations_;
  double success_probability_;
  double squared_inlier_threshold_;
  unsigned int random_seed_;
};
  
// See Lebeda et al., Fixing the Locally Optimized RANSAC, BMVC, Table 1 for
// details on the variables.
class LORansacOptions : public RansacOptions {
 public:
  LORansacOptions(): num_lo_steps_(10), threshold_multiplier_(std::sqrt(2.0)),
                     num_lsq_iterations_(4),
                     min_sample_multiplicator_(7),
                     non_min_sample_multiplier_(3) {}
  int num_lo_steps_;
  double threshold_multiplier_;
  int num_lsq_iterations_;
  // The maximum number of data points used for least squares refinement is
  // min_sample_multiplicator_ * min_sample_size. Lebeda et al. recommend
  // setting min_sample_multiplicator_ to 7 (empirically determined for
  // epipolar geometry estimation.
  int min_sample_multiplicator_;
  // The solver needs to report the minimal size of the non-minimal sample
  // needed for its non-minimal solver. In practice, we draw a sample of size
  // min(non_min_sample_size * non_min_sample_multiplier_, N / 2), where N is
  // the number of data points.
  int non_min_sample_multiplier_;
};
  
struct RansacStatistics {
  uint32_t num_iterations;
  int best_num_inliers;
  double best_model_score;
  double inlier_ratio;
  std::vector<int> inlier_indices;
};

class RansacBase {
 protected:
  void ResetStatistics(RansacStatistics* statistics) const {
    RansacStatistics& stats = *statistics;
    stats.best_num_inliers = 0;
    stats.best_model_score = std::numeric_limits<double>::max();
    stats.num_iterations = 0u;
    stats.inlier_ratio = 0.0;
    stats.inlier_indices.clear();
  }
  
  // Computes the number of RANSAC iterations required for a given inlier
  // ratio, the probability of missing the best model, and sample size.
  // Assumes that min_iterations <= max_iterations.
  inline uint32_t NumRequiredIterations(const double inlier_ratio,
                                        const double prob_missing_best_model,
                                        const int sample_size,
                                        const uint32_t min_iterations,
                                        const uint32_t max_iterations) const {
    if (inlier_ratio <= 0.0) {
      return max_iterations;
    }
    if (inlier_ratio >= 1.0) {
      return min_iterations;
    }
    
    const double kProbNonInlierSample = 1.0 - std::pow(inlier_ratio,
                                       static_cast<double>(sample_size));
    const double kLogNumerator = std::log(prob_missing_best_model);
    const double kLogDenominator = std::log(kProbNonInlierSample);
    
    double num_iters = std::ceil(kLogNumerator / kLogDenominator + 0.5);
    uint32_t num_req_iterations =  std::min(static_cast<uint32_t>(num_iters),
                                            max_iterations);
    num_req_iterations = std::max(min_iterations, num_req_iterations);
    return num_req_iterations;
  }
};
  
// Implements LO-RANSAC with MSAC (top-hat) scoring, based on the description
// provided in [Lebeda, Matas, Chum, Fixing the Locally Optimized RANSAC, BMVC
// 2012]. Iteratively re-weighted least-squares optimization is optional.
template<class Model, class Solver>
class LocallyOptimizedMSAC : public RansacBase {
 public:
  // Estimates a model using a given solver. Notice that the solver contains
  // all data and is responsible to implement a non-minimal solver and
  // least-squares refinement. The latter two are optional, i.e., a dummy
  // implementation returning false is sufficient.
  // Returns the number of inliers.
  int EstimateModel(const LORansacOptions& options, const Solver& solver,
                    Model* best_model, RansacStatistics* statistics) const {
    ResetStatistics(statistics);
    RansacStatistics& stats = *statistics;
    
    // Sanity check: No need to run RANSAC if there are not enough data
    // points.
    const int kMinSampleSize = solver.min_sample_size();
    const int kNumData = solver.num_data();
    if (kMinSampleSize > kNumData || kMinSampleSize <= 0) {
      return 0;
    }
  
    // Initializes variables, etc.
    UniformSampling sampler(options.random_seed_, kNumData, kMinSampleSize);
    
    uint32_t max_num_iterations = std::max(options.max_num_iterations_,
                                           options.min_num_iterations_);

    const double kSqrInlierThresh = options.squared_inlier_threshold_;
    
    Model best_minimal_model;
    double best_min_model_score = std::numeric_limits<double>::max();
    
    std::vector<int> minimal_sample(kMinSampleSize);
    std::vector<Model> estimated_models;
    
    // Runs random sampling.
    for (stats.num_iterations = 0u; stats.num_iterations < max_num_iterations;
         ++stats.num_iterations) {
      sampler.Sample(&minimal_sample);
      
      // MinimalSolver returns the number of estimated models.
      const int kNumEstimatedModels = solver.MinimalSolver(minimal_sample,
                                                           &estimated_models);
      if (kNumEstimatedModels <= 0) continue;
      
      // Finds the best model among all estimated models.
      double best_local_score = std::numeric_limits<double>::max();
      int best_local_model_id = 0;
      GetBestEstimatedModelId(solver, estimated_models, kSqrInlierThresh,
                              &best_local_score, &best_local_model_id);
      
      // Updates the best model found so far.
      if (best_local_score < best_min_model_score) {
        // New best model (estimated from inliers found. Stores this model
        // and runs local optimization.
        best_min_model_score = best_local_score;
        best_minimal_model = estimated_models[best_local_model_id];
        
        // Performs local optimization. By construction, the local optimization
        // method returns the best model between all models found by local
        // optimization and the input model, i.e., score_refined_model <=
        // best_min_model_score holds.
        Model refined_model;
        double score_refined_model = std::numeric_limits<double>::max();
        LocalOptimization(options, solver, best_minimal_model,
                          best_min_model_score, &refined_model,
                          &score_refined_model);
        
        // Updates the best model.
        UpdateBestModel(score_refined_model, best_minimal_model,
                        &(stats.best_model_score), best_model);
        
        // Updates the number of RANSAC iterations.
        stats.best_num_inliers = GetInliers(solver, *best_model,
                                            kSqrInlierThresh,
                                            &(stats.inlier_indices));
        stats.inlier_ratio =
                        static_cast<double>(stats.best_num_inliers)
                                              /static_cast<double>(kNumData);
        max_num_iterations = NumRequiredIterations(
            stats.inlier_ratio, 1.0 - options.success_probability_,
            kMinSampleSize, options.min_num_iterations_,
            options.max_num_iterations_);
      }
    }
    
    return stats.best_num_inliers;
  }

 protected:
  void GetBestEstimatedModelId(const Solver& solver,
                               const std::vector<Model>& models,
                               const double squared_inlier_threshold,
                               double* best_score, int* best_model_id) const {
    *best_score = std::numeric_limits<double>::max();
    *best_model_id = 0;
    const int kNumEstimatedModels = static_cast<int>(models.size());
    for (int m = 0; m < kNumEstimatedModels; ++m) {
      double score = std::numeric_limits<double>::max();
      ScoreModel(solver, models[m], squared_inlier_threshold, &score);
      
      if (score < *best_score) {
        *best_score = score;
        *best_model_id = m;
      }
    }
  }

  void ScoreModel(const Solver& solver, const Model& model,
                  const double squared_inlier_threshold, double* score) const {
    const int kNumData = solver.num_data();
    *score = 0.0;
    for (int i = 0; i < kNumData; ++i) {
      double squared_error = solver.EvaluateModelOnPoint(model, i);
      *score += ComputeScore(squared_error, squared_inlier_threshold);
    }
  }
  
  // MSAC (top-hat) scoring function.
  inline double ComputeScore(const double squared_error,
                             const double squared_error_threshold) const {
    return std::min(squared_error, squared_error_threshold);
  }

  int GetInliers(const Solver& solver, const Model& model,
                 const double squared_inlier_threshold,
                 std::vector<int>* inliers) const {
    const int kNumData = solver.num_data();
    if (inliers == nullptr) {
      int num_inliers = 0;
      for (int i = 0; i < kNumData; ++i) {
        double squared_error = solver.EvaluateModelOnPoint(model, i);
        if (squared_error < squared_inlier_threshold) {
          ++num_inliers;
        }
      }
      return num_inliers;
    } else {
      inliers->clear();
      int num_inliers = 0;
      for (int i = 0; i < kNumData; ++i) {
        double squared_error = solver.EvaluateModelOnPoint(model, i);
        if (squared_error < squared_inlier_threshold) {
          ++num_inliers;
          inliers->push_back(i);
        }
      }
      return num_inliers;
    }
  }
    
  // See algorithms 2 and 3 in Lebeda et al.
  void LocalOptimization(const LORansacOptions& options, const Solver& solver,
                         const Model& best_minimal_model,
                         const double score_best_minimal_model,
                         Model* refined_model,
                         double* score_refined_model) const {
    *score_refined_model = score_best_minimal_model;
    *refined_model = best_minimal_model;
    
    const int kNumData = solver.num_data();
    // kMinNonMinSampleSize stores how many data points are required for a
    // non-minimal sample. For example, consider the case of pose estimation
    // for a calibrated camera. A minimal sample has size 3, while the
    // smallest non-minimal sample has size 4.
    const int kMinNonMinSampleSize = solver.non_minimal_sample_size();
    if (kMinNonMinSampleSize > kNumData) return;
    
    const double kSqInThresh = options.squared_inlier_threshold_;
    const double kThreshMult = options.threshold_multiplier_;
    
    // Performs an initial least squares fit of the best model found by the
    // minimal solver so far and then determines the inliers to that model
    // under a (slightly) relaxed inlier threshold.
    std::mt19937 rng;
    rng.seed(options.random_seed_);
    
    Model m_init = best_minimal_model;
    LeastSquaresFit(options, kSqInThresh * kThreshMult, solver, &rng, &m_init);
    
    double score = std::numeric_limits<double>::max();
    ScoreModel(solver, m_init, kSqInThresh, &score);
    UpdateBestModel(score, m_init, score_refined_model, refined_model);
    
    std::vector<int> inliers_base;
    int num_inliers_base = GetInliers(solver, m_init, kSqInThresh,
                                      &inliers_base);
    
    // Determines the size of the non-miminal samples drawn in each LO step.
    const int kNonMinSampleSize = std::max(kMinNonMinSampleSize, std::min(
                   kMinNonMinSampleSize * options.non_min_sample_multiplier_,
                                static_cast<int>(inliers_base.size()) / 2));
    
    // Performs the actual local optimization (LO).
    std::vector<int> sample;
    for (int r = 0; r < options.num_lo_steps_; ++r) {
      sample = inliers_base;
      utils::RandomShuffleAndResize(kNonMinSampleSize, &rng, &sample);
      
      Model m_non_min;
      if (!solver.NonMinimalSolver(sample, &m_non_min)) continue;
      
      ScoreModel(solver, m_non_min, kSqInThresh, &score);
      UpdateBestModel(score, m_non_min, score_refined_model, refined_model);
      
      // Iterative least squares refinement.
      LeastSquaresFit(options, kSqInThresh, solver, &rng, &m_non_min);
      
      // The current threshold multiplier and its update.
      double thresh = kThreshMult * kSqInThresh;
      double thresh_mult_update = (kThreshMult - 1.0) * kSqInThresh /
                            static_cast<int>(options.num_lsq_iterations_ - 1);
      for (int i = 0; i < options.num_lsq_iterations_; ++i) {
        LeastSquaresFit(options, thresh, solver, &rng, &m_non_min);
        
        ScoreModel(solver, m_non_min, kSqInThresh, &score);
        UpdateBestModel(score, m_non_min, score_refined_model, refined_model);
        thresh -= thresh_mult_update;
      }
    }
  }
  
  void LeastSquaresFit(const LORansacOptions& options, const double thresh,
                       const Solver& solver, std::mt19937* rng,
                       Model* model) const {
    const int kLSqSampleSize = options.min_sample_multiplicator_ *
                                                     solver.min_sample_size();
    std::vector<int> inliers;
    int num_inliers = GetInliers(solver, *model, thresh, &inliers);
    int lsq_data_size = std::min(kLSqSampleSize, num_inliers);
    utils::RandomShuffleAndResize(lsq_data_size, rng, &inliers);
    solver.LeastSquares(inliers, model);
  }
  
  inline void UpdateBestModel(const double score_curr, const Model& m_curr,
                              double* score_best, Model* m_best) const {
    if (score_curr < *score_best) {
      *score_best = score_curr;
      *m_best = m_curr;
    }
  }

  
};
  
}  // namespace ransac_lib

#endif  // RANSACLIB_RANSACLIB_RANSAC_H_