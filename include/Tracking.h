//定义预处理变量，#ifndef variance_name 表示变量未定义时为真，并执行之后的代码直到遇到 #endif。
#ifndef TRACKING_H
#define TRACKING_H

#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>

#include "Viewer.h"
#include "FrameDrawer.h"
#include "Map.h"
#include "LocalMapping.h"
#include "LoopClosing.h"
#include "Frame.h"
#include "ORBVocabulary.h"
#include "KeyFrameDatabase.h"
#include "ORBextractor.h"
#include "Initializer.h"
#include "MapDrawer.h"
#include "System.h"

#include "IMU/imudata.h"
#include "IMU/configparam.h"


#include <mutex>


namespace ORB_SLAM2
{
    class Viewer;

    class FrameDrawer;

    class Map;

    class LocalMapping;

    class LoopClosing;

    class System;


    class Tracking
    {

    public:
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        // Vision+IMU
        bool mbCreateNewKFAfterReloc;
        bool mbRelocBiasPrepare;

        void RecomputeIMUBiasAndCurrentNavstate(NavState &nscur);

        // 20帧用于计算偏移
        vector<Frame> mv20FramesReloc;

        // 通过IMU信息预测当前帧状态
        void PredictNavStateByIMU(bool bMapUpdated);

        IMUPreintegrator mIMUPreIntInTrack;

        // 运动模型估计
        bool TrackWithIMU(bool bMapUpdated = false);

        bool TrackLocalMapWithIMU(bool bMapUpdated = false);

        ConfigParam *mpParams;

        cv::Mat GrabImageMonoVI(const cv::Mat &im, const std::vector<IMUData> &vimu, const double &timestamp);

        // 保存上一帧关键帧到当前帧的IMU数据，用于创建关键帧后进行预积分
        // 在初始化和新关键帧创建时被清空
        std::vector<IMUData> mvIMUSinceLastKF;

        IMUPreintegrator
        GetIMUPreIntSinceLastKF(Frame *pCurF, KeyFrame *pLastKF, const std::vector<IMUData> &vIMUSInceLastKF);

        IMUPreintegrator GetIMUPreIntSinceLastFrame(Frame *pCurF, Frame *pLastF);

    public:
        // 构造函数。
        // Tracking(System *pSys, ORBVocabulary *pVoc, FrameDrawer *pFrameDrawer, MapDrawer *pMapDrawer, Map *pMap,
        //          KeyFrameDatabase *pKFDB, const string &strSettingPath, const int sensor);
        Tracking(System *pSys, ORBVocabulary *pVoc, FrameDrawer *pFrameDrawer, MapDrawer *pMapDrawer, Map *pMap,
                 KeyFrameDatabase *pKFDB, const string &strSettingPath, const int sensor, ConfigParam *pParams);

        // 对输入的图像进行预处理，调用Track（）。提取特征并进行立体匹配。
        cv::Mat GrabImageStereo(const cv::Mat &imRectLeft, const cv::Mat &imRectRight, const double &timestamp);

        cv::Mat GrabImageRGBD(const cv::Mat &imRGB, const cv::Mat &imD, const double &timestamp);

        cv::Mat GrabImageMonocular(const cv::Mat &im, const double &timestamp);

        // 设置各线程间类对象的指针，传递数据。
        void SetLocalMapper(LocalMapping *pLocalMapper);

        void SetLoopClosing(LoopClosing *pLoopClosing);

        void SetViewer(Viewer *pViewer);

        // 加载设置文件。
        void ChangeCalibration(const string &strSettingPath);


        // 关闭局部地图只进行相机定位。
        void InformOnlyTracking(const bool &flag);


    public:

        // 跟踪状态。
        enum eTrackingState
        {
            SYSTEM_NOT_READY = -1,
            NO_IMAGES_YET = 0,
            NOT_INITIALIZED = 1,
            OK = 2,
            LOST = 3
        };

        eTrackingState mState;
        eTrackingState mLastProcessedState;

        // 输入传感器。
        int mSensor;

        // 当前帧。
        Frame mCurrentFrame;
        cv::Mat mImGray;

        // 单目 初始化变量。
        std::vector<int> mvIniLastMatches;
        std::vector<int> mvIniMatches;                // 初始化帧间匹配情况
        std::vector<cv::Point2f> mvbPrevMatched;        // 初始化帧特征点
        std::vector<cv::Point3f> mvIniP3D;            // 三角化初始地图点云
        Frame mInitialFrame;

        // 链表lists用来存储恢复相机完整轨迹的数据。
        // 存储参考关键帧和每帧相对它的运动变换。
        list <cv::Mat> mlRelativeFramePoses;            // 相对参考帧的位姿。
        list<KeyFrame *> mlpReferences;                // 参考帧。
        list<double> mlFrameTimes;
        list<bool> mlbLost;                    // 帧丢失情况

        // 为1时关闭局部地图至进行相机定位。
        bool mbOnlyTracking;

        void Reset();

    protected:

        // 主跟踪函数，与传感器类型无关。
        void Track();

        // 对于双目和RGBD的地图初始化。
        void StereoInitialization();

        // 对单目传感器的地图初始化。
        void MonocularInitialization();

        void CreateInitialMapMonocular();

        // 恒速模型估计
        void CheckReplacedInLastFrame();

        bool TrackReferenceKeyFrame();

        void UpdateLastFrame();

        bool TrackWithMotionModel();

        // 重定位
        bool Relocalization();

        //更新局部地图，点云，关键帧。
        void UpdateLocalMap();

        void UpdateLocalPoints();

        void UpdateLocalKeyFrames();

        // 局部地图跟踪
        bool TrackLocalMap();

        void SearchLocalPoints();

        // 添加新关键帧
        bool NeedNewKeyFrame();

        void CreateNewKeyFrame();

        // 在执行定位模式时，当与地图中的点云没有匹配时，这个标志为1。如果有暂时的匹配点跟踪可以继续。
        // 在这种情况下按照视觉里程计运行。系统会尝试进行重定位得到0偏移的局部地图
        bool mbVO;

        // 其他线程的指针。
        LocalMapping *mpLocalMapper;
        LoopClosing *mpLoopClosing;


        // ORB
        // ORB特征提取器，不论是单目还是双目，都会用到mpORBextractorLeft。
        // 如果是双目，还会用到mpORBextractorRight。
        // 如果是单目，在初始化时用mpIniORBextractor而不是mpORBextractorLeft。
        // mpIniORBextractor提取的特征点个数是mpORbextractorLeft的两倍。
        ORBextractor *mpORBextractorLeft, *mpORBextractorRight;
        ORBextractor *mpIniORBextractor;

        // BoW
        ORBVocabulary *mpORBVocabulary;
        KeyFrameDatabase *mpKeyFrameDB;

        //单目初始化
        Initializer *mpInitializer;

        // 局部地图
        KeyFrame *mpReferenceKF;                // 当参考帧，前关键帧就是参考帧
        std::vector<KeyFrame *> mvpLocalKeyFrames;        // 局部关键帧集合
        std::vector<MapPoint *> mvpLocalMapPoints;        // 局部地图点云集合。


        // System类
        System *mpSystem;

        // Drawers
        Viewer *mpViewer;
        FrameDrawer *mpFrameDrawer;
        MapDrawer *mpMapDrawer;

        // Map类
        Map *mpMap;


        // 相机标定参数
        cv::Mat mK;
        cv::Mat mDistCoef;
        float mbf;

        // 新建关键帧的规则(根据帧率fps)
        int mMinFrames;
        int mMaxFrames;


        // 点云远近的深度阈值
        // 通过双目或RGBD观测到的深度较小点云总认为是可靠的,并且根据一帧就可以插入。深度较大的关键帧需要在两帧关键帧中找到匹配。
        float mThDepth;

        // 对于RGBD相机来说，一些数据集的深度信息是被放缩过的，例如TUM数据集。
        float mDepthMapFactor;

        // 在这一帧中的匹配内点数量
        int mnMatchesInliers;

        // 上一帧，关键帧和重定位信息
        KeyFrame *mpLastKeyFrame;
        Frame mLastFrame;
        unsigned int mnLastKeyFrameId;
        unsigned int mnLastRelocFrameId;


        // 恒速运动模型
        cv::Mat mVelocity;

        // 颜色类型 （真RGB,假RGB,如果时灰度则忽略）
        bool mbRGB;

        list<MapPoint *> mlpTemporalPoints;
    };
}
#endif
