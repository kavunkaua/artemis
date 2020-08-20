#include "ProcessFrameTask.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <artemis-config.h>

#include "Connection.hpp"
#include "ApriltagDetector.hpp"

#include <tbb/parallel_for.h>

namespace fort {
namespace artemis {

ProcessFrameTask::ProcessFrameTask(const Options & options,
                                   const VideoOutputTaskPtr & videoOutput,
                                   const UserInterfaceTaskPtr & userInterface,
                                   const ConnectionPtr & connection,
                                   const FullFrameExportTaskPtr & fullFrameExport,
                                   std::pair<size_t,size_t> inputResolution)
	: d_options(options)
	, d_videoOutput(videoOutput)
	, d_userInterface(userInterface)
	, d_connection(connection)
	, d_fullFrameExport(fullFrameExport)
	, d_maximumThreads(cv::getNumThreads()) {
	d_actualThreads = d_maximumThreads;

	if ( options.Apriltag.Family != tags::Family::Undefined ) {
		d_detector = std::make_unique<ApriltagDetector>(d_actualThreads,
		                                                cv::Size(inputResolution.first,
		                                                         inputResolution.second),
		                                                options.Apriltag);
	}

	int outputWidth = options.VideoOutput.OutputWidth(inputResolution.first,
	                                                  inputResolution.second);


	d_framePool.Reserve(DownscaledImagePerCycle() * ARTEMIS_FRAME_QUEUE_CAPACITY,
	                    outputWidth,
	                    options.VideoOutput.Height,
	                    CV_8UC1);
	d_nextAntCatalog = Time::Now();
	d_nextFrameExport = d_nextAntCatalog.Add(2 * Duration::Minute);
}


ProcessFrameTask::~ProcessFrameTask() {}

void ProcessFrameTask::TearDown() {
	// TODO: Close UserInterface connection
	// TODO: Close Frame Exporter
	// TODO: close video output
}


void ProcessFrameTask::Run() {

	Frame::Ptr frame;
	for (;;) {
		d_frameQueue.pop(frame);
		if ( !frame ) {
			break;
		}

		if ( true /* TODO: fullframeExport finished */ ) {
			d_actualThreads = d_maximumThreads;
			cv::setNumThreads(d_actualThreads);
		}

		ProcessFrameMandatory(frame);

		if ( d_frameQueue.size() > 0 ) {
			DropFrame(frame);
			continue;
		}

		ProcessFrame(frame);
	}

	TearDown();

}


void ProcessFrameTask::ProcessFrameMandatory(const Frame::Ptr & frame ) {
	if ( !d_videoOutput && !d_userInterface ) {
		return;
	}
	auto downscaled = d_framePool.Get();
	cv::resize(frame->ToCV(),*downscaled,downscaled->size(),0,0,cv::INTER_NEAREST);

	if ( d_userInterface ) {
		if ( !d_videoOutput ) {
			d_downscaled = downscaled;
		} else {
			d_downscaled = d_framePool.Get();
			*d_downscaled = downscaled->clone();
		}
	}

	if ( d_videoOutput ) {
		//TODO: send frame to ouput

	}

}

void ProcessFrameTask::DropFrame(const Frame::Ptr & frame) {
	if ( !d_connection ) {
		return;
	}

	auto m = PrepareMessage(frame);
	m->set_error(hermes::FrameReadout::PROCESS_OVERFLOW);

	Connection::PostMessage(d_connection,*m);
}



void ProcessFrameTask::ProcessFrame(const Frame::Ptr & frame) {

	auto m = PrepareMessage(frame);

	if ( ShouldProcess(frame->ID()) == true ) {
		Detect(frame,*m);
		if ( d_connection ) {
			Connection::PostMessage(d_connection,*m);
		}
		CatalogAnt(frame,*m);
		ExportFullFrame(frame);
	}

	DisplayFrame(frame,m);
}


std::shared_ptr<hermes::FrameReadout> ProcessFrameTask::PrepareMessage(const Frame::Ptr & frame) {
	auto m = d_messagePool.Get();
	m->Clear();
	m->set_timestamp(frame->Timestamp());
	m->set_frameid(frame->ID());
	frame->Time().ToTimestamp(m->mutable_time());
	m->set_producer_uuid(d_options.Network.UUID);
	m->set_width(frame->Width());
	m->set_height(frame->Height());
	return m;
}


bool ProcessFrameTask::ShouldProcess(uint64_t ID) {
	if ( d_options.Process.FrameStride <= 1 ) {
		return true;
	}
	return d_options.Process.FrameID.count(ID % d_options.Process.FrameStride) != 0;
}



void ProcessFrameTask::QueueFrame( const Frame::Ptr & frame ) {
	d_frameQueue.push(frame);
}

void ProcessFrameTask::CloseFrameQueue() {
	d_frameQueue.push({});
}

void ProcessFrameTask::Detect(const Frame::Ptr & frame,
                              hermes::FrameReadout & m) {
	if ( d_detector ) {
		d_detector->Detect(frame->ToCV(),d_actualThreads,m);
	}
}

void ProcessFrameTask::ExportFullFrame(const Frame::Ptr & frame) {
	if ( d_nextFrameExport.Before(frame->Time()) ) {
		return;
	}

	if ( false /* TODO: export is not sucessful */ ) {
		return;
	}
	d_actualThreads = d_maximumThreads - 1;
	cv::setNumThreads(d_actualThreads);
	d_nextFrameExport = frame->Time().Add(d_options.Process.ImageRenewPeriod);

}



void ProcessFrameTask::CatalogAnt(const Frame::Ptr & frame,
                                  const hermes::FrameReadout & m) {
	if ( d_options.Process.NewAntOutputDir.empty() ) {
		return;
	}

	ResetExportedID(frame->Time());


	auto toExport = FindUnexportedID(m);
	tbb::parallel_for(std::size_t(0),
	                  toExport.size(),
	                  [&] (std::size_t index) {
		                  const auto & [tagID,x,y] = toExport[index];
		                  ExportROI(frame->ToCV(),frame->ID(),tagID,x,y);
	                  });
}

void ProcessFrameTask::ResetExportedID(const Time & time) {
	if ( d_nextAntCatalog.Before(time) ) {
		d_nextAntCatalog = time.Add(d_options.Process.ImageRenewPeriod);
		d_exportedID.clear();
	}
}

std::vector<std::tuple<uint32_t,double,double>>
ProcessFrameTask::FindUnexportedID(const hermes::FrameReadout & m) {
	std::vector<std::tuple<uint32_t,double,double>> res;
	for ( const auto & t : m.tags() ) {
		if ( d_exportedID.count(t.id()) !=  0 ) {
			continue;
		}
		res.push_back({t.id(),t.x(),t.y()});
		if ( res.size() >= d_actualThreads ) {
			break;
		}
	}
	return res;
}


cv::Rect ProcessFrameTask::GetROIAt(int x, int y,
                                    const cv::Size & bound) {
	x = std::clamp(x - int(d_options.Process.NewAntROISize / 2),
	               0,
	               bound.width - int(d_options.Process.NewAntROISize));

	y = std::clamp(y - int(d_options.Process.NewAntROISize / 2),
	               0,
	               bound.height - int(d_options.Process.NewAntROISize));

	return cv::Rect(cv::Point2d(x,y),cv::Size(d_options.Process.NewAntROISize,
	                                          d_options.Process.NewAntROISize));
}

void ProcessFrameTask::ExportROI(const cv::Mat & image,
                                 uint64_t frameID,
                                 uint32_t tagID,
                                 double x,
                                 double y) {

	std::ostringstream oss;
	oss << d_options.Process.NewAntOutputDir << "/ant_" << tagID << "_" << frameID << ".png";
	cv::imwrite(oss.str(),cv::Mat(image,GetROIAt(x,y,image.size())));
}


void ProcessFrameTask::DisplayFrame(const Frame::Ptr frame,
                                    const std::shared_ptr<hermes::FrameReadout> & m) {
	// 1. fetches wanted ROI
	// 2. prepare roi if needed
	// 3. send all to process
}


size_t ProcessFrameTask::DownscaledImagePerCycle() const {
	size_t res(0);
	if ( d_userInterface ) {
		res += 2;
	}
	if ( d_videoOutput ) {
		res += 1;
	}
	return res;
}



} // namespace artemis
} // namespace fort
