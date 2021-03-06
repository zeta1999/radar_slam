//
// Created by gao on 2020/12/28.
//
#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
//#include <opencv2/features2d.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/core/base.hpp>
#include "radar_utils.hpp"
#include "features.hpp"
#include <eigen3/Eigen/Core>
#include <queue>
#include <time.h>
#include <stdlib.h>
#include "association.hpp"
#include "teaser/registration.h"
//#include <opencv2/imgproc.hpp>

// Macro constants for generating noise and outliers
#define NOISE_BOUND 0.05
//#define N_OUTLIERS 1700

#define HIGHT 640
#define WIDTH 960
#define SCALE 3.0

#define MAX_RANGE 600
#define AZIMUTH_RANGE 360
#define OUTLIER_THRESHOLD 5.0

#define FILE_NUM 40

using namespace std;


// Run TEASER++ registration
// Prepare solver parameters
teaser::RobustRegistrationSolver::Params params;



struct Feature{
    int id; //feature id
    vector<pair<int, Eigen::Vector3d>> pointSequence; //frameNum, point
};

vector<Eigen::Vector3d> read_csv(string path){
    vector<Eigen::Vector3d> pcl;
    ifstream fin(path);
    string line;
    getline(fin, line);
    while(getline(fin, line)){
        istringstream sin(line);
        Eigen::Vector3d point;
        vector<double> fields;
        string field;
        while(getline(sin, field, ',')){
            fields.push_back(atof(field.c_str()));
        }
        point.x() = fields[0];
        point.y() = fields[1];
        point.z() = fields[2];
        pcl.push_back(point);
    }
    return pcl;
}

class FeatureTracker{
public:
    FeatureTracker(vector<Eigen::Vector3d> _pcl):prePcl(_pcl){
        trace.push_back(Eigen::Vector3d(0,0,0));
    }

    bool AddKeyPoints(std::vector<cv::KeyPoint>& _kp, const vector<Eigen::Vector3d> _pcl, int patch_size, bool scale = true){
        double u,v;
        _kp.clear();
        if(scale){
            for(int i = 0; i < _pcl.size(); i++){
                u = _pcl[i].x() * SCALE + WIDTH/2;
                v = _pcl[i].y() * SCALE + HIGHT/2;
                _kp.push_back(cv::KeyPoint(u,v,patch_size));
            }
        }
        else{
            for(int i = 0; i < _pcl.size(); i++){
                u = _pcl[i].x();
                v = _pcl[i].y();
                _kp.push_back(cv::KeyPoint(u,v,patch_size));
            }
        }

        return true;
    }

    cv::Mat Points2Image(vector<Eigen::Vector3d> _pcl, bool drawPoints = false){
        cv::Mat img = cv::Mat::zeros(HIGHT, WIDTH, CV_8UC3);
        int x,y;
        cv::Point2f center;
        for(int i = 0; i < _pcl.size(); i++){
            x = (int)(_pcl[i].x()*SCALE + WIDTH/2);
            y = (int)(_pcl[i].y()*SCALE + HIGHT/2);
            center = cv::Point(x, y);
            cv::circle(img, center, 2, cv::Scalar(0,255,255), cv::FILLED);
        }
        cv::Mat img_tmp;
        cv::circle(img, cv::Point2f(0,0), 10, cv::Scalar(0,255,255), cv::FILLED);
        cv::cvtColor(img, img_tmp, cv::COLOR_BGR2GRAY);

        if(drawPoints){
            cv::imshow("pcl_img", img_tmp);
            cv::waitKey(0);
        }

        return img_tmp;
    }

    vector<Eigen::Vector3d> pointsCvt2RealPoints(std::vector<Eigen::Vector3d> _points){
        Eigen::Vector3d p;
        vector<Eigen::Vector3d> pcl;
        for(int i = 0; i < _points.size(); i++){
            p.x() = (_points[i].x() - WIDTH/2) / SCALE;
            p.y() = (_points[i].y() - HIGHT/2) / SCALE;
            p.z() = 0.;
            pcl.push_back(p);
        }
        return pcl;
    }

    vector<Eigen::Vector3d> RealPoints2Imgpoints(std::vector<cv::KeyPoint> _points){
        Eigen::Vector3d p;
        vector<Eigen::Vector3d> pcl;
        for(int i = 0; i < _points.size(); i++){
            p.x() = (int)(_points[i].pt.x*SCALE + WIDTH/2);
            p.y() = (int)(_points[i].pt.y*SCALE + HIGHT/2);
            p.z() = 0.;
            pcl.push_back(p);
        }
        return pcl;
    }


    void OutlierExeclusion(const vector<Eigen::Vector3d> _pcl1, const vector<Eigen::Vector3d> _pcl2,
                           vector<Eigen::Vector3d> &_pcl1_new, vector<Eigen::Vector3d> &_pcl2_new,
                           std::vector<cv::DMatch> &matches){
        vector<cv::Point2f> pcl1, pcl2;
        std::vector<cv::DMatch> good_matches;
        for (uint j = 0; j < matches.size(); ++j) {
            cv::Point2f p1, p2;
            p1.x = _pcl1[matches[j].queryIdx].x();
            p1.y = _pcl1[matches[j].queryIdx].y();
            p2.x = _pcl2[matches[j].trainIdx].x();
            p2.y = _pcl2[matches[j].trainIdx].y();
            pcl1.push_back(p1);
            pcl2.push_back(p2);
        }
        cv::Mat mask;
        cv::findHomography(pcl1, pcl2, mask,cv::RANSAC, OUTLIER_THRESHOLD);
//        vector<cv::Point2f> pcl1_new, pcl2_new;

        int j = 0;
        for(int i=0; i < mask.rows; i++){
            if(mask.at<uchar>(i,0) == 1){
                _pcl1_new.emplace_back(pcl1[i].x, pcl1[i].y, 0);
                _pcl2_new.emplace_back(pcl2[i].x, pcl2[i].y, 0);
                good_matches.push_back(matches[i]);
                good_matches.back().trainIdx = j;
                good_matches.back().queryIdx = j;
                j++;
            }
        }
        matches = good_matches;
    }

    bool myDescriptor(cv::Mat image, cv::Mat &descriptors, vector<cv::KeyPoint> keypoints){

        uint M = AZIMUTH_RANGE;
        float azimuth_step = (2 * M_PI) / float(M);
        uint max_range = MAX_RANGE;
        uint range;
        float azimuth;
        cv::Mat desc1 = cv::Mat::zeros(keypoints.size(), max_range, CV_32F);
        cv::Mat desc2 = cv::Mat::zeros(keypoints.size(), M, CV_32F);
        vector<Eigen::Vector2i> imgKeyPoints;

        for(auto kp:keypoints){
            Eigen::Vector2i p;
            p.x() = kp.pt.x;
            p.y() = kp.pt.y;
            imgKeyPoints.push_back(p);
        }
        for(uint i = 0; i < image.rows; i++){
            for(uint j = 0; j < image.cols; j++){
                if(image.at<uchar>(i,j) != 0){
                    for(uint k = 0; k < keypoints.size(); k++){

                        azimuth = atan2f(i - imgKeyPoints[k].y(), j - imgKeyPoints[k].x());
                        if(azimuth < 0)
                            azimuth += 2 * M_PI;
                        desc2.at<float>(k, azimuth/azimuth_step)++;

                        range = sqrt(pow(i-imgKeyPoints[k].y(), 2) + pow(j-imgKeyPoints[k].x(), 2));
                        if(range > max_range)
                            continue;
                        else{
                            desc1.at<float>(k, range)++;
                        }
                    }
                }
            }
        }
        for(uint i = 0; i < keypoints.size(); i++){
            cv::normalize(desc1.row(i), desc1.row(i), 0, 1, cv::NORM_MINMAX);
        }

        for(uint i = 0; i < keypoints.size(); i++){
            int max_index = 0;
            for(int j = 0; j < M; j++) {
                if (desc2.at<float>(i, j) > desc2.at<float>(i, max_index)){
//                    cout << desc2.at<float>(i, j) << " " << desc2.at<float>(i, max_index) << "\n";
                    max_index = j;
                }
            }
            if(max_index != 0){
                cv::Mat tmp;
                desc2.row(i).colRange(0,max_index).copyTo(tmp);
                desc2.row(i).colRange(max_index, M).copyTo(desc2.row(i).colRange(0, M - max_index));
                tmp.copyTo(desc2.row(i).colRange(M - max_index, M));
            }
            cv::normalize(desc2.row(i), desc2.row(i), 0, 1, cv::NORM_MINMAX);
        }

        cv::hconcat(desc1, desc2, descriptors);
//        for(uint i = 0; i < keypoints.size(); i++){
//            cv::normalize(descriptors.row(i), descriptors.row(i), 0, 1, cv::NORM_MINMAX);
//        }

//        desc1.copyTo(descriptors);
//        desc2.copyTo(descriptors);
        return true;
    }


    bool MatchPoints(vector<Eigen::Vector3d> _pcl){

        frameNum++;
        vector<Eigen::Vector3d> &pcl_1 = prePcl;
        vector<Eigen::Vector3d> &pcl_2 = _pcl;
        cv::Mat img1, img2;

        //match pcl_1 and pcl_2
        img1 = Points2Image(pcl_1, false);
        img2 = Points2Image(pcl_2, false);

        int patch_size = 2;
        cv::Mat desc1, desc2;//descriptor
        std::vector<cv::KeyPoint> kp1, kp2;//key points
        AddKeyPoints(kp1, pcl_1, patch_size);
        AddKeyPoints(kp2, pcl_2, patch_size);
        myDescriptor(img1, desc1, kp1);
        myDescriptor(img2, desc2, kp2);
//        detector->detectAndCompute(img1, cv::Mat(), kp1, desc1);
//        detector->detectAndCompute(img2, cv::Mat(), kp2, desc2);


//        detector->setPatchSize(patch_size);
//        detector->setEdgeThreshold(patch_size);

//        detector->compute(img1, kp1, desc1);
//        detector->compute(img2, kp2, desc2);

        matcher.knnMatch(desc1, desc2, knn_matches, 1);

//        std::vector<cv::DMatch> good_matches;
        for (uint j = 0; j < knn_matches.size(); ++j) {
            if (!knn_matches[j].size())
                continue;
            if (knn_matches[j][0].distance < OUTLIER_THRESHOLD){
                good_matches.push_back(knn_matches[j][0]); // distance the smaller the better
            }
        }


        vector<Eigen::Vector3d> pcl1_new, pcl2_new, pcl_tmp1, pcl_tmp2;
        for(uint j = 0; j < kp1.size(); j++){
            pcl_tmp1.emplace_back(kp1[j].pt.x, kp1[j].pt.y, 0.0);
        }
        for(uint j = 0; j < kp2.size(); j++){
            pcl_tmp2.emplace_back(kp2[j].pt.x, kp2[j].pt.y, 0.0);
        }
        OutlierExeclusion(pcl_tmp1, pcl_tmp2, pcl1_new, pcl2_new, good_matches);
        cout << "kp_1 size is: " << kp1.size() << endl;
        cout << "pcl1_new size is: " << pcl1_new.size() << endl;
        AddKeyPoints(kp1, pcl1_new, patch_size, false);
        AddKeyPoints(kp2, pcl2_new, patch_size, false);


        pcl1_new = pointsCvt2RealPoints(pcl1_new);
        pcl2_new = pointsCvt2RealPoints(pcl2_new);// convert image keypoints to real world points
        // Convert the good key point matches to Eigen matrices
        Eigen::MatrixXd p1 = Eigen::MatrixXd::Zero(2, pcl1_new.size());
        Eigen::MatrixXd p2 = p1;
//        for (uint j = 0; j < good_matches.size(); ++j) {
//            p1(0, j) = pcl1_new[good_matches[j].queryIdx].x();
//            p1(1, j) = pcl1_new[good_matches[j].queryIdx].y();
//            p2(0, j) = pcl2_new[good_matches[j].trainIdx].x();
//            p2(1, j) = pcl2_new[good_matches[j].trainIdx].y();
//        }
        for(uint j = 0; j < pcl1_new.size(); j++){
            p1(0,j) = pcl1_new[j].x();
            p1(1,j) = pcl1_new[j].y();
            p2(0,j) = pcl2_new[j].x();
            p2(1,j) = pcl2_new[j].y();
        }

        Eigen::Matrix<double, 3, Eigen::Dynamic> src, tgt;

        for(int i = 0; i < pcl1_new.size(); i++){
            src.col(i) << pcl1_new[i].x(), pcl1_new[i].y(), pcl1_new[i].z();
        }
        for(int i = 0; i < pcl2_new.size(); i++){
            tgt.col(i) << pcl2_new[i].x(), pcl2_new[i].y(), pcl2_new[i].z();
        }

        // Teaser Solver
        teaser::RobustRegistrationSolver solver(params);
        solver.solve(src, tgt);
        auto solution = solver.getSolution();
        std::cout << "Estimated rotation: " << std::endl;
        std::cout << solution.rotation << std::endl;
        std::cout << "Estimated translation: " << std::endl;
        std::cout << solution.translation << std::endl;

//        Ransac ransac(p2, p1, ransac_threshold, inlier_ratio, max_iterations);
//        srand(time(NULL));
//        ransac.computeModel();
//        Eigen::MatrixXd T;  // T_1_2
//        ransac.getTransform(T);
//        cout << to_string(frameNum) + ":" << "\n" << T << "\n" << endl;

        cv::Mat img_match;
        try{
            cv::drawMatches(img1, kp1, img2, kp2, good_matches, img_match);
            cv::imshow("match_img", img_match);
            cv::waitKey(0);
        }
        catch (exception &e){
            cout << "Standard exception: " << e.what() << endl;
        }

        //record results
//        float yaw = -1 * asin(T(0, 1));
//        trace.emplace_back(T(0,2), T(1,2), yaw);

        //update featureVec
        prePcl = _pcl;
        good_matches.clear();
        knn_matches.clear();
        return true;
    }
    int minHessian = 150;
    cv::Ptr<cv::xfeatures2d::SURF> detector = cv::xfeatures2d::SURF::create(minHessian, 4, 3, true, true);
//    cv::Ptr<cv::DescriptorMatcher> matcher = cv::DescriptorMatcher::create(cv::DescriptorMatcher::BRUTEFORCE_HAMMING);
    cv::BFMatcher matcher = cv::BFMatcher();
    std::vector<std::vector<cv::DMatch>> knn_matches;
    vector<cv::DMatch> good_matches;

    //ransac parameters
    double ransac_threshold = 1.0;
    double inlier_ratio = 0.80;
    int max_iterations = 1000;

    int frameNum = 0;
    vector<Eigen::Vector3d> prePcl;
    vector<Eigen::Vector3d> trace;


};


int main(int argc, char** argv){
    std::string datadir = "/home/gao/Dopper_radar/catkin_ws/src/beginner_tutorial/data/scene8/pcl/";
    std::string seq = "pcl_";
    std::vector<std::string> radar_files;
    string radar_path0 = datadir + seq + "0" + ".csv";
//    string radar_path1 = datadir + seq + "1" + ".csv";
    ofstream ofs;

    params.noise_bound = NOISE_BOUND;
    params.cbar2 = 1;
    params.estimate_scaling = false;
    params.rotation_max_iterations = 100;
    params.rotation_gnc_factor = 1.4;
    params.rotation_estimation_algorithm =
            teaser::RobustRegistrationSolver::ROTATION_ESTIMATION_ALGORITHM::GNC_TLS;
    params.rotation_cost_threshold = 0.005;

    // Solve with TEASER++
//    teaser::RobustRegistrationSolver solver(params);
//    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
//    solver.solve(src, tgt);
//    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

//    auto solution = solver.getSolution();

    vector<Eigen::Vector3d> pcl_0;
    pcl_0 = read_csv(radar_path0);

    FeatureTracker featureTracker(pcl_0);

    for(int i=1; i < FILE_NUM; i++){
        string path = datadir + seq + to_string(i) + ".csv";
        vector<Eigen::Vector3d> pcl;
        pcl = read_csv(path);
        if(!featureTracker.MatchPoints(pcl)){
            cout << "error occurred!" << endl;
            break;
        }
    }

//    ofs.open("/home/gao/Dopper_radar/catkin_ws/src/beginner_tutorial/data/scene8/results/results.csv", std::ios::out);
//    ofs << "x" << "," << "y" << "," << "yaw"  << "\n";
//    for(int i = 0; i < featureTracker.trace.size(); i++){
//        ofs << featureTracker.trace[i].x() << ","
//            << featureTracker.trace[i].y() << ","
//            << featureTracker.trace[i].z() << "\n";
//    }

    return 0;
}
