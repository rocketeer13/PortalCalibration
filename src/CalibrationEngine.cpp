#include "CalibrationEngine.h"

CalibrationEngine::CalibrationEngine(const int horizontalCount, const int verticalCount) :
  m_boardSize( horizontalCount, verticalCount ),
  m_boardMarkerCount( horizontalCount * verticalCount ),
  m_markerDiameter( .5 )
{ }

void CalibrationEngine::CalibrateCamera(shared_ptr<lens::ICamera> capture, int requestedSamples)
{
  // Grab views and place them in the matrixes
  auto objectPoints = CalculateObjectPoints( ); 
  auto imagePoints = GrabCameraImagePoints(capture, requestedSamples);

  // Calibrate for intrinsics
  auto calibrationData = CalibrateView( objectPoints, imagePoints, cv::Size( capture->getWidth( ), capture->getHeight( ) ) );
  
  // Calibrate for extrinsics
  imagePoints = GrabCameraImagePoints(capture, 1); // Only use 1 since we are capturing for 1 view
  CalibrateExtrinsic( objectPoints, imagePoints, calibrationData );
}

void CalibrationEngine::CalibrateProjector(shared_ptr<lens::ICamera> capture, shared_ptr<IProjector> projector, int requestedSamples)
{
  // Grab views for the projector
  auto objectPoints = CalculateObjectPoints( );
  auto imagePoints = GrabProjectorImagePoints( capture, projector, requestedSamples );

  // Calibrate for projector intrinsics
  auto calibrationData = CalibrateView( objectPoints, imagePoints, cv::Size( capture->getWidth(), capture->getHeight( ) ) );

  // Calibrate for extrinsics
  imagePoints = GrabProjectorImagePoints( capture, projector, 1 ); // Only using 1 since we are capturing for 1 view
  CalibrateExtrinsic( objectPoints, imagePoints, calibrationData );
}

vector<vector<cv::Point2f>> CalibrationEngine::GrabCameraImagePoints( shared_ptr<lens::ICamera> capture, int poses2Capture )
{
	int successes = 0;
	bool found = false;
	vector< vector< cv::Point2f > > imagePoints;
	vector< cv::Point2f > pointBuffer;

	// Create a display to give the user some feedback
	Display display("Calibration");
		
	// While we have boards to grab, grab-em
	while ( successes < poses2Capture )
	{
	  // Let the user know how many more images we need and how to capture
	  std::stringstream message;
	  message << "Press <Enter> to capture pose \n";
	  message << successes;
	  message << "/";
	  message << poses2Capture;
	  display.OverlayText( message.str() );

	  while ( m_userWaitKey != cvWaitKey( 15 ) )
	  {
		// Just display to the user. They are setting up the calibration board
		cv::Mat frame( capture->getFrame() );
		cv::drawChessboardCorners( frame, m_boardSize, cv::Mat( pointBuffer ), found );
		display.ShowImage( frame );
	  }

	  // User is ready, try and find the circles
	  pointBuffer.clear();
	  cv::Mat colorFrame( capture->getFrame( ) );
	  cv::Mat gray;
	  cv::cvtColor( colorFrame, gray, CV_BGR2GRAY);
	  found = cv::findCirclesGrid( gray, m_boardSize, pointBuffer, cv::CALIB_CB_ASYMMETRIC_GRID );

	  // Make sure we found it, and that we found all the points
	  if(found && pointBuffer.size() == m_boardMarkerCount)
	  {
		imagePoints.push_back(pointBuffer);
		++successes;
	  }
	} // End collection while loop

	return imagePoints;
}

vector<vector<cv::Point2f>> CalibrationEngine::GrabProjectorImagePoints(shared_ptr<lens::ICamera> capture, shared_ptr<IProjector> projector, int poses2Capture )
{
  	int successes = 0;
	bool found = false;
	vector< vector< cv::Point2f > > imagePoints;
	vector< cv::Point2f > pointBuffer;
	cv::Size projectorSize( projector->getWidth( ), projector->getHeight( ) );

	// Create a display to give the user some feedback
	Display display("Calibration");

	// While we have boards to grab, grab-em
	while ( successes < poses2Capture )
	{
	  // Let the user know how many more images we need and how to capture
	  std::stringstream message;
	  message << "Press <Enter> to capture pose \n";
	  message << successes;
	  message << "/";
	  message << poses2Capture;
	  display.OverlayText( message.str() );

	  while ( m_userWaitKey != cvWaitKey( 15 ) )
	  {
		// Just display to the user. They are setting up the calibration board
		cv::Mat frame( capture->getFrame() );
		cv::drawChessboardCorners( frame, m_boardSize, cv::Mat( pointBuffer ), found );
		display.ShowImage( frame );
	  }

	  // User is ready, try and find the circles
	  pointBuffer.clear();

	  // Project a white image so that it is easier to see the calibration board
	  cv::Mat whiteFrame( projectorSize, CV_8UC3, cv::Scalar(255,255,255));
	  projector->projectImage(whiteFrame);

	  // Look for the calibration board
	  cv::Mat colorFrame( capture->getFrame( ) );
	  cv::Mat gray;
	  cv::cvtColor( colorFrame, gray, CV_BGR2GRAY);
	  found = cv::findCirclesGrid( gray, m_boardSize, pointBuffer, cv::CALIB_CB_ASYMMETRIC_GRID );

	  // Make sure we found it, and that we found all the points
	  if(found && pointBuffer.size() == m_boardMarkerCount)
	  {
		// We found all the markers in the camera view. Now we need to image with the projector		
		auto horizontalUnwrappedPhase = ProjectAndCaptureUnwrappedPhase(capture, projector, IStructuredLight::Horizontal);
		auto verticalUnwrappedPhase = ProjectAndCaptureUnwrappedPhase(capture, projector, IStructuredLight::Vertical);

		vector< cv::Point2f > projectorPointBuffer;
		for(int coord = 0; coord < pointBuffer.size( ); ++coord)
		{
		  // Use the horizontal unwrapped phase to get the x and vertical for y
		  // TODO - -1.8 is not correct
		  projectorPointBuffer.push_back(cv::Point2f(
			InterpolateProjectorPosition(horizontalUnwrappedPhase.at<float>(pointBuffer[coord]), -1.8f, 70),
			InterpolateProjectorPosition(verticalUnwrappedPhase.at<float>(pointBuffer[coord]), -1.8f, 70)));
		}

		imagePoints.push_back(projectorPointBuffer);
		++successes;
	  }
	} // End collection while loop

	return imagePoints;
}

cv::Mat CalibrationEngine::ProjectAndCaptureUnwrappedPhase(shared_ptr<lens::ICamera> capture, shared_ptr<IProjector> projector, IStructuredLight::FringeDirection direction)
{
  // TODO - How do we know that we want to use 5? (Settings File?)
  NFringeStructuredLight	  fringeGenerator(5);
  TwoWavelengthPhaseUnwrapper phaseUnwrapper;
  vector<cv::Mat>			  wrappedPhase;
  cv::Size					  projectorSize( projector->getWidth( ), projector->getHeight( ) );

  // TODO - How do we know that we want to use 70 and 75? (Settings File?)
  auto smallWavelength = fringeGenerator.GenerateFringe(projectorSize, 70, IStructuredLight::Horizontal);
  wrappedPhase.push_back( ProjectAndCaptureWrappedPhase( capture, projector, smallWavelength ) );
  auto largerWavelength = fringeGenerator.GenerateFringe(projectorSize, 75, IStructuredLight::Horizontal);
  wrappedPhase.push_back( ProjectAndCaptureWrappedPhase( capture, projector, largerWavelength ) );
  return phaseUnwrapper.UnwrapPhase(wrappedPhase);
}

cv::Mat CalibrationEngine::ProjectAndCaptureWrappedPhase(shared_ptr<lens::ICamera> capture, shared_ptr<IProjector> projector, vector<cv::Mat> fringeImages)
{
  vector<cv::Mat> capturedFringes;
  cv::Mat gray;

  for(int patternNumber = 0; patternNumber < fringeImages.size(); ++patternNumber)
  {
	projector->projectImage(fringeImages[patternNumber]);
	cv::Mat colorFringe( capture->getFrame( ) );
	cv::cvtColor( colorFringe, gray, CV_BGR2GRAY );
	capturedFringes.push_back( gray );
  }

  NFringeStructuredLight phaseWrapper(fringeImages.size()); 
  return phaseWrapper.WrapPhase(capturedFringes);
}

CalibrationData CalibrationEngine::CalibrateView(vector<cv::Point3f> objectPoints, vector<vector<cv::Point2f>> imagePoints, cv::Size viewSize)
{
  // Start with the identity and it will get refined from there
  cv::Mat distortionCoefficients = cv::Mat::zeros(5, 1, CV_64F);
  cv::Mat intrinsicMatrix = cv::Mat::eye(3, 3, CV_64F);
  vector<cv::Mat> rotationVectors;
  vector<cv::Mat> translationVectors;
  
  vector<vector<cv::Point3f>> objectPointList;
  for(int i = 0; i < imagePoints.size(); ++i)
	{ objectPointList.push_back(objectPoints); }

  cv::calibrateCamera(objectPointList, imagePoints, viewSize, intrinsicMatrix, distortionCoefficients, rotationVectors, translationVectors, CV_CALIB_FIX_K4 | CV_CALIB_FIX_K5);

  CalibrationData data;
  data.SetDistortion(distortionCoefficients);
  data.SetIntrinsic(intrinsicMatrix);
  
  return data;
}

void CalibrationEngine::CalibrateExtrinsic(vector<cv::Point3f> objectPoints, vector<vector<cv::Point2f>> imagePoints, CalibrationData& calibrationData)
{
  cv::Mat rotationVector;
  cv::Mat translationVector;

  cv::solvePnP(objectPoints, imagePoints, calibrationData.GetIntrinsic(), calibrationData.GetDistortion(), rotationVector, translationVector);
  calibrationData.SetRotationVector(rotationVector);
}

vector<cv::Point3f> CalibrationEngine::CalculateObjectPoints()
{
  vector<cv::Point3f> objectPoints;

  for( int row = 0; row < m_boardSize.height; ++row )
  {
	for( int col = 0; col < m_boardSize.width; ++col )
	{
	  objectPoints.push_back( cv::Point3f( float(2.0 * col + row % 2) * m_markerDiameter,
										   float(row * m_markerDiameter),
										   0.0f));
	}
  }

  return objectPoints;
}

float CalibrationEngine::InterpolateProjectorPosition(float phi, float phi0, int pitch)
{
  return 1.0 + ( ( phi - phi0) / ( ( 2.0 * M_PI ) / float( pitch ) ) );
}