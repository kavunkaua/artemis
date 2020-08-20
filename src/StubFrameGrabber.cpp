#include "StubFrameGrabber.hpp"

#include <thread>

#include <opencv2/highgui/highgui.hpp>


namespace fort {
namespace artemis {

StubFrame::StubFrame(const cv::Mat & mat, uint64_t timestamp, uint64_t ID)
	: d_mat(mat.clone())
	, d_timestamp(timestamp)
	, d_ID(ID) {
}

StubFrame::~StubFrame() {}

void * StubFrame::Data() {
	return d_mat.data;
}

size_t StubFrame::Width() const {
	return d_mat.cols;
}

size_t StubFrame::Height() const {
	return d_mat.rows;
}

uint64_t StubFrame::Timestamp() const {
	return d_timestamp;
}

uint64_t StubFrame::ID() const {
	return d_ID;
}

const cv::Mat & StubFrame::ToCV() {
	return d_mat;
}


StubFrameGrabber::StubFrameGrabber(const std::string & path)
	: d_ID(0)
	, d_timestamp(0) {
	d_image = cv::imread(path,0);
	if ( d_image.data == NULL ) {
		throw std::runtime_error("Could not load '" + path + "'");
	}
}

StubFrameGrabber::~StubFrameGrabber() {
}

void StubFrameGrabber::Start() {
	d_ID = 0;
	d_timestamp = 0;
	d_last = clock::now();
}

void StubFrameGrabber::Stop() {
}

std::pair<int32_t,int32_t> StubFrameGrabber::GetResolution() {
	return std::make_pair(d_image.cols,d_image.rows);
}

Frame::Ptr StubFrameGrabber::NextFrame() {
	d_last += std::chrono::milliseconds(250);
	std::this_thread::sleep_until(d_last);

	Frame::Ptr res = std::make_shared<StubFrame>(d_image,d_timestamp,d_ID);
	d_timestamp += 250000;
	d_ID += 1;
	return res;
}

} // namespace artemis
} // namespace fort
