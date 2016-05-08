#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "caffe/data_layers.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
FlowImageDataLayer<Dtype>::~FlowImageDataLayer<Dtype>() {
  this->JoinPrefetchThread();
}

template <typename Dtype>
void FlowImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.flow_image_data_param().new_height();
  const int new_width  = this->layer_param_.flow_image_data_param().new_width();
  const bool is_color  = this->layer_param_.flow_image_data_param().is_color();
  string root_folder = this->layer_param_.flow_image_data_param().root_folder();
    
  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames and labels
  const string& source = this->layer_param_.flow_image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string filename;
  string label_filename;
  string line;
  while (std::getline(infile, line)) {
    	std::stringstream ss(line);
	std::istream_iterator<std::string> begin(ss);
	std::istream_iterator<std::string> end;
	lines_.push_back(vector<std::string>(begin, end));
  }

  if (this->layer_param_.flow_image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " examples.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.flow_image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.flow_image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }

  // Read an image, and use it to initialize the top blobs.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][0],
                                    new_height, new_width, is_color);
  // Append flows to image
  if (lines_[lines_id_].size()>2) {
	vector<cv::Mat> Mat_Arr;  
	Mat_Arr.push_back(cv_img);
	for (int flow_id = 2; flow_id < lines_[lines_id_].size(); flow_id++){
		Mat_Arr.push_back(ReadImageToCVMat(root_folder + lines_[lines_id_][flow_id],
					    new_height, new_width, false, false));
	}
	cv::Mat cv_img_flow;
	cv::merge(Mat_Arr, cv_img_flow);
	cv_img = cv_img_flow;
  }
  const int channels = cv_img.channels();
  const int height = cv_img.rows;
  const int width = cv_img.cols;

  // sanity check label image
  cv::Mat cv_lab = ReadImageToCVMat(root_folder + lines_[lines_id_][1],
                                    new_height, new_width, false, true);
  CHECK(cv_lab.channels() == 1) << "Can only handle grayscale label images";
  CHECK(cv_lab.rows == height && cv_lab.cols == width) << "Input and label "
      << "image heights and widths must match";
  // image
    
  const int crop_size = this->layer_param_.transform_param().crop_size();
  const int batch_size = this->layer_param_.flow_image_data_param().batch_size();
  if (crop_size > 0) {
    top[0]->Reshape(batch_size, channels, crop_size, crop_size);
    this->prefetch_data_.Reshape(batch_size, channels, crop_size, crop_size);
    this->transformed_data_.Reshape(1, channels, crop_size, crop_size);
    // similarly reshape label data blobs
    top[1]->Reshape(batch_size, 1, crop_size, crop_size);
    this->prefetch_label_.Reshape(batch_size, 1, crop_size, crop_size);
    this->transformed_label_.Reshape(1, 1, crop_size, crop_size);
  } else {
    top[0]->Reshape(batch_size, channels, height, width);
    this->prefetch_data_.Reshape(batch_size, channels, height, width);
    this->transformed_data_.Reshape(1, channels, height, width);
    // similarly reshape label data blobs
    top[1]->Reshape(batch_size, 1, height, width);
    this->prefetch_label_.Reshape(batch_size, 1, height, width);
    this->transformed_label_.Reshape(1, 1, height, width);
  }
  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();
}

template <typename Dtype>
void FlowImageDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is used to create a thread that prefetches the data.
template <typename Dtype>
void FlowImageDataLayer<Dtype>::InternalThreadEntry() {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(this->prefetch_data_.count());
  CHECK(this->transformed_data_.count());
  FlowImageDataParameter flow_image_data_param = this->layer_param_.flow_image_data_param();
  const int batch_size = flow_image_data_param.batch_size();
  const int new_height = flow_image_data_param.new_height();
  const int new_width = flow_image_data_param.new_width();
  const int crop_size = this->layer_param_.transform_param().crop_size();
  const bool is_color = flow_image_data_param.is_color();
  string root_folder = flow_image_data_param.root_folder();

  // Reshape on single input batches for inputs of varying dimension.
  if (batch_size == 1 && crop_size == 0 && new_height == 0 && new_width == 0) {
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][0],
        0, 0, is_color);
    if (lines_[lines_id_].size()>2) {
	vector<cv::Mat> Mat_Arr;  
	Mat_Arr.push_back(cv_img);
	for (int flow_id = 2; flow_id < lines_[lines_id_].size(); flow_id++){
		Mat_Arr.push_back(ReadImageToCVMat(root_folder + lines_[lines_id_][flow_id],
					    new_height, new_width, false, false));
	}
	cv::Mat cv_img_flow;
	cv::merge(Mat_Arr, cv_img_flow);
	cv_img = cv_img_flow;
    }	
    this->prefetch_data_.Reshape(1, cv_img.channels(),
        cv_img.rows, cv_img.cols);
    this->transformed_data_.Reshape(1, cv_img.channels(),
        cv_img.rows, cv_img.cols);
    this->prefetch_label_.Reshape(1, 1, cv_img.rows, cv_img.cols);
    this->transformed_label_.Reshape(1, 1, cv_img.rows, cv_img.cols);
  }
  Dtype* prefetch_data = this->prefetch_data_.mutable_cpu_data();
  Dtype* prefetch_label = this->prefetch_label_.mutable_cpu_data();
  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][0],
        new_height, new_width, is_color);
    CHECK(cv_img.data) << "Could not load " << lines_[lines_id_][0];
    if (lines_[lines_id_].size()>2) {
	vector<cv::Mat> Mat_Arr;  
	Mat_Arr.push_back(cv_img);
	for (int flow_id = 2; flow_id < lines_[lines_id_].size(); flow_id++){
		Mat_Arr.push_back(ReadImageToCVMat(root_folder + lines_[lines_id_][flow_id],
					    new_height, new_width, false, false));
	}
	cv::Mat cv_img_flow;
	cv::merge(Mat_Arr, cv_img_flow);
	cv_img = cv_img_flow;
    }
    cv::Mat cv_lab = ReadImageToCVMat(root_folder + lines_[lines_id_][1],
        new_height, new_width, false, true);
    CHECK(cv_lab.data) << "Could not load " << lines_[lines_id_][1];
    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    int offset = this->prefetch_data_.offset(item_id);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img, &(this->transformed_data_));
    // transform label the same way
    int label_offset = this->prefetch_label_.offset(item_id);
    this->transformed_label_.set_cpu_data(prefetch_label + label_offset);
    
    this->data_transformer_->Transform(cv_lab, &this->transformed_label_, true);
    CHECK(!this->layer_param_.transform_param().mirror() &&
        this->layer_param_.transform_param().crop_size() == 0) 
        << "FIXME: Any stochastic transformation will break layer due to "
        << "the need to transform input and label images in the same way";
    trans_time += timer.MicroSeconds();

    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.flow_image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(FlowImageDataLayer);
REGISTER_LAYER_CLASS(FlowImageData);

}  // namespace caffe
