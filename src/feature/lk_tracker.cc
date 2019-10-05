#include "lk_tracker.h"
#include <cmath>
#include "util/tf_util.h"

namespace omni_slam
{
namespace feature
{

LKTracker::LKTracker(const int window_size, const int num_scales, const float delta_pix_err_thresh, const float err_thresh, const int template_update_rate, const int term_count, const double term_eps)
    : windowSize_(window_size / pow(2, num_scales), window_size / pow(2, num_scales)),
    numScales_(num_scales),
    errThresh_(err_thresh),
    deltaPixErrThresh_(delta_pix_err_thresh),
    termCrit_(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, term_count, term_eps),
    templateUpdateRate_(template_update_rate),
    prevFrame_(nullptr)
{
}

void LKTracker::Init(data::Frame &init_frame)
{
    frameNum_ = 0;
    prevId_ = init_frame.GetID();
    prevFrame_ = &init_frame;
    prevTemplateId_ = init_frame.GetID();
    prevImg_ = init_frame.GetImage().clone();
    if (init_frame.HasStereoImage())
    {
        prevStereoImg_ = init_frame.GetStereoImage().clone();
    }
}

int LKTracker::Track(std::vector<data::Landmark> &landmarks, data::Frame &cur_frame, std::vector<double> &errors, bool stereo)
{
    if (prevImg_.empty())
    {
        return 0;
    }
    bool curCompressed = cur_frame.IsCompressed();
    std::vector<cv::Point2f> pointsToTrack;
    std::vector<cv::KeyPoint> origKpt;
    std::vector<int> origInx;
    std::vector<cv::Point2f> stereoPointsToTrack;
    std::vector<cv::KeyPoint> stereoOrigKpt;
    std::vector<int> stereoOrigInx;
    std::vector<cv::Point2f> results;
    std::vector<cv::Point2f> stereoResults;
    for (int i = 0; i < landmarks.size(); i++)
    {
        data::Landmark &landmark = landmarks[i];
        const data::Feature *feat = landmark.GetObservationByFrameID(prevTemplateId_);
        const data::Feature *featPrev = landmark.GetObservationByFrameID(prevId_);
        if (feat != nullptr && featPrev != nullptr)
        {
            pointsToTrack.push_back(feat->GetKeypoint().pt);
            results.push_back(featPrev->GetKeypoint().pt);
            origKpt.push_back(feat->GetKeypoint());
            origInx.push_back(i);
        }
        if (cur_frame.HasStereoImage() && !prevStereoImg_.empty())
        {
            const data::Feature *stereoFeat = landmark.GetStereoObservationByFrameID(prevTemplateId_);
            const data::Feature *stereoFeatPrev = landmark.GetStereoObservationByFrameID(prevId_);
            if (stereoFeat != nullptr && stereoFeatPrev != nullptr)
            {
                stereoPointsToTrack.push_back(stereoFeat->GetKeypoint().pt);
                stereoResults.push_back(stereoFeatPrev->GetKeypoint().pt);
                stereoOrigKpt.push_back(stereoFeat->GetKeypoint());
                stereoOrigInx.push_back(i);
            }
        }
    }
    if (pointsToTrack.size() == 0)
    {
        prevId_ = cur_frame.GetID();
        prevFrame_ = &cur_frame;
        if (++frameNum_ % templateUpdateRate_ == 0)
        {
            prevTemplateId_ = cur_frame.GetID();
            prevImg_ = cur_frame.GetImage().clone();
            if (cur_frame.HasStereoImage())
            {
                prevStereoImg_ = cur_frame.GetStereoImage().clone();
            }
        }
        return 0;
    }
    std::vector<unsigned char> status;
    std::vector<float> err;
    std::vector<unsigned char> stereoStatus;
    std::vector<float> stereoErr;
    if (prevId_ == prevTemplateId_)
    {
        cv::calcOpticalFlowPyrLK(prevImg_, cur_frame.GetImage(), pointsToTrack, results, status, err, windowSize_, numScales_, termCrit_, 0);
    }
    else
    {
        cv::calcOpticalFlowPyrLK(prevImg_, cur_frame.GetImage(), pointsToTrack, results, status, err, windowSize_, numScales_, termCrit_, cv::OPTFLOW_USE_INITIAL_FLOW);
    }
    if (stereoPointsToTrack.size() > 0)
    {
        if (prevId_ == prevTemplateId_)
        {
            cv::calcOpticalFlowPyrLK(prevStereoImg_, cur_frame.GetStereoImage(), stereoPointsToTrack, stereoResults, stereoStatus, stereoErr, windowSize_, numScales_, termCrit_, 0);
        }
        else
        {
            cv::calcOpticalFlowPyrLK(prevStereoImg_, cur_frame.GetStereoImage(), stereoPointsToTrack, stereoResults, stereoStatus, stereoErr, windowSize_, numScales_, termCrit_, cv::OPTFLOW_USE_INITIAL_FLOW);
        }
    }
    errors.clear();
    int numGood = 0;
    for (int i = 0; i < results.size(); i++)
    {
        data::Landmark &landmark = landmarks[origInx[i]];
        if (deltaPixErrThresh_ > 0)
        {
            if (landmark.HasGroundTruth() && cur_frame.HasPose())
            {
                Vector2d pixelGnd;
                Vector2d pixelGndPrev;
                if (!cur_frame.GetCameraModel().ProjectToImage(util::TFUtil::WorldFrameToCameraFrame(util::TFUtil::TransformPoint(cur_frame.GetInversePose(), landmark.GetGroundTruth())), pixelGnd) || !prevFrame_->GetCameraModel().ProjectToImage(util::TFUtil::WorldFrameToCameraFrame(util::TFUtil::TransformPoint(prevFrame_->GetInversePose(), landmark.GetGroundTruth())), pixelGndPrev))
                {
                    continue;
                }
                else
                {
                    Vector2d pixelCur;
                    pixelCur << results[i].x, results[i].y;
                    Vector2d pixelPrev;
                    pixelPrev << pointsToTrack[i].x, pointsToTrack[i].y;
                    double cur_error = (pixelCur - pixelGnd).norm();
                    double prev_error = (pixelPrev - pixelGndPrev).norm();
                    if (cur_error - prev_error > deltaPixErrThresh_)
                    {
                        continue;
                    }
                }
            }
        }
        if (status[i] == 1 && err[i] <= errThresh_)
        {
            cv::KeyPoint kpt(results[i], origKpt[i].size);
            data::Feature feat(cur_frame, kpt);
            landmark.AddObservation(feat);
            errors.push_back(err[i]);
            numGood++;
        }
    }
    for (int i = 0; i < stereoResults.size(); i++)
    {
        data::Landmark &landmark = landmarks[stereoOrigInx[i]];
        if (!landmark.IsObservedInFrame(cur_frame.GetID()))
        {
            continue;
        }
        if (stereoStatus[i] == 1 && stereoErr[i] <= errThresh_)
        {
            cv::KeyPoint kpt(stereoResults[i], stereoOrigKpt[i].size);
            data::Feature feat(cur_frame, kpt);
            landmark.AddStereoObservation(feat);
        }
    }
    if (curCompressed)
    {
        cur_frame.CompressImages();
    }

    prevId_ = cur_frame.GetID();
    prevFrame_ = &cur_frame;
    if (++frameNum_ % templateUpdateRate_ == 0)
    {
        prevTemplateId_ = cur_frame.GetID();
        prevImg_ = cur_frame.GetImage().clone();
        if (cur_frame.HasStereoImage())
        {
            prevStereoImg_ = cur_frame.GetStereoImage().clone();
        }
    }

    return numGood;
}

}
}
