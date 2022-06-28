#include<opencv2/opencv.hpp>
#include<torch/torch.h>
#include<torch/script.h>
#include <fstream>
#include <iostream>
#include<string>
#include<vector>
#include"opencv_utils.h"
#include"utils.h"
using namespace cv;
using namespace std;


/**
 * ��ȡͼ��Ԥ����
 */
cv::Mat readImage(String path) {
	cv::Mat image = cv::imread(path);				// BGR
	return image;
}


/**
 * ͼƬԤ����
 */
torch::Tensor preProcess(cv::Mat image, MetaData& meta) {
	int height = image.rows;
	int width = image.cols;
	//����ԭͼ��Ϣ
	meta.height = height;
	meta.width = width;

	cv::Mat image1;
	cv::cvtColor(image, image1, cv::COLOR_BGR2RGB);	// BGR2RGB

	//����
	cv::Mat res = Resize(image1, meta.pred_image_size, meta.pred_image_size, "bilinear");

	//��һ��
	res = Divide(res);

	//ת��Ϊtensor
	torch::Tensor x = torch::from_blob(res.data, { 1, res.rows, res.cols, 3 });
	x = x.permute({ 0, 3, 1, 2 });
	//x = x.div(torch::full({ 1, 3, 512, 512 }, 255.0));

	//��׼��
	vector<double> mean = vector<double>{ 0.485, 0.456, 0.406 };
	vector<double> std = vector<double>{ 0.229, 0.224, 0.225 };
	x = torch::data::transforms::Normalize<>(mean, std)(x);
	cout << "sum:" << x.sum() << endl;  //-853865
	cout << x.size(0) << ", " << x.size(1) << ", " << x.size(2) << ", " << x.size(3) << endl; // 1, 3, 512, 512
	//cout << x << endl;
	return x;
}


/**
 * ��ȡģ��
 */
torch::jit::Module loadTorchScript(String path) {
	torch::jit::Module model = torch::jit::load(path);
	return model;
}


/**
 * �ֱ��׼������ͼ�͵÷�
 */
torch::Tensor normalize(torch::Tensor& targets, double threshold, double max_val, double min_val) {
	auto normalized = ((targets - threshold) / (max_val - min_val)) + 0.5;
	normalized = torch::minimum(normalized, torch::tensor(1));
	normalized = torch::maximum(normalized, torch::tensor(0));
	return normalized;
}


/**
 * ������
 */
void postProcess(torch::Tensor& anomaly_map, torch::Tensor& pred_score, MetaData& meta, cv::Mat& origin_image) {
	anomaly_map.squeeze_();
	//cout << anomaly_map.size(0) << " " << anomaly_map.size(1) << endl;	// 512 512
	
	//��׼������ͼ�͵÷�
	anomaly_map = normalize(anomaly_map, meta.pixel_threshold, meta.max, meta.min);
	pred_score  = normalize(pred_score, meta.image_threshold, meta.max, meta.min);
	cout << "pred_score:" << pred_score << endl;
	ofstream ofs;
	ofs.open("result.txt", ios::out);
	ofs << pred_score;
	ofs.close();

	//����ʵ���� 0~1 �� 0~255
	cv::Mat anomaly_map1(cv::Size(meta.pred_image_size, meta.pred_image_size), CV_8UC1, anomaly_map.data_ptr());
	anomaly_map1.convertTo(anomaly_map1, CV_8U, 1, 0);

	//��˹�˲���Ӧ�÷���post_processǰ�棬��������˺��棬��ΪҪʹ��opencv
	cv::Mat res;
	int sigma = 4;
	int kernel_size = 2 * int(4.0 * sigma + 0.5) + 1;
	cv::GaussianBlur(anomaly_map1, res, { kernel_size, kernel_size }, 4.0, 4.0);

	//��ԭ��ԭͼ�ߴ�
	cv::Mat res1 = Resize(res, meta.height, meta.width, "bilinear");
	//cout << res1.rows << ", " << res1.cols << ", " << res1.channels() << endl;	//2711 5351 1

	//anomaly_map = (anomaly_map - anomaly_map.min()) / np.ptp(anomaly_map) np.ptp()����ʵ�ֵĹ��ܵ�ͬ��np.max(array) - np.min(array)
	//double minValue, maxValue;    // ���ֵ����Сֵ
	//cv::Point  minIdx, maxIdx;    // ��Сֵ���꣬���ֵ����   
	//cv::minMaxLoc(res1, &minValue, &maxValue, &minIdx, &maxIdx);
	//res1 = (res1 - minValue) / (maxValue - minValue);

	//��ͨ��ת��Ϊ3ͨ��
	cv::Mat res2;
	cv::applyColorMap(res1, res2, cv::COLORMAP_JET);
	//cout << res2.rows << ", " << res2.cols << ", " << res2.channels() << endl;	//2711 5351 3

	//�ϲ�ԭͼ������ͼ
	cv::Mat res3;
	cv::addWeighted(res2, 0.4, origin_image, 0.6, 0, res3);
	cv::imwrite("anomaly_map.png", res2);
	cv::imwrite("result.png", res3);
}


/**
 * ����
 */
void inference(torch::jit::Module& model, torch::Tensor& x, MetaData& meta, cv::Mat& origin_image) {
	//��������ֵ������ֱ��ʹ�� {} ��������
	//vector<torch::jit::IValue> input;
	//input.push_back(x);
	//x = torch::randn({ 1, 3, 512, 512 });
	torch::jit::IValue y = model.forward({ x });
	//�������ֵ����ȡ��ʽ toTuple() toList()
	torch::Tensor anomaly_map = y.toTuple()->elements()[0].toTensor();
	torch::Tensor pred_score  = y.toTuple()->elements()[1].toTensor();
	//cout << pred_score << endl;

	//����
	postProcess(anomaly_map, pred_score, meta, origin_image);
}


int main() {
	cout << "cuda�Ƿ���ã�"  << torch::cuda::is_available() << endl;
	cout << "cudnn�Ƿ���ã�" << torch::cuda::cudnn_is_available() << endl;

	// ��ȡmeta
	auto meta = getJson("D:\\ai\\code\\abnormal\\anomalib\\results\\export\\512-0.1\\param.json");

	//��ȡͼƬ
	string img_path = "D:\\ai\\code\\abnormal\\anomalib\\datasets\\some\\1.abnormal\\OriginImage_20220526_113038_Cam1_2_crop.jpg";
	auto image      = readImage(img_path);
	auto x          = preProcess(image, meta);

	cout << meta.image_threshold << " " << meta.pixel_threshold << " " << meta.min << " " << meta.max << " " 
		<< meta.pred_image_size << " " << meta.height << " " << meta.width << endl;
	// 0.92665 0.92665 0.000141821 1.70372 512 2711 5351

	//��ȡģ��
	string path = "D:\\ai\\code\\abnormal\\anomalib\\results\\export\\512-0.1\\output.torchscript";
	auto model = loadTorchScript(path);

	//����
	inference(model, x, meta, image);

	return 0;
}