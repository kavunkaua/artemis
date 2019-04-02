#include <glog/logging.h>

#include <thread>

#include "Apriltag2Process.h"
#include "ResizeProcess.h"
#include "OutputProcess.h"
#include "AntCataloguerProcess.h"
#include "utils/FlagParser.h"
#include "utils/StringManipulation.h"
#include "EuresysFrameGrabber.h"
#include <EGrabber.h>

#include "utils/PosixCall.h"

#include <asio.hpp>

#include "ProcessQueueExecuter.h"


struct Options {
	bool PrintHelp;

	AprilTag2Detector::Config AprilTag2;
	CameraConfiguration       Camera;


	std::string Host;
	uint16_t    Port;
	bool        VideoOutputToStdout;
	size_t      VideoOutputHeight;
	std::string NewAntOuputDir;

	std::string frameIDString;
	size_t      FrameStride;
	std::set<uint64> FrameID;

	size_t      Workers;
};


void ParseArgs(int & argc, char ** argv,Options & opts ) {
	options::FlagParser parser(options::FlagParser::Default,"low-level vision detection for the FORmicidae Tracker");

	AprilTag2Detector::Config c;
	opts.VideoOutputHeight = 1080;
	opts.FrameStride = 1;
	opts.frameIDString = "";
	opts.Port = 3002;
	parser.AddFlag("help",opts.PrintHelp,"Print this help message",'h');

	parser.AddFlag("at-family",opts.AprilTag2.Family,"The apriltag2 family to use");
	parser.AddFlag("new-ant-roi-size", opts.AprilTag2.NewAntROISize, "Size of the image to save when a new ant is found");
	parser.AddFlag("at-quad-decimate",opts.AprilTag2.QuadDecimate,"Decimate original image for faster computation but worse pose estimation. Should be 1.0 (no decimation), 1.5, 2, 3 or 4");
	parser.AddFlag("at-quad-sigma",opts.AprilTag2.QuadSigma,"Apply a gaussian filter for quad detection, noisy image likes a slight filter like 0.8");
	parser.AddFlag("at-refine-edges",opts.AprilTag2.RefineEdges,"Refines the edge of the quad, especially needed if decimation is used, inexpensive");
	parser.AddFlag("at-no-refine-decode",opts.AprilTag2.NoRefineDecode,"Do not refines the tag code detection. Refining is often required for small tags");
	parser.AddFlag("at-refine-pose",opts.AprilTag2.RefinePose,"Refines the pose");
	parser.AddFlag("at-quad-min-cluster",opts.AprilTag2.QuadMinClusterPixel,"Minimum number of pixel to consider it a quad");
	parser.AddFlag("at-quad-max-n-maxima",opts.AprilTag2.QuadMaxNMaxima,"Number of candidate to consider to fit quad corner");
	parser.AddFlag("at-quad-critical-radian",opts.AprilTag2.QuadCriticalRadian,"Rejects quad with angle to close to 0 or 180 degrees");
	parser.AddFlag("at-quad-max-line-mse",opts.AprilTag2.QuadMaxLineMSE,"MSE threshold to reject a fitted quad");
	parser.AddFlag("at-quad-min-bw-diff",opts.AprilTag2.QuadMinBWDiff,"Difference in pixel value to consider a region black or white");
	parser.AddFlag("at-quad-deglitch",opts.AprilTag2.QuadDeglitch,"Deglitch only for noisy images");
	parser.AddFlag("host", opts.Host, "Host to send tag detection readout",'h');
	parser.AddFlag("port", opts.Port, "Port to send tag detection readout",'p');
	parser.AddFlag("video-to-stdout", opts.VideoOutputToStdout, "Sends video output to stdout");
	parser.AddFlag("video-output-height", opts.VideoOutputHeight, "Video Output height (width computed to maintain aspect ratio");
	parser.AddFlag("new-ant-output-dir",opts.NewAntOuputDir,"Path where to save new detected ant pictures");
	parser.AddFlag("frame-stride",opts.FrameStride,"Frame sequence length");
	parser.AddFlag("frame-ids",opts.frameIDString,"Frame ID to consider in the frame sequence, if empty consider all");
	parser.AddFlag("camera-fps",opts.Camera.FPS,"Camera FPS to use");
	parser.AddFlag("camera-exposure-us",opts.Camera.ExposureTime,"Camera Exposure time in us");
	parser.AddFlag("camera-strobe-us",opts.Camera.ExposureTime,"Camera Strobe Length in us");
	parser.AddFlag("camera-strobe-offset-us",opts.Camera.ExposureTime,"Camera Strobe Offset in us, negative value allowed");
	parser.AddFlag("camera-slave-mode",opts.Camera.Slave,"Use the camera in slave mode (CoaXPress Data Forwarding)");
	parser.AddFlag("workers",opts.Workers,"Number of worker to use for processing");

	parser.Parse(argc,argv);
	if (opts.PrintHelp == true) {
		parser.PrintUsage(std::cerr);
		exit(0);
	}
	if (opts.FrameStride == 0 ) {
		opts.FrameStride = 1;
	}
	if (opts.FrameStride > 100 ) {
		throw std::invalid_argument("Frame stride to big, mas is 100");
	}
	if (opts.Workers == 0 ) {
		opts.Workers = 1;
	}

	if ( opts.frameIDString.empty() ) {
		for(size_t i = 0; i < opts.FrameStride; ++i ) {
			opts.FrameID.insert(i);
		}
	} else {
		std::vector<std::string> IDs;
		base::SplitString(opts.frameIDString.cbegin(),
		                  opts.frameIDString.cend(),
		                  ",",
		                  std::back_inserter<std::vector<std::string>>(IDs));
		for (auto IDstr : IDs) {
			std::istringstream is(base::TrimSpaces(IDstr));
			uint64_t ID;
			is >> ID;
			if ( !is.good() && is.eof() == false ) {
				std::ostringstream os;
				os << "Cannot parse '" << IDstr << "'  in  '" << opts.frameIDString << "'";
				throw std::runtime_error(os.str());
			}
			if ( ID >= opts.FrameStride ) {
				std::ostringstream os;
				os << "Frame ID (" << ID << ") cannot be superior to frame stride (" << opts.FrameStride << ")";
				throw std::runtime_error(os.str());
			}
			opts.FrameID.insert(ID);
		}
	}
}


void Execute(int argc, char ** argv) {
	::google::InitGoogleLogging(argv[0]);
	Options opts;
	ParseArgs(argc, argv,opts);



	asio::io_service io;

	//Stops on SIGINT
	asio::signal_set signals(io,SIGINT);
	signals.async_wait([&io](const asio::error_code &,
	                         int ) {
		                   LOG(INFO) << "Terminating (SIGINT)";
		                   io.stop();
	                   });

	Connection::Ptr connection;
	if (!opts.Host.empty()) {
		connection = Connection::Create(io,opts.Host,opts.Port);
	}


	//creates queues
	ProcessQueue pq = AprilTag2Detector::Create(opts.AprilTag2,
	                                            connection);

	if ( !opts.NewAntOuputDir.empty() ) {
		pq.push_back(std::make_shared<AntCataloguerProcess>(io,opts.NewAntOuputDir,opts.AprilTag2.NewAntROISize));

	}
	//queues when outputting data
	if (opts.VideoOutputToStdout) {
		pq.push_back(std::make_shared<ResizeProcess>(opts.VideoOutputHeight));
		pq.push_back(std::make_shared<OutputProcess>(io));
	}

	Euresys::EGenTL gentl;
	EuresysFrameGrabber  fg(gentl,opts.Camera);
	ProcessQueueExecuter executer(io,opts.Workers);


	fort::FrameReadout error;

	std::function<void()> WaitForFrame = [&WaitForFrame,&io,&executer,&pq,&fg,&opts,&error,connection](){
		Frame::Ptr f = fg.NextFrame();
		if ( opts.FrameStride > 1 ) {
			uint64_t IDInStride = f->ID() % opts.FrameStride;
			if (opts.FrameID.count(IDInStride) == 0 ) {
				io.post(WaitForFrame);
				return;
			}
		}

		if ( executer.IsDone() == false ) {
			LOG(WARNING) << "Process overflow : skipping frame " << f->ID();
			if (connection) {

				error.Clear();
				error.set_timestamp(f->Timestamp());
				error.set_frameid(f->ID());
				auto time = error.mutable_time();
				time->set_seconds(f->Time().tv_sec);
				time->set_nanos(f->Time().tv_usec*1000);
				error.set_error(fort::FrameReadout::PROCESS_OVERFLOW);

				Connection::PostMessage(connection,error);
			}
			io.post(WaitForFrame);
			return;
		}
		executer.Start(pq,f);
		io.post(WaitForFrame);
	};

	fg.Start();
	io.post(WaitForFrame);
	std::vector<std::thread> threads;

	for(size_t i = 0; i < opts.Workers; ++i) {
		threads.push_back(std::thread([&io](){
					io.run();
				}));
	}

	io.run();
	fg.Stop();

	for( auto & t : threads) {
		t.join();
	}


}

int main(int argc, char ** argv) {
	try {
		Execute(argc,argv);
	} catch (const std::exception & e) {
		LOG(ERROR) << "Got uncaught exception: " << e.what();
		return 1;
	}
}
