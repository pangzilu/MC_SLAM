



#include "Frame.h"
#include "Converter.h"
#include "ORBmatcher.h"

#include <thread>

namespace ORB_SLAM2
{

    // 定义并初始化静态成员，声明静态成员在.h中。
    long unsigned int Frame::nNextId = 0;
    bool Frame::mbInitialComputations = true;
    float Frame::cx, Frame::cy, Frame::fx, Frame::fy, Frame::invfx, Frame::invfy;
    float Frame::mnMinX, Frame::mnMinY, Frame::mnMaxX, Frame::mnMaxY;
    float Frame::mfGridElementWidthInv, Frame::mfGridElementHeightInv; 


    
    /******************************VI SLAM*************************/
    // 利用IMU数据计算预积分，保存在IMUPreInt
    void Frame::ComputeIMUPreIntSinceLastFrame(const Frame* pLastF, IMUPreintegrator& IMUPreInt) const
    {
	// 重置预积分数据
	IMUPreInt.reset();
	
	const std::vector<IMUData> &vIMUSInceLastFrame = mvIMUDataSinceLastFrame;
	
	Vector3d bg = pLastF->GetNavState().Get_BiasGyr();
	Vector3d ba = pLastF->GetNavState().Get_BiasAcc();
	
	// 上一帧到第一个IMU数据的预积分值
	{
	    const IMUData & imu = vIMUSInceLastFrame.front();
	    // 保存第一个IMU数据与上一帧的时间差
	    double dt = imu._t - pLastF->mTimeStamp;
	    IMUPreInt.update(imu._g - bg, imu._a - ba, dt);
	    
	    if(dt < 0)
	    {
		cerr << std::fixed << std::setprecision(3) << "dt = " << dt << ", this frame vs last imu time:" << pLastF->mTimeStamp << " vs " << imu._t<<endl;
		std::cerr.unsetf(std::ios::showbase);
	    }   
	}
	
	for(size_t i=0; i<vIMUSInceLastFrame.size(); i++)
	{
	    const IMUData &imu = vIMUSInceLastFrame[i];
	    double nextt;					// 下一个时刻的imu数据时间戳
	    // 本组的imu数据的最后一个，时间戳是当前帧
	    if(i==vIMUSInceLastFrame.size()-1)
		nextt = mTimeStamp;
	    else
		nextt = vIMUSInceLastFrame[i+1]._t;
	    
	    double dt = nextt - imu._t;
	    // 预积分计算PRV
	    IMUPreInt.update(imu._g-bg, imu._a-ba, dt);
	    
	    // 测试输出
	    if(dt <= 0)
	    {
		cerr << std::fixed << std::setprecision(3) << "dt = " << dt << ", this vs next time: " << imu._t << "vs" << nextt << endl;
		std::cerr.unsetf(std::ios::showbase);
	    }
	}
	
    }
    
    
    // 更新当前帧pose
    void Frame::UpdatePoseFromNS(const cv::Mat& Tbc)
    {
	cv::Mat Rbc = Tbc.rowRange(0,3).colRange(0,3).clone();
	cv::Mat Pbc = Tbc.rowRange(0,3).col(3).clone();
	
	cv::Mat Rwb = Converter::toCvMat(mNavState.Get_RotMatrix());
	cv::Mat Pwb = Converter::toCvMat(mNavState.Get_P());
	
	cv::Mat Rcw = (Rwb*Rbc).t();
	cv::Mat Pwc = Rwb*Pbc + Pwb;
	cv::Mat Pcw = -Rcw*Pwc;
	
	cv::Mat Tcw = cv::Mat::eye(4,4,CV_32F);
	Rcw.copyTo(Tcw.rowRange(0,3).colRange(0,3));
	Pcw.copyTo(Tcw.rowRange(0,3).col(3));
	
	SetPose(Tcw);
	
    }
    
    
    
    // 更新导航状态
    void Frame::UpdateNavState(const IMUPreintegrator& imupreint, const Vector3d& gw)
    {
	Converter::updateNS(mNavState, imupreint, gw);
    }
    


    const NavState &Frame::GetNavState(void) const
    {
	return mNavState;
    }
    
    
    
    void Frame::SetInitialNavStateAndBias(const NavState& ns)
    {
	mNavState = ns;
	mNavState.Set_BiasGyr(ns.Get_BiasGyr()+ns.Get_dBias_Gyr());
	mNavState.Set_BiasAcc(ns.Get_BiasAcc()+ns.Get_dBias_Acc());
	mNavState.Set_DeltaBiasGyr(Vector3d::Zero());
	mNavState.Set_DeltaBiasAcc(Vector3d::Zero());
    }
    
    
    
    void Frame::SetNavStateBiasGyr(const Vector3d& bg)
    {
	mNavState.Set_BiasGyr(bg);
    }
    
    
    
    void Frame::SetNavStateBiasAcc(const Vector3d& ba)
    {
	mNavState.Set_BiasAcc(ba);
    }

    
    
    void Frame::SetNavState(const NavState& ns)
    {
	mNavState = ns;
    }



    Frame::Frame(const cv::Mat& imGray, const double& timeStamp, const std::vector<IMUData>& vimu, ORBextractor* extractor, ORBVocabulary* voc, 
		 cv::Mat& K, cv::Mat& distCoef, const float& bf, const float& thDepth, KeyFrame* pLastKF):
		 mpORBvocabulary(voc), mpORBextractorLeft(extractor), mpORBextractorRight(static_cast<ORBextractor*>(NULL)),
		 mTimeStamp(timeStamp), mK(K.clone()), mDistCoef(distCoef.clone()), mbf(bf), mThDepth(thDepth)
    {
	
	// 保存imu数据
	mvIMUDataSinceLastFrame = vimu;
	
	mnId = nNextId++;
	
	// 尺度信息
	mnScaleLevels = mpORBextractorLeft->GetLevels();
	mfScaleFactor = mpORBextractorLeft->GetScaleFactor();
	mfLogScaleFactor = log(mfScaleFactor);
	mvScaleFactors = mpORBextractorLeft->GetScaleFactors();
	mvInvScaleFactors = mpORBextractorLeft->GetInverseScaleFactors();
	mvLevelSigma2 = mpORBextractorLeft->GetScaleSigmaSquares();
	mvInvLevelSigma2 = mpORBextractorLeft->GetInverseScaleSigmaSquares();
	
	ExtractORB(0, imGray);
	
	N = mvKeys.size();
	
	if(mvKeys.empty())
	    return;
	
	UndistortKeyPoints();
	
	// 设定标志位，表示无立体匹配信息
	mvuRight = vector<float>(N,-1);
	mvDepth = vector<float>(N,-1);
	
	mvpMapPoints = vector<MapPoint *>(N, static_cast<MapPoint *>(NULL));
	mvbOutlier = vector<bool>(N,false);
	
	if(mbInitialComputations)
	{
	    ComputeImageBounds(imGray);
	    mfGridElementWidthInv = static_cast<float>(FRAME_GRID_COLS)/static_cast<float>(mnMaxX-mnMinX);
	    mfGridElementHeightInv = static_cast<float>(FRAME_GRID_ROWS)/static_cast<float>(mnMaxY-mnMinY);
	    
	    fx = K.at<float>(0,0);
	    fy = K.at<float>(1,1);
	    cx = K.at<float>(0,2);
		cy = K.at<float>(1,2);
	    invfx = 1.0f/fx;
	    invfy = 1.0f/fy;
	    
	    mbInitialComputations = false;
	}
	
	mb = mbf/fx;
	
	AssignFeaturesToGrid();
	
    }

    
    /**********************************************************/
    
    // 默认构造函数。 
    Frame::Frame()
    {

    }



    // 复制构造函数。
    Frame::Frame(const Frame &frame):
        mpORBvocabulary(frame.mpORBvocabulary), mpORBextractorLeft(frame.mpORBextractorLeft), 
        mpORBextractorRight(frame.mpORBextractorRight), mTimeStamp(frame.mTimeStamp), mK(frame.mK.clone()), 
        mDistCoef(frame.mDistCoef.clone()), mbf(frame.mbf), mb(frame.mb), mThDepth(frame.mThDepth), N(frame.N),
        mvKeys(frame.mvKeys), mvKeysRight(frame.mvKeysRight), mvKeysUn(frame.mvKeysUn), mvuRight(frame.mvuRight),
        mvDepth(frame.mvDepth),mBowVec(frame.mBowVec), mFeatVec(frame.mFeatVec),
        mDescriptors(frame.mDescriptors.clone()), mDescriptorsRight(frame.mDescriptorsRight.clone()),
        mvpMapPoints(frame.mvpMapPoints), mvbOutlier(frame.mvbOutlier), mnId(frame.mnId),
        mpReferenceKF(frame.mpReferenceKF), mnScaleLevels(frame.mnScaleLevels),
        mfScaleFactor(frame.mfScaleFactor), mfLogScaleFactor(frame.mfLogScaleFactor),
        mvScaleFactors(frame.mvScaleFactors), mvInvScaleFactors(frame.mvInvScaleFactors),
        mvLevelSigma2(frame.mvLevelSigma2), mvInvLevelSigma2(frame.mvInvLevelSigma2) 
    {

        for(int i=0; i<FRAME_GRID_COLS; i++)
            for(int j=0; j<FRAME_GRID_ROWS; j++)
                mGrid[i][j] = frame.mGrid[i][j];

        if(!frame.mTcw.empty())
            SetPose(frame.mTcw);

		mvIMUDataSinceLastFrame = frame.mvIMUDataSinceLastFrame;
		mNavState = frame.GetNavState();
		mMargCovInv = frame.mMargCovInv;
		mNavStatePrior = frame.mNavStatePrior;
    }
    
    
    
    // 双目的初始化
    Frame::Frame(const cv::Mat &imLeft, const cv::Mat &imRight, const double &timeStamp, ORBextractor* extractorLeft, ORBextractor* extractorRight, ORBVocabulary* voc, cv::Mat &K, cv::Mat &distCoef, const float &bf, const float &thDepth)
        :mpORBvocabulary(voc),mpORBextractorLeft(extractorLeft),mpORBextractorRight(extractorRight), mTimeStamp(timeStamp), mK(K.clone()),mDistCoef(distCoef.clone()), mbf(bf), mb(0), mThDepth(thDepth),
         mpReferenceKF(static_cast<KeyFrame*>(NULL))
    {
        // Frame ID
        mnId=nNextId++;

        // Scale Level Info
        mnScaleLevels = mpORBextractorLeft->GetLevels();
        mfScaleFactor = mpORBextractorLeft->GetScaleFactor();
        mfLogScaleFactor = log(mfScaleFactor);
        mvScaleFactors = mpORBextractorLeft->GetScaleFactors();
        mvInvScaleFactors = mpORBextractorLeft->GetInverseScaleFactors();
        mvLevelSigma2 = mpORBextractorLeft->GetScaleSigmaSquares();
        mvInvLevelSigma2 = mpORBextractorLeft->GetInverseScaleSigmaSquares();

        // ORB extraction
        // 同时对左右目提特征
        thread threadLeft(&Frame::ExtractORB,this,0,imLeft);
        thread threadRight(&Frame::ExtractORB,this,1,imRight);
        threadLeft.join();
        threadRight.join();

        if(mvKeys.empty())
            return;

        N = mvKeys.size();

        // Undistort特征点，这里没有对双目进行校正，因为要求输入的图像已经进行极线校正
        UndistortKeyPoints();

        // 计算双目间的匹配, 匹配成功的特征点会计算其深度
        // 深度存放在mvuRight 和 mvDepth 中
        ComputeStereoMatches();

        // 对应的mappoints
        mvpMapPoints = vector<MapPoint*>(N,static_cast<MapPoint*>(NULL));   
        mvbOutlier = vector<bool>(N,false);


        // This is done only for the first Frame (or after a change in the calibration)
        if(mbInitialComputations)
        {
            ComputeImageBounds(imLeft);

            mfGridElementWidthInv=static_cast<float>(FRAME_GRID_COLS)/(mnMaxX-mnMinX);
            mfGridElementHeightInv=static_cast<float>(FRAME_GRID_ROWS)/(mnMaxY-mnMinY);

            fx = K.at<float>(0,0);
            fy = K.at<float>(1,1);
            cx = K.at<float>(0,2);
            cy = K.at<float>(1,2);
            invfx = 1.0f/fx;
            invfy = 1.0f/fy;

            mbInitialComputations=false;
        }

        mb = mbf/fx;

        AssignFeaturesToGrid();
    }

    // RGBD初始化
    Frame::Frame(const cv::Mat &imGray, const cv::Mat &imDepth, const double &timeStamp, ORBextractor* extractor,ORBVocabulary* voc, cv::Mat &K, cv::Mat &distCoef, const float &bf, const float &thDepth)
        :mpORBvocabulary(voc),mpORBextractorLeft(extractor),mpORBextractorRight(static_cast<ORBextractor*>(NULL)),
         mTimeStamp(timeStamp), mK(K.clone()),mDistCoef(distCoef.clone()), mbf(bf), mThDepth(thDepth)
    {
        // Frame ID
        mnId=nNextId++;

        // Scale Level Info
        mnScaleLevels = mpORBextractorLeft->GetLevels();
        mfScaleFactor = mpORBextractorLeft->GetScaleFactor();    
        mfLogScaleFactor = log(mfScaleFactor);
        mvScaleFactors = mpORBextractorLeft->GetScaleFactors();
        mvInvScaleFactors = mpORBextractorLeft->GetInverseScaleFactors();
        mvLevelSigma2 = mpORBextractorLeft->GetScaleSigmaSquares();
        mvInvLevelSigma2 = mpORBextractorLeft->GetInverseScaleSigmaSquares();

        // ORB extraction
        ExtractORB(0,imGray);

        N = mvKeys.size();

        if(mvKeys.empty())
            return;

        UndistortKeyPoints();

        ComputeStereoFromRGBD(imDepth);

        mvpMapPoints = vector<MapPoint*>(N,static_cast<MapPoint*>(NULL));
        mvbOutlier = vector<bool>(N,false);

        // This is done only for the first Frame (or after a change in the calibration)
        if(mbInitialComputations)
        {
            ComputeImageBounds(imGray);

            mfGridElementWidthInv=static_cast<float>(FRAME_GRID_COLS)/static_cast<float>(mnMaxX-mnMinX);
            mfGridElementHeightInv=static_cast<float>(FRAME_GRID_ROWS)/static_cast<float>(mnMaxY-mnMinY);

            fx = K.at<float>(0,0);
            fy = K.at<float>(1,1);
            cx = K.at<float>(0,2);
            cy = K.at<float>(1,2);
            invfx = 1.0f/fx;
            invfy = 1.0f/fy;

            mbInitialComputations=false;
        }

        mb = mbf/fx;

        AssignFeaturesToGrid();
    }



    // 单目初始化构造函数。
    Frame::Frame(const cv::Mat &imGray, const double &timeStamp, ORBextractor *extractor, ORBVocabulary *voc,
                    cv::Mat &K, cv::Mat &distCoef, const float &bf, const float &thDepth):
        mpORBvocabulary(voc), mpORBextractorLeft(extractor), mpORBextractorRight(static_cast<ORBextractor *>(NULL)),
        mTimeStamp(timeStamp), mK(K.clone()), mDistCoef(distCoef.clone()), mbf(bf), mThDepth(thDepth)
    {

        // 帧ID。
        mnId = nNextId++;

        // 尺度信息。
        mnScaleLevels = mpORBextractorLeft->GetLevels();
        mfScaleFactor = mpORBextractorLeft->GetScaleFactor();
        mfLogScaleFactor = log(mfScaleFactor);
        mvScaleFactors = mpORBextractorLeft->GetScaleFactors();
        mvInvScaleFactors = mpORBextractorLeft->GetInverseScaleFactors();
        mvLevelSigma2 = mpORBextractorLeft->GetScaleSigmaSquares();
        mvInvLevelSigma2 = mpORBextractorLeft->GetInverseScaleSigmaSquares();

        // ORB特增提取。
        ExtractORB(0, imGray);

        N = mvKeys.size();

        if(mvKeys.empty())
            return;


        // 调用OpenCV的矫正函数，矫正ORB提取的特征点。
        UndistortKeyPoints();

        // 设定标志位，表示无立体匹配信息。
        mvuRight = vector<float>(N, -1);
        mvDepth = vector<float>(N, -1);
	
        mvpMapPoints = vector<MapPoint *>(N, static_cast<MapPoint *>(NULL));
        mvbOutlier = vector<bool>(N, false);

        // 仅对于第一帧或标定参数变化时。
        if(mbInitialComputations)
        {
            ComputeImageBounds(imGray);

            // 每个小栅格的长宽（像素坐标）的倒数，坐标×mfGrid*  表示栅格编号。
            mfGridElementWidthInv = static_cast<float>(FRAME_GRID_COLS)/static_cast<float>(mnMaxX-mnMinX);
            mfGridElementHeightInv = static_cast<float>(FRAME_GRID_ROWS)/static_cast<float>(mnMaxY-mnMinY);

            fx = K.at<float>(0,0);
            fy = K.at<float>(1,1);
            cx = K.at<float>(0,2);
            cy = K.at<float>(1,2);
            invfx = 1.0f/fx;
            invfy = 1.0f/fy;

            mbInitialComputations = false;
        }

        mb = mbf/fx;
        AssignFeaturesToGrid();

    }



    // 分配特征点到栅格。
    void Frame::AssignFeaturesToGrid()
    {
        int nReserve = 0.5f*N/(FRAME_GRID_COLS*FRAME_GRID_ROWS);
        
        // 分配存储空间。
        for(unsigned int i=0; i<FRAME_GRID_COLS; i++)
            for(unsigned int j=0; j<FRAME_GRID_ROWS; j++)
                mGrid[i][j].reserve(nReserve);

        // 在mGrid中记录特征点编号。
        for(int i=0; i<N; i++)
        {
            const cv::KeyPoint &kp = mvKeysUn[i];

            int nGridPosX, nGridPosY;
            if(PosInGrid(kp, nGridPosX, nGridPosY))
                mGrid[nGridPosX][nGridPosY].push_back(i);
        }

    }



    // 提取ORB特征。
    void Frame::ExtractORB(int flag, const cv::Mat &im)
    {
        // 单目和RGBD。
        if (flag == 0)
            (*mpORBextractorLeft)(im, cv::Mat(), mvKeys, mDescriptors);
        // 双目。
        else
            (*mpORBextractorRight)(im, cv::Mat(), mvKeysRight, mDescriptorsRight);

    }



    // 设置相机姿态，并通过UpdatePoseMatrices()更新mRcw, mRwc。
    void Frame::SetPose(cv::Mat Tcw)
    {
        mTcw = Tcw.clone();
        UpdatePoseMatrices();

    }



    // 根据mTcw计算旋转矩阵mRcw，mRwc，位移mtcw, 相机知心mOw。
    void Frame::UpdatePoseMatrices()
    {

        // 坐标为齐次， [x_camera 1] = [R|t] [x_world 1]。
        mRcw = mTcw.rowRange(0,3).colRange(0,3);
        mRwc = mRcw.t();
        // mtcw相机坐标系下，世界坐标系原点的坐标，相机坐标系指向世界坐标系。
        mtcw = mTcw.rowRange(0,3).col(3);
        // mOw，世界坐标系下相机坐标系原点的坐标，世界坐标系指向相机坐标系。
        mOw = -mRcw.t()*mtcw;

    }



    /* 判断地图点是否在视野范围内。
    * 计算重投影坐标，观测方向夹角，预测在当前帧的尺度。
    * @param pMP 地图点云。   
    * @param viewingCosLimit 视角和平均视角方向的阈值。
    * @return       在视野范围内返回true。
    * 被Tracking::SearchLocalPoints()调用。
    */
    bool Frame::isInFrustum(MapPoint *pMP, float viewingCosLimit)
    {
        pMP->mbTrackInView = false;

        // 地图点的世界坐标。
        cv::Mat P = pMP->GetWorldPos();
        
        // 地图点在相机坐标系下的坐标。 
        const cv::Mat Pc = mRcw*P + mtcw;
        const float &PcX = Pc.at<float>(0);
        const float &PcY = Pc.at<float>(1);
        const float &PcZ = Pc.at<float>(2);

        // 检查地图点深度>0 
        if(PcZ < 0.0f)
            return false;

        // 判据1 将地图点投影到当前帧的像素坐标，判断是否在图像内。
        const float invz = 1.0f/PcZ;
        const float u = fx*PcX*invz+cx;
        const float v = fy*PcY*invz+cy;
        if(u<mnMinX || u>mnMaxX)
            return false;
        if(v<mnMinY || v>mnMaxY)
            return false;


        // 判据3 计算MapPoint到相机中心的距离，判断是否在尺度变换范围内。
        const float maxDistance = pMP->GetMaxDistanceInvariance();
        const float minDistance = pMP->GetMinDistanceInvariance();
        // 世界坐标系下，地图点到相机的向量，相机指向地图点。
        const cv::Mat PO = P-mOw;
        const float dist = cv::norm(PO);
        if(dist<minDistance || dist>maxDistance)
            return false;


        // 判据2 计算当前视角和平均视角的余弦值，若视角>60度(余弦值<cos60)，返回false。 
        cv::Mat Pn = pMP->GetNormal();
        const float viewCos = PO.dot(Pn)/dist;
        if(viewCos < viewingCosLimit)
            return false;

        // 判据4 根据深度预测尺度(对应特征点在一层)。
        const int nPredictedLevel = pMP->PredictScale(dist, mfLogScaleFactor);
        if(nPredictedLevel>=mnScaleLevels || nPredictedLevel<0)
            return false;

        // 标记该地图点，用于Tracking。
        pMP->mbTrackInView = true;
        pMP->mTrackProjX = u;
        pMP->mTrackProjXR = u - mbf*invz;   // 投影到双目右侧相机的横坐标上。
        pMP->mTrackProjY = v;
        pMP->mnTrackScaleLevel = nPredictedLevel;
        pMP->mTrackViewCos = viewCos;

        return true;

    }



    /* 找到以x,y为中心，边长为2r的方形内且尺度在[minLevel, maxLevel]的特征点。
    *@param
    *  x    图像坐标u
    *  y    图像坐标v
    *  r    边长r
    *  minLevel     最小尺度。
    *  maxLevel     最大尺度。
    *@return       满足条件的特征序号。
    */
    vector<size_t> Frame::GetFeaturesInArea(const float &x, const float &y, const float &r, const int minLevel, const int maxLevel) const
    {

        vector<size_t> vIndices;
        vIndices.reserve(N);

        // 确定栅格编号范围。
        const int nMinCellX = max(0, (int)floor((x-mnMinX-r)*mfGridElementWidthInv));
        if(nMinCellX>=FRAME_GRID_COLS)
            return vIndices;

        const int nMaxCellX = min((int)FRAME_GRID_COLS-1, (int)ceil((x-mnMinX+r)*mfGridElementWidthInv));
        if(nMaxCellX<0)
            return vIndices;

        const int nMinCellY = max(0, (int)floor((y-mnMinY-r)*mfGridElementHeightInv)); 
        if(nMinCellY>=FRAME_GRID_ROWS)
            return vIndices;

        const int nMaxCellY = min((int)FRAME_GRID_ROWS-1, (int)ceil((y-mnMinY+r)*mfGridElementHeightInv));
        if(nMaxCellY<0)
            return vIndices;

        const bool bCheckLevels = (minLevel>0) || (maxLevel>=0);

        for(int ix = nMinCellX; ix<=nMaxCellX; ix++)
        {
            for(int iy = nMinCellY; iy<=nMaxCellY; iy++)
            {
                const vector<size_t> vCell = mGrid[ix][iy];
                if(vCell.empty())
                    continue;

                // 遍历格子中的MP。
                for(size_t j=0, jend=vCell.size(); j<jend; j++)
                {
                    const cv::KeyPoint &kpUn = mvKeysUn[vCell[j]];
                    if(bCheckLevels)
                    {
                        if(kpUn.octave<minLevel)
                            continue;
                        if(maxLevel>=0)
                            if(kpUn.octave>maxLevel)
                                continue;
                    }

                    const float distx = kpUn.pt.x-x;
                    const float disty = kpUn.pt.y-y;

                    if(fabs(distx)<r && fabs(disty)<r)
                        vIndices.push_back(vCell[j]);
                }
            }
        }

        return vIndices;

    }



    // 校正后的特征点是否在图像内。
    bool Frame::PosInGrid(const cv::KeyPoint &kp, int &posX, int &posY)
    {
        posX = round((kp.pt.x-mnMinX)*mfGridElementWidthInv);
        posY = round((kp.pt.y-mnMinY)*mfGridElementHeightInv);

        // 特征点坐标被校正后，可能导致不在图像内。
        if(posX<0 || posX>=FRAME_GRID_COLS || posY<0 || posY>=FRAME_GRID_ROWS)
            return false;

        return true;

    }



    // 计算词典mBowVec和mFeatVec，其中mFeatVec记录了属于第i个node（在第4层）的ni个描述子。
    void Frame::ComputeBoW()
    {
        if(mBowVec.empty())
        {
            vector<cv::Mat> vCurrentDesc = Converter::toDescriptorVector(mDescriptors);
            mpORBvocabulary->transform(vCurrentDesc, mBowVec, mFeatVec, 4);
        }

    }



    // 调用OpenCV的矫正函数校正ORB提取的特征点。
    void Frame::UndistortKeyPoints()
    {
        // 如果图像没有失真。
        if(mDistCoef.at<float>(0)==0.0)
        {
            mvKeysUn = mvKeys;
            return;
        }

        // N是提取的特征点数量，将N个特征点保存在N*2的mat中。
        cv::Mat mat(N,2,CV_32F);
        for(int i=0; i<N; i++)
        {
            mat.at<float>(i,0) = mvKeys[i].pt.x;
            mat.at<float>(i,1) = mvKeys[i].pt.y;
        }

        // 调整mat的通道为2，矩阵的形状不变。
        mat = mat.reshape(2);
        cv::undistortPoints(mat, mat, mK, mDistCoef, cv::Mat(), mK);    // 使用cv的函数进行失真校正。
        mat = mat.reshape(1);

        // 储存校正后的特征点。
        mvKeysUn.resize(N);
        for(int i=0; i<N; i++)
        {
            cv::KeyPoint kp = mvKeys[i];
            kp.pt.x = mat.at<float>(i,0);
            kp.pt.y = mat.at<float>(i,1);
            mvKeysUn[i] = kp;
        }

    }



    // 计算图像的边界。
    void Frame::ComputeImageBounds(const cv::Mat &imLeft)
    {
        // 图像需要校正。
        if(mDistCoef.at<float>(0) != 0.0)
        {
            // 矫正前4个边界点，(0,0)，(cols,0)，(0,rows)，(cols,rows)。
            cv::Mat mat(4,2,CV_32F);
            // 图像左上点。
            mat.at<float>(0,0) = 0.0;
            mat.at<float>(0,1) = 0.0;
            // 图像右上点。
            mat.at<float>(1,0) = imLeft.cols;
            mat.at<float>(1,1) = 0.0;
            // 图像左下点。
            mat.at<float>(2,0) = 0.0;
            mat.at<float>(2,1) = imLeft.rows;
            // 图像右下点。
            mat.at<float>(3,0) = imLeft.cols;
            mat.at<float>(3,1) = imLeft.rows;

            // 图像校正。
            mat = mat.reshape(2);
            cv::undistortPoints(mat, mat, mK, mDistCoef, cv::Mat(), mK);
            mat = mat.reshape(1);

            // 图像左上和左下横坐标最小。
            mnMinX = min(mat.at<float>(0,0), mat.at<float>(2,0)); 
            // 图像右上和右下横坐标最大。
            mnMaxX = max(mat.at<float>(1,0), mat.at<float>(3,0));
            // 图像左上和右上纵坐标最小。
            mnMinY = min(mat.at<float>(0,1), mat.at<float>(1,1));
            // 图像左下和右下纵坐标最小。
            mnMaxY = max(mat.at<float>(2,1), mat.at<float>(3,1));
        }

        // 图像不需要校正。
        else
        {
            mnMinX = 0.0f;
            mnMaxX = imLeft.cols;
            mnMinY = 0.0f;
            mnMaxY = imLeft.rows;
        }

    }



    // 以下3个函数用于双目或RGBD传感器。    
    // ComputeStereoMatches，ComputeStereoFromRGBD，UnprojectStereo。
    
    
    /**
    * @brief 双目匹配
    *
    * 为左图的每一个特征点在右图中找到匹配点 \n
    * 根据基线(有冗余范围)上描述子距离找到匹配, 再进行SAD精确定位 \n
    * 最后对所有SAD的值进行排序, 剔除SAD值较大的匹配对，然后利用抛物线拟合得到亚像素精度的匹配 \n
    * 匹配成功后会更新 mvuRight(ur) 和 mvDepth(Z)
    */
    void Frame::ComputeStereoMatches()
    {
        mvuRight = vector<float>(N,-1.0f);
        mvDepth = vector<float>(N,-1.0f);

        const int nRows = mpORBextractorLeft->mvImagePyramid[0].rows;

        //Assign keypoints to row table
        // 步骤1：建立特征点搜索范围对应表，一个特征点在一个带状区域内搜索匹配特征点
        // 匹配搜索的时候，不仅仅是在一条横线上搜索，而是在一条横向搜索带上搜索,简而言之，原本每个特征点的纵坐标为1，这里把特征点体积放大，纵坐标占好几行
        // 例如左目图像某个特征点的纵坐标为20，那么在右侧图像上搜索时是在纵坐标为18到22这条带上搜索，搜索带宽度为正负2，搜索带的宽度和特征点所在金字塔层数有关
        // 简单来说，如果纵坐标是20，特征点在图像第20行，那么认为18 19 20 21 22行都有这个特征点
        // vRowIndices[18]、vRowIndices[19]、vRowIndices[20]、vRowIndices[21]、vRowIndices[22]都有这个特征点编号
        vector<vector<size_t> > vRowIndices(nRows,vector<size_t>());

        for(int i=0; i<nRows; i++)
            vRowIndices[i].reserve(200);

        const int Nr = mvKeysRight.size();

        for(int iR=0; iR<Nr; iR++)
        {
            // !!在这个函数中没有对双目进行校正，双目校正是在外层程序中实现的
            const cv::KeyPoint &kp = mvKeysRight[iR];
            const float &kpY = kp.pt.y;
            // 计算匹配搜索的纵向宽度，尺度越大（层数越高，距离越近），搜索范围越大
            // 如果特征点在金字塔第一层，则搜索范围为:正负2
            // 尺度越大其位置不确定性越高，所以其搜索半径越大
            const float r = 2.0f*mvScaleFactors[mvKeysRight[iR].octave];
            const int maxr = ceil(kpY+r);
            const int minr = floor(kpY-r);

            for(int yi=minr;yi<=maxr;yi++)
                vRowIndices[yi].push_back(iR);
        }

        // Set limits for search
        const float minZ = mb;        // NOTE bug mb没有初始化，mb的赋值在构造函数中放在ComputeStereoMatches函数的后面
        const float minD = -3;        // 最小视差, 设置为0即可
        const float maxD = mbf/minZ;  // 最大视差, 对应最小深度 mbf/minZ = mbf/mb = mbf/(mbf/fx) = fx (wubo???)

        // For each left keypoint search a match in the right image
        vector<pair<int, int> > vDistIdx;
        vDistIdx.reserve(N);

        // 步骤2：对左目相机每个特征点，通过描述子在右目带状搜索区域找到匹配点, 再通过SAD做亚像素匹配
        // 注意：这里是校正前的mvKeys，而不是校正后的mvKeysUn
        // KeyFrame::UnprojectStereo和Frame::UnprojectStereo函数中不一致
        // 这里是不是应该对校正后特征点求深度呢？(wubo???)
        for(int iL=0; iL<N; iL++)
        {
            const cv::KeyPoint &kpL = mvKeys[iL];
            const int &levelL = kpL.octave;
            const float &vL = kpL.pt.y;
            const float &uL = kpL.pt.x;

            // 可能的匹配点
            const vector<size_t> &vCandidates = vRowIndices[vL];

            if(vCandidates.empty())
                continue;

            const float minU = uL-maxD; // 最小匹配范围
            const float maxU = uL-minD; // 最大匹配范围

            if(maxU<0)
                continue;

            int bestDist = ORBmatcher::TH_HIGH;
            size_t bestIdxR = 0;

            // 每个特征点描述子占一行，建立一个指针指向iL特征点对应的描述子
            const cv::Mat &dL = mDescriptors.row(iL);

            // Compare descriptor to right keypoints
            // 步骤2.1：遍历右目所有可能的匹配点，找出最佳匹配点（描述子距离最小）
            for(size_t iC=0; iC<vCandidates.size(); iC++)
            {
                const size_t iR = vCandidates[iC];
                const cv::KeyPoint &kpR = mvKeysRight[iR];

                // 仅对近邻尺度的特征点进行匹配
                if(kpR.octave<levelL-1 || kpR.octave>levelL+1)
                    continue;

                const float &uR = kpR.pt.x;

                if(uR>=minU && uR<=maxU)
                {
                    const cv::Mat &dR = mDescriptorsRight.row(iR);
                    const int dist = ORBmatcher::DescriptorDistance(dL,dR);

                    if(dist<bestDist)
                    {
                        bestDist = dist;
                        bestIdxR = iR;
                    }
                }
            }
            // 最好的匹配的匹配误差存在bestDist，匹配点位置存在bestIdxR中

            // Subpixel match by correlation
            // 步骤2.2：通过SAD匹配提高像素匹配修正量bestincR
            if(bestDist<ORBmatcher::TH_HIGH)
            {
                // coordinates in image pyramid at keypoint scale
                // kpL.pt.x对应金字塔最底层坐标，将最佳匹配的特征点对尺度变换到尺度对应层 (scaleduL, scaledvL) (scaleduR0, )
                const float uR0 = mvKeysRight[bestIdxR].pt.x;
                const float scaleFactor = mvInvScaleFactors[kpL.octave];
                const float scaleduL = round(kpL.pt.x*scaleFactor);
                const float scaledvL = round(kpL.pt.y*scaleFactor);
                const float scaleduR0 = round(uR0*scaleFactor);

                // sliding window search
                const int w = 5; // 滑动窗口的大小11*11 注意该窗口取自resize后的图像
                cv::Mat IL = mpORBextractorLeft->mvImagePyramid[kpL.octave].rowRange(scaledvL-w,scaledvL+w+1).colRange(scaleduL-w,scaleduL+w+1);
                IL.convertTo(IL,CV_32F);
                IL = IL - IL.at<float>(w,w) * cv::Mat::ones(IL.rows,IL.cols,CV_32F);//窗口中的每个元素减去正中心的那个元素，简单归一化，减小光照强度影响

                int bestDist = INT_MAX;
                int bestincR = 0;
                const int L = 5;
                vector<float> vDists;
                vDists.resize(2*L+1); // 11

                // 滑动窗口的滑动范围为（-L, L）,提前判断滑动窗口滑动过程中是否会越界
                const float iniu = scaleduR0+L-w; //这个地方是否应该是scaleduR0-L-w (wubo???)
                const float endu = scaleduR0+L+w+1;
                if(iniu<0 || endu >= mpORBextractorRight->mvImagePyramid[kpL.octave].cols)
                    continue;

                for(int incR=-L; incR<=+L; incR++)
                {
                    // 横向滑动窗口
                    cv::Mat IR = mpORBextractorRight->mvImagePyramid[kpL.octave].rowRange(scaledvL-w,scaledvL+w+1).colRange(scaleduR0+incR-w,scaleduR0+incR+w+1);
                    IR.convertTo(IR,CV_32F);
                    IR = IR - IR.at<float>(w,w) * cv::Mat::ones(IR.rows,IR.cols,CV_32F);//窗口中的每个元素减去正中心的那个元素，简单归一化，减小光照强度影响

                    float dist = cv::norm(IL,IR,cv::NORM_L1); // 一范数，计算差的绝对值
                    if(dist<bestDist)
                    {
                        bestDist =  dist;// SAD匹配目前最小匹配偏差
                        bestincR = incR; // SAD匹配目前最佳的修正量
                    }

                    vDists[L+incR] = dist; // 正常情况下，这里面的数据应该以抛物线形式变化
                }

                if(bestincR==-L || bestincR==L) // 整个滑动窗口过程中，SAD最小值不是以抛物线形式出现，SAD匹配失败，同时放弃求该特征点的深度
                    continue;

                // Sub-pixel match (Parabola fitting)
                // 步骤2.3：做抛物线拟合找谷底得到亚像素匹配deltaR
                // (bestincR,dist) (bestincR-1,dist) (bestincR+1,dist)三个点拟合出抛物线
                // bestincR+deltaR就是抛物线谷底的位置，相对SAD匹配出的最小值bestincR的修正量为deltaR
                const float dist1 = vDists[L+bestincR-1];
                const float dist2 = vDists[L+bestincR];
                const float dist3 = vDists[L+bestincR+1];

                const float deltaR = (dist1-dist3)/(2.0f*(dist1+dist3-2.0f*dist2));

                // 抛物线拟合得到的修正量不能超过一个像素，否则放弃求该特征点的深度
                if(deltaR<-1 || deltaR>1)
                    continue;

                // Re-scaled coordinate
                // 通过描述子匹配得到匹配点位置为scaleduR0
                // 通过SAD匹配找到修正量bestincR
                // 通过抛物线拟合找到亚像素修正量deltaR
                float bestuR = mvScaleFactors[kpL.octave]*((float)scaleduR0+(float)bestincR+deltaR);

                // 这里是disparity，根据它算出depth
                float disparity = (uL-bestuR);

                if(disparity>=0 && disparity<maxD) // 最后判断视差是否在范围内
                {
                    if(disparity<=0)
                    {
                        disparity=0.01;
                        bestuR = uL-0.01;
                    }
                    // depth 是在这里计算的
                    // depth=baseline*fx/disparity
                    mvDepth[iL]=mbf/disparity;   // 深度
                    mvuRight[iL] = bestuR;       // 匹配对在右图的横坐标
                    vDistIdx.push_back(pair<int,int>(bestDist,iL)); // 该特征点SAD匹配最小匹配偏差
                }
            }
        }

        // 步骤3：剔除SAD匹配偏差较大的匹配特征点
        // 前面SAD匹配只判断滑动窗口中是否有局部最小值，这里通过对比剔除SAD匹配偏差比较大的特征点的深度
        sort(vDistIdx.begin(),vDistIdx.end()); // 根据所有匹配对的SAD偏差进行排序, 距离由小到大
        const float median = vDistIdx[vDistIdx.size()/2].first;
        const float thDist = 1.5f*1.4f*median; // 计算自适应距离, 大于此距离的匹配对将剔除

        for(int i=vDistIdx.size()-1;i>=0;i--)
        {
            if(vDistIdx[i].first<thDist)
                break;
            else
            {
                mvuRight[vDistIdx[i].second]=-1;
                mvDepth[vDistIdx[i].second]=-1;
            }
        }
    }



    void Frame::ComputeStereoFromRGBD(const cv::Mat &imDepth)
    {
        // mvDepth直接由depth图像读取
        mvuRight = vector<float>(N,-1);
        mvDepth = vector<float>(N,-1);

        for(int i=0; i<N; i++)
        {
            const cv::KeyPoint &kp = mvKeys[i];
            const cv::KeyPoint &kpU = mvKeysUn[i];

            const float &v = kp.pt.y;
            const float &u = kp.pt.x;

            const float d = imDepth.at<float>(v,u);

            if(d>0)
            {
                mvDepth[i] = d;
                mvuRight[i] = kpU.pt.x-mbf/d;
            }
        }
    }



    /**
    * @brief Backprojects a keypoint (if stereo/depth info available) into 3D world coordinates.
    * @param  i 第i个keypoint
    * @return   3D点（相对于世界坐标系）
    */
    cv::Mat Frame::UnprojectStereo(const int &i)
    {
        // KeyFrame::UnprojectStereo
        // mvDepth是在ComputeStereoMatches函数中求取的
        // mvDepth对应的校正前的特征点，可这里却是对校正后特征点反投影
        // KeyFrame::UnprojectStereo中是对校正前的特征点mvKeys反投影
        // 在ComputeStereoMatches函数中应该对校正后的特征点求深度？？ (wubo???)
        const float z = mvDepth[i];
        if(z>0)
        {
            const float u = mvKeysUn[i].pt.x;
            const float v = mvKeysUn[i].pt.y;
            const float x = (u-cx)*z*invfx;
            const float y = (v-cy)*z*invfy;
            cv::Mat x3Dc = (cv::Mat_<float>(3,1) << x, y, z);
            return mRwc*x3Dc+mOw;
        }
        else
            return cv::Mat();
    }




}   // namespace ORB_SLAM2




