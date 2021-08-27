#include "antdetect.hpp"

#include <unistd.h>

/*std::vector<std::array<float,2>> TDetect(bool useCUDA, std::string modelFilepath, std::string labelFilepath, cv::Mat imageBGR, size_t nThreads)
{
    int koef = 2;
    std::string instanceName{"image-classification-inference"};

    Ort::SessionOptions sessionOptions;
    
    std::vector<std::string> labels{readLabels(labelFilepath)};

    Ort::Env env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING, instanceName.c_str());
    
    sessionOptions.SetIntraOpNumThreads(1);

    if (useCUDA)
    {
        OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CUDA(sessionOptions, 0);
    }

    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    Ort::Session session(env, modelFilepath.c_str(), sessionOptions);

    //cv::Mat imageBGR = cv::imread(imageFilepath, cv::ImreadModes::IMREAD_COLOR);
//---------------------------------------------------------------------

    //v::namedWindow("Display window", cv::WINDOW_AUTOSIZE);
    cv::Mat resizedImageBGR;
    cv::Mat bufimageBGR;
    cv::Mat img;
    //cv::resize(imageBGR, resizedImageBGR,cv::Size(500, 500),cv::InterpolationFlags::INTER_CUBIC);

    cv::Point2f pd;
    cv::Point2f p1;
    

    int step = 30;

    int startX = 0;
    int startY = 0;
    //p1.x = 166;
    //p1.y = 565;

    p1.x = 0;
    p1.y = 0;

    bufimageBGR = resizedImageBGR;

    float d;

    std::vector<cv::Point2f> detects;

    int test = 0;
    while(p1.y + step < 1000 && test < 100)
    {
        test++;
        img = imageBGR(cv::Rect(p1.x*4, p1.y*4, 400, 400));
        d = detect(img,labels,session);
        if(d >= 0.99)
        {
            pd.x = (p1.x+50)/koef;
            pd.y = (p1.y+50)/koef;

            detects.push_back(pd);
        }
        
        if(d > 0.69)
            step = 3;

        if(d < 0.1)
            step = 20;

        cv::resize(imageBGR, bufimageBGR,cv::Size(1000/koef, 1000/koef),cv::InterpolationFlags::INTER_CUBIC);

        drawrec(bufimageBGR,p1,d,koef);

        for(int j=0; j<detects.size(); j++)
        {
            cv::circle(bufimageBGR, detects.at(j), 5, cv::Scalar(0, 0, 255), 10/koef);
        }

        imshow("Detector", bufimageBGR);
        cv::waitKey(100);

        p1.x += step;
        if(p1.x + 100 > 1000)
        {
            p1.x = 0;
            p1.y += step;
        }
    }    

    std::vector<std::array<float,2>> v;
	std::array<float,2> p;
	p[0] = 15;
	p[1] = 18;
	v.push_back(p);
	return v;
}
*/

std::vector<cv::Point2f> detectorT (torch::jit::script::Module module, cv::Mat frame, torch::DeviceType device_type)
{ 
  std::vector<std::string> labels;
  labels.push_back("more than one ant");
  labels.push_back("no ants");
  labels.push_back("one ant");
  labels.push_back("trophallaxis");

  int pinput = 175;
  int step = 100;

  cv::Point2f pd;
  cv::Point2f p1;
  p1.x = 0;
  p1.y = 0;

  cv::Mat imageBGR;
  cv::Mat bufimageBGR;

  std::vector<cv::Point2f> detects;
  std::vector<cv::Point2f> detectsforcircle;
  
  while(p1.y + pinput < frame.rows)
  {
    imageBGR = frame(cv::Rect(p1.x, p1.y, pinput, pinput));
    //cv::resize(imageBGR, imageBGR, cv::Size(pinput, pinput), cv::INTER_CUBIC);
    cv::cvtColor(imageBGR, imageBGR, cv::COLOR_BGR2RGB);
    imageBGR.convertTo(imageBGR, CV_32FC3, 1.0f / 255.0f);
    auto input_tensor = torch::from_blob(imageBGR.data, {1, imageBGR.rows, imageBGR.cols, 3});
    input_tensor = input_tensor.permute({0, 3, 1, 2}).contiguous();
    
    input_tensor = input_tensor.to(device_type);
    std::vector<torch::jit::IValue> input;
    input.emplace_back(input_tensor);

    at::Tensor output = module.forward(input).toTensor();
    //std::cout << labels[output.argmax(1).item().toInt()] << '\n';

    if(labels[output.argmax(1).item().toInt()] == "trophallaxis")
    {
      pd.x = p1.x+pinput/2;
      pd.y = p1.y+pinput/2;
      detects.push_back(pd);
    }

    p1.x += step;
    if(p1.x + pinput >= frame.cols)
    {
        p1.x = 0;
        p1.y += step;
    }
  }

  return detects;
}