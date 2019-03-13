#include <algorithm>
#include <assert.h>
#include <cmath>
#include <cuda_runtime_api.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <time.h>

#include "NvInfer.h"
#include "NvOnnxParser.h"
#include "common.h"
#include "image.h"
#include "NvInferPlugin.h"



#ifdef OPENCV
#include "opencv2/highgui/highgui_c.h"
#include "opencv2/core/version.hpp"
#ifndef CV_VERSION_EPOCH
#include "opencv2/videoio/videoio_c.h"
#endif

#include "http_stream.h"
#include "gettimeofday.h"

#endif
using namespace nvinfer1;

static const int INPUT_H = 512;
static const int INPUT_W = 512;
static const int INPUT_C = 3;
static const int OUTPUT_SIZE = 114688;
static Logger gLogger;
static int gUseDLACore{ -1 };

void onnxToTRTModel(const std::string& modelFile, // name of the onnx model
	unsigned int maxBatchSize,    // batch size - NB must be at least as large as the batch we want to run with
	IHostMemory*& trtModelStream) // output buffer for the TensorRT model
{
	int verbosity = (int)nvinfer1::ILogger::Severity::kWARNING;
	// create the builder
	IBuilder* builder = createInferBuilder(gLogger);
	nvinfer1::INetworkDefinition* network = builder->createNetwork();

	auto parser = nvonnxparser::createParser(*network, gLogger);


	//Optional - uncomment below lines to view network layer information
	//config->setPrintLayerInfo(true);
	//parser->reportParsingInfo();

	if (!parser->parseFromFile(modelFile.c_str(), verbosity))
	{
		string msg("failed to parse onnx file");
		gLogger.log(nvinfer1::ILogger::Severity::kERROR, msg.c_str());
		exit(EXIT_FAILURE);
	}

	// Build the engine
	builder->setMaxBatchSize(maxBatchSize);
	builder->setMaxWorkspaceSize(3_GB); //不能超过你的实际能用的显存的大小，例如我的1060的可用为4.98GB，超过4.98GB会报错

	samplesCommon::enableDLA(builder, gUseDLACore);
	ICudaEngine* engine = builder->buildCudaEngine(*network);
	assert(engine);

	// we can destroy the parser
	parser->destroy();

	// serialize the engine, then close everything down  序列化
	trtModelStream = engine->serialize();
	engine->destroy();
	network->destroy();
	builder->destroy();
}

void doInference(IExecutionContext& context, float* input, float* output, int batchSize)
{
	const ICudaEngine& engine = context.getEngine();
	// input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
	// of these, but in this case we know that there is exactly one input and one output.
	assert(engine.getNbBindings() == 2);
	void* buffers[2];

	// In order to bind the buffers, we need to know the names of the input and output tensors.
	// note that indices are guaranteed to be less than IEngine::getNbBindings()
	int inputIndex, outputIndex;
	for (int b = 0; b < engine.getNbBindings(); ++b)
	{
		if (engine.bindingIsInput(b))
			inputIndex = b;
		else
			outputIndex = b;
	}
	// create GPU buffers and a stream   创建GPU缓冲区和流
	CHECK(cudaMalloc(&buffers[inputIndex], batchSize *INPUT_C* INPUT_H * INPUT_W * sizeof(float)));
	CHECK(cudaMalloc(&buffers[outputIndex], batchSize * OUTPUT_SIZE * sizeof(float)));

	cudaStream_t stream;
	CHECK(cudaStreamCreate(&stream));

	// DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
	CHECK(cudaMemcpyAsync(buffers[inputIndex], input, batchSize *INPUT_C* INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
	context.enqueue(batchSize, buffers, stream, nullptr);
	CHECK(cudaMemcpyAsync(output, buffers[outputIndex], batchSize * OUTPUT_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
	cudaStreamSynchronize(stream);

	// release the stream and the buffers
	cudaStreamDestroy(stream);
	CHECK(cudaFree(buffers[inputIndex]));
	CHECK(cudaFree(buffers[outputIndex]));
}

int main(int argc, char** argv)
{
	gUseDLACore = samplesCommon::parseDLA(argc, argv);
	// create a TensorRT model from the onnx model and serialize it to a stream
	IHostMemory* trtModelStream{ nullptr };
	onnxToTRTModel("D:/pytorch/light-weight-refinenet/test_up.onnx", 1, trtModelStream);  //读onnx模型,序列化引擎
	assert(trtModelStream != nullptr);
	// deserialize the engine    DLA加速
	//反序列化引擎
	IRuntime* runtime = createInferRuntime(gLogger);
	assert(runtime != nullptr);
	if (gUseDLACore >= 0)
	{
		runtime->setDLACore(gUseDLACore);
	}
	//反序列化
	ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream->data(), trtModelStream->size(), nullptr);
	assert(engine != nullptr);
	trtModelStream->destroy();
	IExecutionContext* context = engine->createExecutionContext();
	assert(context != nullptr);


	int cam_index = 0;
	char *filename = (argc > 1) ? argv[1] : 0;
	std::cout << "Hello World!\n";
	CvCapture * cap;

	if (filename) {
		//cap = cvCaptureFromFile(filename);
		cap = get_capture_video_stream(filename);
	}
	else
	{
		cap = get_capture_webcam(cam_index);;
	}
	cvNamedWindow("Segmention", CV_WINDOW_NORMAL); //创建窗口显示图像，可以鼠标随意拖动窗口改变大小
	cvResizeWindow("Segmention", 512, 512);//设定窗口大小
	float prob[OUTPUT_SIZE];
	float fps = 0;
	while (1) {
		struct timeval tval_before, tval_after, tval_result;
		gettimeofday(&tval_before, NULL);
		image in = get_image_from_stream_cpp(cap);//c,h,w结构且已经处以225，浮点数
		image in_s = resize_image(in, 512, 512);//改变图片大小为标准长宽，用于网络一的图片
		in_s = normal_image(in_s);//正则化
		//prob = 【1，7，128，128】-->imgae[7,128,128]-->【7,512，512】-->[3,512,512]
		//【512，512，7】-->[512,512,1]
		// run inference   进行推理
		doInference(*context, in_s.data, prob, 1);
		image real_out = Tranpose(prob); //[128，128，7]


		show_image(real_out, "Segmention");   //显示图片

		free_image(in);
		free_image(in_s);
		free_image(real_out);
		if (cvWaitKey(10) == 27) break;
		gettimeofday(&tval_after, NULL);
		timersub(&tval_after, &tval_before, &tval_result);
		float curr = 1000000.f / ((long int)tval_result.tv_usec);
		printf("\nFPS:%.0f\n", fps);
		fps = .9*fps + .1*curr;
	}
	// destroy the engine

	return 0;
}
