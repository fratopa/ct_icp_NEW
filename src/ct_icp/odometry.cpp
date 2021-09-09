#include <omp.h>
#include <chrono>
#include "odometry.hpp"
#include "Utilities/PersoTimer.h"

#define _USE_MATH_DEFINES
#include <math.h>

#ifdef CT_ICP_WITH_VIZ

#include <viz3d/engine.hpp>

#endif

namespace ct_icp {

    /* -------------------------------------------------------------------------------------------------------------- */
    OdometryOptions OdometryOptions::DefaultDrivingProfile() {
        return OdometryOptions{};
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    OdometryOptions OdometryOptions::DefaultRobustOutdoorLowInertia() {
        OdometryOptions default_options;
        default_options.voxel_size = 0.3;
        default_options.sample_voxel_size = 1.5;
        default_options.max_distance = 100.0;
        default_options.init_num_frames = 20;
        default_options.max_num_points_in_voxel = 20;
        default_options.min_distance_points = 0.05;
        default_options.distance_error_threshold = 5.0;
        default_options.motion_compensation = CONTINUOUS;
        default_options.initialization = INIT_NONE;
        default_options.robust_registration = true;
        default_options.debug_viz = false;

        default_options.robust_full_voxel_threshold = 0.5;
        default_options.robust_empty_voxel_threshold = 0.1;
        default_options.robust_num_attempts = 10;
        default_options.robust_max_voxel_neighborhood = 4;

        auto &ct_icp_options = default_options.ct_icp_options;
        ct_icp_options.size_voxel_map = 0.8;
        ct_icp_options.num_iters_icp = 10;
        ct_icp_options.threshold_voxel_occupancy = 5;
        ct_icp_options.min_number_neighbors = 20;
        ct_icp_options.voxel_neighborhood = 1;

        ct_icp_options.init_num_frames = 20;
        ct_icp_options.max_number_neighbors = 20;
        ct_icp_options.min_number_neighbors = 20;
        ct_icp_options.max_dist_to_plane_ct_icp = 0.5;
        ct_icp_options.norm_x_end_iteration_ct_icp = 0.0001;
        ct_icp_options.point_to_plane_with_distortion = true;
        ct_icp_options.distance = CT_POINT_TO_PLANE;
        ct_icp_options.num_closest_neighbors = 1;
        ct_icp_options.beta_constant_velocity = 0.0;
        ct_icp_options.beta_location_consistency = 0.001;
        ct_icp_options.beta_small_velocity = 0.01;
        ct_icp_options.loss_function = CAUCHY;
        ct_icp_options.solver = CERES;
        ct_icp_options.ls_max_num_iters = 20;
        ct_icp_options.ls_num_threads = 8;
        ct_icp_options.ls_sigma = 0.2;
        ct_icp_options.ls_tolerant_min_threshold = 0.05;

        return default_options;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    size_t Odometry::MapSize() const {
        return ::ct_icp::MapSize(voxel_map_);
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    void DistortFrame(std::vector<Point3D> &points, Eigen::Quaterniond &begin_quat, Eigen::Quaterniond &end_quat,
                      Eigen::Vector3d &begin_t, Eigen::Vector3d &end_t) {
        Eigen::Quaterniond end_quat_I = end_quat.inverse(); // Rotation of the inverse pose
        Eigen::Vector3d end_t_I = -1.0 * (end_quat_I * end_t); // Translation of the inverse pose
        for (auto &point: points) {
            double alpha_timestamp = point.alpha_timestamp;
            Eigen::Quaterniond q_alpha = begin_quat.slerp(alpha_timestamp, end_quat);
            q_alpha.normalize();
            Eigen::Matrix3d R = q_alpha.toRotationMatrix();
            Eigen::Vector3d t = (1.0 - alpha_timestamp) * begin_t + alpha_timestamp * end_t;

            // Distort Raw Keypoints
            point.raw_pt = end_quat_I * (q_alpha * point.raw_pt + t) + end_t_I;
        }
    }

    inline void TransformPoint(MOTION_COMPENSATION compensation, Point3D &point3D,
                               Eigen::Quaterniond &q_begin, Eigen::Quaterniond &q_end,
                               Eigen::Vector3d &t_begin, Eigen::Vector3d &t_end) {
        Eigen::Vector3d t;
        Eigen::Matrix3d R;
        double alpha_timestamp = point3D.alpha_timestamp;
        switch (compensation) {
            case MOTION_COMPENSATION::NONE:
            case MOTION_COMPENSATION::CONSTANT_VELOCITY:
                R = q_end.toRotationMatrix();
                t = t_end;
                break;
            case MOTION_COMPENSATION::CONTINUOUS:
            case MOTION_COMPENSATION::ITERATIVE:
                R = q_begin.slerp(alpha_timestamp, q_end).normalized().toRotationMatrix();
                t = (1.0 - alpha_timestamp) * t_begin + alpha_timestamp * t_end;
                break;
        }
        point3D.pt = R * point3D.raw_pt + t;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    Odometry::RegistrationSummary Odometry::RegisterFrameWithEstimate(const std::vector<Point3D> &frame,
                                                                      const TrajectoryFrame &initial_estimate) {
        auto frame_index = InitializeMotion(&initial_estimate);
        return DoRegister(frame, frame_index);
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    Odometry::RegistrationSummary Odometry::RegisterFrame(const std::vector<Point3D> &frame) {
        auto frame_index = InitializeMotion();
        return DoRegister(frame, frame_index);
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    int Odometry::InitializeMotion(const TrajectoryFrame *initial_estimate) {
        int index_frame = registered_frames_++;
        if (initial_estimate != nullptr) {
            // Insert previous estimate
            trajectory_.emplace_back(*initial_estimate);
            return index_frame;
        }

        // Initial Trajectory Estimate
        trajectory_.emplace_back(TrajectoryFrame());

        if (index_frame <= 1) {
            // Initialize first pose at Identity
            trajectory_[index_frame].begin_R = Eigen::MatrixXd::Identity(3, 3);
            trajectory_[index_frame].begin_t = Eigen::Vector3d(0., 0., 0.);
            trajectory_[index_frame].end_R = Eigen::MatrixXd::Identity(3, 3);
            trajectory_[index_frame].end_t = Eigen::Vector3d(0., 0., 0.);
        } else if (index_frame == 2) {
            if (options_.initialization == INIT_CONSTANT_VELOCITY) {
                // Different regimen for the second frame due to the bootstrapped elasticity
                Eigen::Matrix3d R_next_end =
                        trajectory_[index_frame - 1].end_R * trajectory_[index_frame - 2].end_R.inverse() *
                        trajectory_[index_frame - 1].end_R;
                Eigen::Vector3d t_next_end = trajectory_[index_frame - 1].end_t +
                                             trajectory_[index_frame - 1].end_R *
                                             trajectory_[index_frame - 2].end_R.inverse() *
                                             (trajectory_[index_frame - 1].end_t -
                                              trajectory_[index_frame - 2].end_t);

                trajectory_[index_frame].begin_R = trajectory_[index_frame - 1].end_R;
                trajectory_[index_frame].begin_t = trajectory_[index_frame - 1].end_t;
                trajectory_[index_frame].end_R = R_next_end;
                trajectory_[index_frame].end_t = t_next_end;
            } else {
                // Important ! Start with a rigid frame and let the ICP distort it !
                trajectory_[index_frame] = trajectory_[index_frame - 1];
                trajectory_[index_frame].end_t = trajectory_[index_frame].begin_t;
                trajectory_[index_frame].end_R = trajectory_[index_frame].begin_R;
            }
        } else {
            if (options_.initialization == INIT_CONSTANT_VELOCITY) {
                Eigen::Matrix3d R_next_begin =
                        trajectory_[index_frame - 1].begin_R * trajectory_[index_frame - 2].begin_R.inverse() *
                        trajectory_[index_frame - 1].begin_R;
                Eigen::Vector3d t_next_begin = trajectory_[index_frame - 1].begin_t +
                                               trajectory_[index_frame - 1].begin_R *
                                               trajectory_[index_frame - 2].begin_R.inverse() *
                                               (trajectory_[index_frame - 1].begin_t -
                                                trajectory_[index_frame - 2].begin_t);

                Eigen::Matrix3d R_next_end =
                        trajectory_[index_frame - 1].end_R * trajectory_[index_frame - 2].end_R.inverse() *
                        trajectory_[index_frame - 1].end_R;
                Eigen::Vector3d t_next_end = trajectory_[index_frame - 1].end_t +
                                             trajectory_[index_frame - 1].end_R *
                                             trajectory_[index_frame - 2].end_R.inverse() *
                                             (trajectory_[index_frame - 1].end_t -
                                              trajectory_[index_frame - 2].end_t);

                trajectory_[index_frame].begin_R = R_next_begin;; //trajectory_[index_frame - 1].end_R;
                trajectory_[index_frame].begin_t = t_next_begin;; //trajectory_[index_frame - 1].end_t;
                trajectory_[index_frame].end_R = R_next_end;
                trajectory_[index_frame].end_t = t_next_end;
            } else {
                trajectory_[index_frame] = trajectory_[index_frame - 1];
                // Important ! Start with a rigid frame and let the ICP distort it !
                trajectory_[index_frame] = trajectory_[index_frame - 1];
                trajectory_[index_frame].end_t = trajectory_[index_frame].begin_t;
                trajectory_[index_frame].end_R = trajectory_[index_frame].begin_R;
            }
        }
        return index_frame;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    std::vector<Point3D> Odometry::InitializeFrame(const std::vector<Point3D> &const_frame,
                                                   int index_frame) {

        /// PREPROCESS THE INITIAL FRAME
        double sample_size = index_frame < options_.init_num_frames ? options_.init_voxel_size : options_.voxel_size;
        std::vector<Point3D> frame(const_frame);

        std::mt19937_64 g;
        std::shuffle(frame.begin(), frame.end(), g);
        //Subsample the scan with voxels taking one random in every voxel
        sub_sample_frame(frame, sample_size);

        // No elastic ICP for first frame because no initialization of ego-motion
        if (index_frame == 1) {
            for (auto &point3D: frame) {
                point3D.alpha_timestamp = 1.0;
            }
        }

        if (index_frame > 1) {
            if (options_.motion_compensation == CONSTANT_VELOCITY) {
                // The motion compensation of Constant velocity modifies the raw points of the point cloud
                auto &tr_frame = trajectory_[index_frame];
                Eigen::Quaterniond begin_quat(tr_frame.begin_R);
                Eigen::Quaterniond end_quat(tr_frame.end_R);
                DistortFrame(frame, begin_quat, end_quat, tr_frame.begin_t, tr_frame.end_t);
            }

            auto q_begin = Eigen::Quaterniond(trajectory_[index_frame].begin_R);
            auto q_end = Eigen::Quaterniond(trajectory_[index_frame].end_R);
            Eigen::Vector3d t_begin = trajectory_[index_frame].begin_t;
            Eigen::Vector3d t_end = trajectory_[index_frame].end_t;
            for (auto &point3D: frame) {
                TransformPoint(options_.motion_compensation, point3D, q_begin, q_end, t_begin, t_end);
            }
        }

        for (auto &point: frame)
            point.index_frame = index_frame;

        return frame;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    Odometry::RegistrationSummary Odometry::DoRegister(const std::vector<Point3D> &const_frame,
                                                       int index_frame) {
        auto start = std::chrono::steady_clock::now();
        auto &log_out = std::cout;
        const bool kDisplay = options_.debug_print;
        CTICPOptions ct_icp_options = options_.ct_icp_options; // Make a copy of the options
        const double kSizeVoxelInitSample = options_.voxel_size;

        const double kSizeVoxelMap = options_.ct_icp_options.size_voxel_map;
        const double kMinDistancePoints = options_.min_distance_points;
        const int kMaxNumPointsInVoxel = options_.max_num_points_in_voxel;

        if (kDisplay) {
            log_out << "/* ------------------------------------------------------------------------ */" << std::endl;
            log_out << "/* ------------------------------------------------------------------------ */" << std::endl;
            log_out << "REGISTRATION OF FRAME number " << index_frame <<
                    " with " << (options_.ct_icp_options.solver == CERES ? "CERES" : "GN") << " solver" << std::endl;
        }

        auto frame = InitializeFrame(const_frame, index_frame);
        if (kDisplay)
            log_out << "Number of points in sub-sampled frame: " << frame.size() << " / " << const_frame.size()
                    << std::endl;
        if (index_frame > 0) {
            Eigen::Vector3d t_diff = trajectory_[index_frame].end_t - trajectory_[index_frame].begin_t;
            if (kDisplay)
                log_out << "Initial ego-motion distance: " << t_diff.norm() << std::endl;
        }

        const auto initial_estimate = trajectory_.back();
        RegistrationSummary summary;
        summary.frame = initial_estimate;
        auto previous_frame = initial_estimate;
        if (index_frame > 0) {
            bool success = false;
            summary.number_of_attempts = 1;
            double sample_voxel_size = index_frame < options_.init_num_frames ?
                                       options_.init_sample_voxel_size : options_.sample_voxel_size;
            double min_voxel_size = std::min(options_.init_voxel_size, options_.voxel_size);
            do {
                auto start_ct_icp = std::chrono::steady_clock::now();
                TryRegister(frame, index_frame, ct_icp_options, summary, sample_voxel_size);
                auto end_ct_icp = std::chrono::steady_clock::now();
                std::chrono::duration<double> elapsed_icp = (end_ct_icp - start);
                if (kDisplay)
                    log_out << "Elapsed Elastic_ICP: " << (elapsed_icp.count()) * 1000.0 << std::endl;

                // Compute Modification of trajectory
                if (index_frame > 0) {
                    summary.distance_correction = (trajectory_[index_frame].begin_t -
                                                   trajectory_[index_frame - 1].end_t).norm();

                    Eigen::Matrix3d delta_R = (trajectory_[index_frame - 1].end_R *
                                               trajectory_[index_frame].begin_R.inverse());
                    summary.relative_orientation = AngularDistance(trajectory_[index_frame - 1].end_R,
                                                                   trajectory_[index_frame].end_R);
                    summary.ego_orientation = summary.frame.EgoAngularDistance();

                }
                summary.relative_distance = (trajectory_[index_frame].end_t - trajectory_[index_frame].begin_t).norm();

                success = AssessRegistration(summary.keypoints, summary, kDisplay ? &log_out : nullptr);
                if (options_.robust_fail_early)
                    summary.success = success;
                if (!success) {
                    // Either fail or
                    if (kDisplay)
                        log_out << "Registration Attempt n°" << summary.number_of_attempts << " failed with message: "
                                << summary.error_message << std::endl;
                    if (options_.robust_registration && summary.number_of_attempts < options_.robust_num_attempts) {
                        double trans_distance = previous_frame.TranslationDistance(summary.frame);
                        double rot_distance = previous_frame.RotationDistance(summary.frame);
                        if (kDisplay)
                            log_out << "Distance to previous trans : " << trans_distance <<
                                    " rot distance " << rot_distance << std::endl;

                        if (previous_frame.TranslationDistance(summary.frame) < 1.e-2 &&
                            previous_frame.RotationDistance(summary.frame) < 1.e-3) {
                            // Do not waste time for no reward
                            break;
                        }
                        previous_frame = summary.frame;
                        // Handle the failure cases
                        // trajectory_[index_frame] = initial_estimate;
                        ct_icp_options.threshold_voxel_occupancy = std::min(
                                ct_icp_options.threshold_voxel_occupancy + 5,
                                options_.max_num_points_in_voxel);
                        ct_icp_options.voxel_neighborhood = std::min(++ct_icp_options.voxel_neighborhood,
                                                                     options_.robust_max_voxel_neighborhood);
                        ct_icp_options.ls_max_num_iters += 30;
                        ct_icp_options.num_iters_icp = min(ct_icp_options.num_iters_icp + 20, 50);
                        ct_icp_options.norm_x_end_iteration_ct_icp = max(
                                ct_icp_options.norm_x_end_iteration_ct_icp / 10, 1.e-5);
                        sample_voxel_size = std::max(sample_voxel_size / 1.5, min_voxel_size);

                        summary.number_of_attempts++;
                    } else {
                        success = true;
                    }
                }
            } while (!success);

            if (!summary.success)
                return summary;
        }

        if (kDisplay) {
            if (index_frame > 0) {
                log_out << "Trajectory correction [begin(t) - end(t-1)]: "
                        << summary.distance_correction << std::endl;
                log_out << "Final ego-motion distance: " << summary.relative_distance << std::endl;
            }
        }

        if (index_frame == 0) {
            voxel_map_.clear();
        }

        bool add_points = true;

        if (options_.robust_registration) {
            if (kDisplay)
                log_out << "[Robust Registration] The rotation ego motion is "
                        << summary.ego_orientation << " (deg)/ " << " relative orientation "
                        << summary.relative_orientation << " (deg) " << std::endl;
            if (summary.ego_orientation > options_.robust_threshold_ego_orientation ||
                summary.relative_orientation > options_.robust_threshold_relative_orientation) {
                if (kDisplay)
                    log_out << "[Robust Registration] Change in orientation too important. "
                               "Points will not be added." << std::endl;
                add_points = false;
            }
        }

        if (add_points) {
            //Update Voxel Map+
            AddPointsToMap(voxel_map_, frame, kSizeVoxelMap,
                           kMaxNumPointsInVoxel, kMinDistancePoints);
        }


#ifdef CT_ICP_WITH_VIZ
        if (options_.debug_viz) {

            auto &instance = viz::ExplorationEngine::Instance();
            auto model_ptr = std::make_shared<viz::PointCloudModel>();
            auto &model_data = model_ptr->ModelData();
            model_data.xyz.reserve(MapSize());
            for (auto &voxel: voxel_map_) {
                for (int i(0); i < voxel.second.NumPoints(); ++i)
                    model_data.xyz.push_back(voxel.second.points[i].cast<float>());
            }
            model_data.point_size = 1;
            instance.AddModel(-3, model_ptr);
        }
#endif


        // Remove voxels too far from actual position of the vehicule
        const double kMaxDistance = options_.max_distance;
        const Eigen::Vector3d location = trajectory_[index_frame].end_t;
        RemovePointsFarFromLocation(voxel_map_, location, kMaxDistance);


        if (kDisplay) {
            log_out << "Average Load Factor (Map): " << voxel_map_.load_factor() << std::endl;
            log_out << "Number of Buckets (Map): " << voxel_map_.bucket_count() << std::endl;
            log_out << "Number of points (Map): " << MapSize() << std::endl;
        }

        auto end = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        if (kDisplay) {
            log_out << "Elapsed Time: " << elapsed_seconds.count() * 1000.0 << " (ms)" << std::endl;
        }


        summary.corrected_points = frame;
        summary.all_corrected_points = const_frame;

        Eigen::Quaterniond q_begin(summary.frame.begin_R);
        Eigen::Quaterniond q_end(summary.frame.end_R);

        for (auto &point3D: summary.all_corrected_points) {
            double timestamp = point3D.alpha_timestamp;
            Eigen::Quaterniond slerp = q_begin.slerp(timestamp, q_end).normalized();
            point3D.pt = slerp.toRotationMatrix() * point3D.raw_pt +
                         summary.frame.begin_t * (1.0 - timestamp) + timestamp * summary.frame.end_t;
        }

        return summary;
    }


    /* -------------------------------------------------------------------------------------------------------------- */
    Odometry::RegistrationSummary Odometry::TryRegister(vector<Point3D> &frame, int index_frame,
                                                        const CTICPOptions &options,
                                                        RegistrationSummary &summary,
                                                        double sample_voxel_size) {
        // Use new sub_sample frame as keypoints
        std::vector<Point3D> keypoints;
        grid_sampling(frame, keypoints, sample_voxel_size);

        auto num_keypoints = (int) keypoints.size();
        summary.sample_size = num_keypoints;

        int number_keypoints_used = 0;
        {

            bool success = false;
            //CT ICP
            if (options_.ct_icp_options.solver == CT_ICP_SOLVER::GN) {
                success = CT_ICP_GN(options, voxel_map_, keypoints, trajectory_, index_frame);
            } else {
                success = CT_ICP_CERES(options, voxel_map_, keypoints, trajectory_, index_frame);
            }

            if (!success) {
                summary.success = false;
                return summary;
            }

            //Update frame
            Eigen::Quaterniond q_begin = Eigen::Quaterniond(trajectory_[index_frame].begin_R);
            Eigen::Quaterniond q_end = Eigen::Quaterniond(trajectory_[index_frame].end_R);
            Eigen::Vector3d t_begin = trajectory_[index_frame].begin_t;
            Eigen::Vector3d t_end = trajectory_[index_frame].end_t;
            for (auto &point: frame) {
                // Modifies the world point of the frame based on the raw_pt
                TransformPoint(options_.motion_compensation, point, q_begin, q_end, t_begin, t_end);
            }
        }
        summary.keypoints = keypoints;
        summary.number_keypoints = number_keypoints_used;
        summary.frame = trajectory_[index_frame];
        return summary;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    bool Odometry::AssessRegistration(const vector<Point3D> &points,
                                      RegistrationSummary &summary, std::ostream *log_stream) const {

        bool success = summary.success;
        if (summary.relative_orientation > options_.robust_threshold_relative_orientation ||
            summary.ego_orientation > options_.robust_threshold_ego_orientation) {
            if (summary.number_of_attempts < options_.robust_num_attempts_when_rotation) {
                summary.error_message = "Large rotations require at least " +
                                        std::to_string(options_.robust_num_attempts_when_rotation) +
                                        " attempts. Got " + std::to_string(summary.number_of_attempts);
                return false;
            }

        }


        if (summary.relative_distance > options_.robust_relative_trans_threshold) {
            summary.error_message = "The relative distance is too important";
            return false;
        }

        // Only do neighbor assessment if enough motion
        bool do_neighbor_assessment = summary.distance_correction > 0.1;
        do_neighbor_assessment |= summary.relative_distance > options_.robust_neighborhood_min_dist;
        do_neighbor_assessment |= summary.relative_orientation > options_.robust_neighborhood_min_orientation;

        if (do_neighbor_assessment && registered_frames_ > options_.init_num_frames) {
            if (options_.robust_registration) {
                const double kSizeVoxelMap = options_.ct_icp_options.size_voxel_map;
                Voxel voxel;
                double ratio_empty_voxel = 0;
                double ratio_half_full_voxel = 0;

                for (auto &point: points) {
                    voxel = Voxel::Coordinates(point.pt, kSizeVoxelMap);
                    if (voxel_map_.find(voxel) == voxel_map_.end())
                        ratio_empty_voxel += 1;
                    if (voxel_map_.find(voxel) != voxel_map_.end() &&
                        voxel_map_.at(voxel).NumPoints() > options_.max_num_points_in_voxel / 2) {
                        // Only count voxels which have at least
                        ratio_half_full_voxel += 1;
                    }
                }

                ratio_empty_voxel /= points.size();
                ratio_half_full_voxel /= points.size();

                if (*log_stream)
                    *log_stream << "[Quality Assessment] Keypoint Ratio of voxel half occupied: " <<
                                ratio_half_full_voxel << std::endl
                                << "[Quality Assessment] Keypoint Ratio of empty voxel " <<
                                ratio_empty_voxel << std::endl;
                if (ratio_half_full_voxel < options_.robust_full_voxel_threshold ||
                    ratio_empty_voxel > options_.robust_empty_voxel_threshold) {
                    success = false;
                    if (ratio_empty_voxel > options_.robust_empty_voxel_threshold)
                        summary.error_message = "[Odometry::AssessRegistration] Ratio of empty voxels " +
                                                std::to_string(ratio_empty_voxel) + "above threshold.";
                    else
                        summary.error_message = "[Odometry::AssessRegistration] Ratio of half full voxels " +
                                                std::to_string(ratio_half_full_voxel) + "below threshold.";

                }
            }
        }

        if (summary.relative_distance > options_.distance_error_threshold) {
            if (log_stream != nullptr)
                *log_stream << "Error in ego-motion distance !" << std::endl;
            return false;
        }

        return success;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    std::vector<TrajectoryFrame> Odometry::Trajectory() const {
        return trajectory_;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    ArrayVector3d Odometry::GetLocalMap() const {
        return MapAsPointcloud(voxel_map_);
    }


    /* -------------------------------------------------------------------------------------------------------------- */
    ArrayVector3d MapAsPointcloud(const VoxelHashMap &map) {
        ArrayVector3d points;
        points.reserve(MapSize(map));
        for (auto &voxel: map) {
            for (int i(0); i < voxel.second.NumPoints(); ++i)
                points.push_back(voxel.second.points[i]);
        }
        return points;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    size_t MapSize(const VoxelHashMap &map) {
        size_t map_size(0);
        for (auto &itr_voxel_map: map) {
            map_size += (itr_voxel_map.second).NumPoints();
        }
        return map_size;
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    void RemovePointsFarFromLocation(VoxelHashMap &map, const Eigen::Vector3d &location, double distance) {
        std::vector<Voxel> voxels_to_erase;
        for (auto &pair: map) {
            Eigen::Vector3d pt = pair.second.points[0];
            if ((pt - location).squaredNorm() > (distance * distance)) {
                voxels_to_erase.push_back(pair.first);
            }
        }
        for (auto &vox: voxels_to_erase)
            map.erase(vox);

    }

    /* -------------------------------------------------------------------------------------------------------------- */
    inline void AddPointToMap(VoxelHashMap &map, const Eigen::Vector3d &point, double voxel_size,
                              int max_num_points_in_voxel, double min_distance_points, int min_num_points = 0) {
        short kx = static_cast<short>(point[0] / voxel_size);
        short ky = static_cast<short>(point[1] / voxel_size);
        short kz = static_cast<short>(point[2] / voxel_size);

        VoxelHashMap::iterator search = map.find(Voxel(kx, ky, kz));
        if (search != map.end()) {
            auto &voxel_block = (search.value());

            if (!voxel_block.IsFull()) {
                double sq_dist_min_to_points = 10 * voxel_size * voxel_size;
                for (int i(0); i < voxel_block.NumPoints(); ++i) {
                    auto &_point = voxel_block.points[i];
                    double sq_dist = (_point - point).squaredNorm();
                    if (sq_dist < sq_dist_min_to_points) {
                        sq_dist_min_to_points = sq_dist;
                    }
                }
                if (sq_dist_min_to_points > (min_distance_points * min_distance_points)) {
                    if (min_num_points <= 0 || voxel_block.NumPoints() >= min_num_points) {
                        voxel_block.AddPoint(point);
                    }
                }
            }
        } else {
            if (min_num_points <= 0) {
                // Do not add points (avoids polluting the map)
                VoxelBlock block(max_num_points_in_voxel);
                block.AddPoint(point);
                map[Voxel(kx, ky, kz)] = std::move(block);
            }

        }

    }

    /* -------------------------------------------------------------------------------------------------------------- */
    void AddPointsToMap(VoxelHashMap &map, const vector<Point3D> &points, double voxel_size,
                        int max_num_points_in_voxel, double min_distance_points, int min_num_points) {
        //Update Voxel Map
        for (const auto &point: points) {
            AddPointToMap(map, point.pt, voxel_size, max_num_points_in_voxel, min_distance_points, min_num_points);
        }
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    void AddPointsToMap(VoxelHashMap &map, const ArrayVector3d &points, double voxel_size,
                        int max_num_points_in_voxel, double min_distance_points) {
        for (const auto &point: points)
            AddPointToMap(map, point, voxel_size, max_num_points_in_voxel, min_distance_points);
    }
    /* -------------------------------------------------------------------------------------------------------------- */


} // namespace ct_icp