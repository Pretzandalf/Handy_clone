#include "calibration.h"
#include "params.h"

#include <deque>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

/**
 * Converts Boost point to ROS2 message
 *
 * @param boost_point Boost point that will be converted
 * @return ROS2 Point message
 */
geometry_msgs::msg::Point toMsgPoint(const handy::calibration::Point& boost_point) {
    geometry_msgs::msg::Point point;
    point.x = boost_point.x();
    point.y = boost_point.y();
    return point;
}

}  // namespace
namespace handy::calibration {
/**
 * Converts all corners of detected chessboard into a convex hull
 *
 * @param corners vector of OpenCV points that should be covered by a convex hull
 * @return convex hull as a boost polygon
 */
Polygon convertToBoostPolygon(const std::vector<cv::Point2f>& corners) {
    Polygon poly;
    Polygon hull;
    for (size_t i = 0; i < corners.size(); ++i) {
        poly.outer().emplace_back(corners[i].x, corners[i].y);
    }
    boost::geometry::convex_hull(poly, hull);
    return hull;
}

/**
 * Converts all detected charuco corners into a convex hull polyline
 *
 * @param detected_corners vector of OpenCV points that should be covered by a convex hull
 * @return vector of markers to display
 */
std::vector<geometry_msgs::msg::Point> toBoardMsgCorners(
    const std::vector<cv::Point2f>& detected_corners) {
    Polygon poly;
    Polygon hull;
    for (size_t i = 0; i < detected_corners.size(); ++i) {
        poly.outer().emplace_back(detected_corners[i].x, detected_corners[i].y);
    }
    boost::geometry::convex_hull(poly, hull);

    std::vector<geometry_msgs::msg::Point> marker_points;
    for (size_t i = 0; i < hull.outer().size(); ++i) {
        marker_points.push_back(toMsgPoint(hull.outer()[i]));
    }
    return marker_points;
}

/**
 * Calculates intersection over union metric as a measure of similarity between two polygons
 *
 * @param first Boost polygon
 * @param second Boost polygon
 * @return the metric value
 */

double getIou(const Polygon& first, const Polygon& second) {
    std::deque<Polygon> inter_poly;
    boost::geometry::intersection(first, second, inter_poly);

    double iou = 0.0;
    if (inter_poly.size() > 1) {
        throw std::logic_error("Intersection is not valid");
    }
    if (!inter_poly.empty()) {
        double inter_area = boost::geometry::area(inter_poly.front());
        iou = inter_area /
              (boost::geometry::area(first) + boost::geometry::area(second) - inter_area);
    }
    return iou;
}

sensor_msgs::msg::CompressedImage toJpegMsg(const cv_bridge::CvImage& cv_image) {
    sensor_msgs::msg::CompressedImage result;
    cv_image.toCompressedImageMsg(result, cv_bridge::Format::JPEG);
    return result;
}

CalibrationNode::CalibrationNode() : Node("calibration_node") {
    RCLCPP_INFO_STREAM(this->get_logger(), "Init started");

    declareLaunchParams();
    RCLCPP_INFO_STREAM(this->get_logger(), "Launch params declared");

    initSignals();
    RCLCPP_INFO_STREAM(this->get_logger(), "Signals initialised");

    cv::Size pattern_size{7, 10};
    for (int i = 0; i < pattern_size.height; ++i) {
        for (int j = 0; j < pattern_size.width; ++j) {
            param_.square_obj_points.emplace_back(i, j, 0);
        }
    }

    timer_.calibration_state = this->create_wall_timer(
        std::chrono::milliseconds(250), [this] { publishCalibrationState(); });

    cv::aruco::Dictionary dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_6X6_250);
    param_.charuco_board = cv::aruco::CharucoBoard(pattern_size, 0.04f, 0.02f, dictionary);
    param_.charuco_board.setLegacyPattern(true);

    cv::aruco::DetectorParameters detector_params;
    detector_params.cornerRefinementMethod = cv::aruco::CORNER_REFINE_APRILTAG;
    detector_params.cornerRefinementWinSize = 10;
    cv::aruco::CharucoParameters board_params;

    charuco_detector_ = std::make_unique<cv::aruco::CharucoDetector>(
        param_.charuco_board, board_params, detector_params);

    for (size_t i = 0; i < param_.cameras_to_calibrate.size(); ++i) {
        signal_.detected_boards[i]->publish(state_.board_markers_array[i]);
    }
}

void CalibrationNode::declareLaunchParams() {
    // clang-format off
    param_.publish_preview_markers = this->declare_parameter<bool>("publish_preview_markers", true);
    param_.path_to_save_params = this->declare_parameter<std::string>("calibration_file_path", "param_path");

    param_.required_board_coverage = this->declare_parameter<double>("required_board_coverage", 0.7);
    param_.min_accepted_error = this->declare_parameter<double>("min_accepted_calib_error", 0.75);
    param_.iou_threshold = this->declare_parameter<double>("iou_threshold", 0.5);
    param_.marker_color=this->declare_parameter<std::vector<double>>("marker_color", {0.0f, 1.0f, 0.0f, 0.12f});

    param_.cameras_to_calibrate = this->declare_parameter<std::vector<int64_t>>("cameras_to_calibrate", {1, 2});
    // clang-format on

    // no more than 2 camera are supported at the moment
    if (param_.cameras_to_calibrate.empty() || param_.cameras_to_calibrate.size() <= 2) {
        RCLCPP_ERROR(
            this->get_logger(),
            "Provided %ld cameras instead of 1 or 2",
            param_.cameras_to_calibrate.size());
    }

    for (size_t i = 0; i < param_.cameras_to_calibrate.size(); i++) {
        param_.id_to_idx[param_.cameras_to_calibrate[i]] = i;
    }
}

void CalibrationNode::initSignals() {
    for (size_t i = 0; i < param_.cameras_to_calibrate.size(); ++i) {
        const std::string calib_name_base =
            "/calibraton_" + std::to_string(param_.cameras_to_calibrate[i]);
        auto callback = [this, i](const sensor_msgs::msg::CompressedImage::ConstSharedPtr& msg) {
            handleFrame(msg, i);
        };
        slot_.image_sub.push_back(this->create_subscription<sensor_msgs::msg::CompressedImage>(
            calib_name_base + "/bgr/image", 10, callback));

        signal_.detected_boards.push_back(
            this->create_publisher<foxglove_msgs::msg::ImageMarkerArray>(
                calib_name_base + "/board_markers", 10));
        signal_.detected_corners.push_back(
            this->create_publisher<foxglove_msgs::msg::ImageMarkerArray>(
                calib_name_base + "/corner_markers", 10));
    }
    service_.button_service = this->create_service<camera_srvs::srv::CalibrationCommand>(
        "/calibration/button",
        std::bind(
            &CalibrationNode::onButtonClick, this, std::placeholders::_1, std::placeholders::_2));

    signal_.calibration_state =
        this->create_publisher<std_msgs::msg::Int16>("/calibration/state", 10);
}

void CalibrationNode::handleFrame(
    const sensor_msgs::msg::CompressedImage::ConstSharedPtr& msg, size_t camera_idx) {
    if (state_.global_calibration_state != kCapturing) {
        return;
    }

    cv_bridge::CvImagePtr image_ptr = cv_bridge::toCvCopy(msg, "bgr8");
    if (!state_.frame_size) {
        state_.frame_size =
            std::make_optional<cv::Size>(image_ptr->image.cols, image_ptr->image.rows);
    }
    if (state_.frame_size->width != image_ptr->image.cols ||
        state_.frame_size->height != image_ptr->image.rows) {
        RCLCPP_ERROR(
            this->get_logger(),
            "Camera ID=%ld provided (%d, %d), expected (%d, %d)",
            param_.cameras_to_calibrate[camera_idx],
            image_ptr->image.cols,
            image_ptr->image.rows,
            state_.frame_size->width,
            state_.frame_size->height);
    }

    std::vector<int> current_ids;
    std::vector<cv::Point2f> current_corners;
    std::vector<cv::Point2f> current_image_points;
    std::vector<cv::Point3f> current_obj_points;

    charuco_detector_->detectBoard(image_ptr->image, current_corners, current_ids);

    if (current_corners.size() < 20) {
        state_.board_corners_array[camera_idx].markers.clear();
        signal_.detected_corners[camera_idx]->publish(state_.board_corners_array[camera_idx]);
        return;
    }
    param_.charuco_board.matchImagePoints(
        current_corners, current_ids, current_obj_points, current_image_points);

    state_.board_corners_array[camera_idx].markers.clear();
    if (current_ids.size() < 30) {
        signal_.detected_corners[camera_idx]->publish(state_.board_corners_array[camera_idx]);
        return;
    }
    if (param_.publish_preview_markers) {
        appendCornerMarkers(current_corners, camera_idx);
        signal_.detected_corners[camera_idx]->publish(state_.board_corners_array[camera_idx]);
    }
    if (!checkMaxSimilarity(current_corners, camera_idx)) {
        return;
    }
    if (param_.publish_preview_markers) {
        state_.board_markers_array[camera_idx].markers.push_back(
            getBoardMarkerFromCorners(current_corners, image_ptr->header));
        signal_.detected_boards[camera_idx]->publish(state_.board_markers_array[camera_idx]);
    }

    state_.obj_points_all[camera_idx].push_back(current_obj_points);
    state_.image_points_all[camera_idx].push_back(current_image_points);
    state_.polygons_all[camera_idx].push_back(convertToBoostPolygon(current_corners));
}

void CalibrationNode::onButtonClick(
    const camera_srvs::srv::CalibrationCommand::Request::SharedPtr& request,
    const camera_srvs::srv::CalibrationCommand::Response::SharedPtr& /*response*/) {
    if (state_.global_calibration_state == kStereoCalibrating ||
        state_.global_calibration_state == kMonoCalibrating) {
        return;
    }
    if (request->cmd == kStart) {
        state_.global_calibration_state = kCapturing;
        RCLCPP_INFO_STREAM(this->get_logger(), "State set to capturing");

    } else if (request->cmd == kCalibrate) {
        for (size_t i = 0; i < param_.cameras_to_calibrate.size(); ++i) {
            calibrate(i);
        }
    } else if (request->cmd == kReset) {
        handleResetCommand();
    }
}

bool CalibrationNode::checkMaxSimilarity(
    std::vector<cv::Point2f>& corners, size_t camera_idx) const {
    Polygon new_poly = convertToBoostPolygon(corners);
    return std::all_of(
        state_.polygons_all[camera_idx].begin(),
        state_.polygons_all[camera_idx].end(),
        [&](const Polygon& prev_poly) {
            return getIou(prev_poly, new_poly) <= param_.iou_threshold;
        });
}

int CalibrationNode::getImageCoverage(size_t camera_idx) const {
    MultiPolygon current_coverage;
    for (const Polygon& new_poly : state_.polygons_all[camera_idx]) {
        MultiPolygon tmp_poly;
        boost::geometry::union_(current_coverage, new_poly, tmp_poly);
        current_coverage = tmp_poly;
    }
    float ratio = boost::geometry::area(current_coverage) /
                  (state_.frame_size->height * state_.frame_size->width);
    return static_cast<int>(ratio * 100);
}

void CalibrationNode::publishCalibrationState() const {
    std_msgs::msg::Int16 msg;
    msg.data = state_.global_calibration_state;
    signal_.calibration_state->publish(msg);
}

visualization_msgs::msg::ImageMarker CalibrationNode::getBoardMarkerFromCorners(
    std::vector<cv::Point2f>& detected_corners, std_msgs::msg::Header& header) {
    visualization_msgs::msg::ImageMarker marker;

    marker.header.frame_id = header.frame_id;
    marker.header.stamp = header.stamp;
    marker.ns = "calibration";
    marker.id = state_.last_marker_id.fetch_add(1);
    marker.type = visualization_msgs::msg::ImageMarker::POLYGON;
    marker.action = visualization_msgs::msg::ImageMarker::ADD;

    marker.position.x = 0;
    marker.position.y = 0;

    marker.scale = 1.;

    marker.filled = 1;
    marker.fill_color.r = param_.marker_color[0];
    marker.fill_color.g = param_.marker_color[1];
    marker.fill_color.b = param_.marker_color[2];
    marker.fill_color.a = param_.marker_color[3];

    marker.points = toBoardMsgCorners(detected_corners);
    return marker;
}

visualization_msgs::msg::ImageMarker CalibrationNode::getCornerMarker(cv::Point2f point) {
    visualization_msgs::msg::ImageMarker marker;

    marker.ns = "calibration";
    marker.id = state_.last_marker_id.fetch_add(1);
    marker.type = visualization_msgs::msg::ImageMarker::CIRCLE;
    marker.action = visualization_msgs::msg::ImageMarker::ADD;

    marker.position.x = point.x;
    marker.position.y = point.y;

    marker.scale = 6;

    marker.filled = 1;
    marker.fill_color.r = 1. - param_.marker_color[0];
    marker.fill_color.g = 1. - param_.marker_color[1];
    marker.fill_color.b = 1. - param_.marker_color[2];
    marker.fill_color.a = 0.7;

    return marker;
}

void CalibrationNode::appendCornerMarkers(
    const std::vector<cv::Point2f>& detected_corners, size_t camera_idx) {
    for (size_t i = 0; i < detected_corners.size(); ++i) {
        visualization_msgs::msg::ImageMarker marker = getCornerMarker(detected_corners[i]);
        state_.board_corners_array[camera_idx].markers.push_back(marker);
    }
}

void CalibrationNode::handleBadCalibration() {
    RCLCPP_INFO_STREAM(this->get_logger(), "Continue taking frames");

    state_.global_calibration_state = kCapturing;
}

void CalibrationNode::handleResetCommand() {
    state_.global_calibration_state = kNotCalibrated;
    for (size_t i = 0; i < param_.cameras_to_calibrate.size(); ++i) {
        state_.image_points_all[i].clear();
        state_.obj_points_all[i].clear();
        state_.board_markers_array[i].markers.clear();
        state_.polygons_all[i].clear();
        signal_.detected_boards[i]->publish(state_.board_markers_array[i]);  // to clear the screen
    }
}

void CalibrationNode::calibrate(size_t camera_idx) {
    state_.global_calibration_state = kMonoCalibrating;
    publishCalibrationState();
    RCLCPP_INFO_STREAM(this->get_logger(), "Calibration inialized");
    int coverage_percentage = getImageCoverage(camera_idx);
    if (coverage_percentage < param_.required_board_coverage) {
        RCLCPP_INFO(this->get_logger(), "Coverage of %d is not sufficient", coverage_percentage);
        handleBadCalibration();
        return;
    }

    CameraIntrinsicParameters intrinsic_params{};
    intrinsic_params.image_size = *state_.frame_size;
    std::vector<double> per_view_errors;
    double rms = cv::calibrateCamera(
        state_.obj_points_all[camera_idx],
        state_.image_points_all[camera_idx],
        *state_.frame_size,
        intrinsic_params.camera_matrix,
        intrinsic_params.dist_coefs,
        cv::noArray(),
        cv::noArray(),
        cv::noArray(),
        cv::noArray(),
        per_view_errors,
        cv::CALIB_FIX_S1_S2_S3_S4,
        cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, DBL_EPSILON));

    RCLCPP_INFO(
        this->get_logger(),
        "Calibration done with error of %f and coverage of %d",
        rms,
        coverage_percentage);

    if (rms < param_.min_accepted_error) {
        intrinsic_params.storeYaml(param_.path_to_save_params);
        state_.global_calibration_state = kOkCalibration;
    } else {
        handleBadCalibration();
    }
}
}  // namespace handy::calibration
