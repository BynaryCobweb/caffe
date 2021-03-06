#include <boost/thread.hpp>
#include <vector>

#include "caffe/blob.hpp"
#include "caffe/data_transformer.hpp"
#include "caffe/internal_thread.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/blocking_queue.hpp"

namespace caffe {

template <typename Dtype>
BaseDataLayer<Dtype>::BaseDataLayer(const LayerParameter& param)
    : Layer<Dtype>(param),
  transform_param_(param.transform_param()) {
}

template <typename Dtype>
void BaseDataLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  //std::cerr << "\ntop size=" << top.size() << std::endl;
  if (top.size() == 1) {
    output_labels_ = false;
  } else {
    output_labels_ = true;
  }
  data_transformer_.reset(
      new DataTransformer<Dtype>(transform_param_, this->phase_));
  data_transformer_->InitRand();
  // The subclasses should setup the size of bottom and top
  DataLayerSetUp(bottom, top);
}

template <typename Dtype>
BasePrefetchingDataLayer<Dtype>::BasePrefetchingDataLayer(
    const LayerParameter& param)
    : BaseDataLayer<Dtype>(param),
  untransformed_top_(false), prefetch_free_(), prefetch_full_(),
  prefetch_free_untransformed_(),  prefetch_full_untransformed_()  {

  if (param.transform_param().has_untransformed_top() &&
      param.transform_param().untransformed_top())
    untransformed_top_ = true;

  for (int i = 0; i < PREFETCH_COUNT; ++i) {
    prefetch_free_.push(&prefetch_[i]);
    if (untransformed_top_)
      prefetch_free_untransformed_.push(&prefetch_untransformed_[i]);
  }
}

template <typename Dtype>
void BasePrefetchingDataLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  BaseDataLayer<Dtype>::LayerSetUp(bottom, top);
  // Before starting the prefetch thread, we make cpu_data and gpu_data
  // calls so that the prefetch thread does not accidentally make simultaneous
  // cudaMalloc calls when the main thread is running. In some GPUs this
  // seems to cause failures if we do not so.
  for (int i = 0; i < PREFETCH_COUNT; ++i) {
    prefetch_[i].data_.mutable_cpu_data();
    if (untransformed_top_)
      prefetch_untransformed_[i].data_.mutable_cpu_data();
    if (this->output_labels_) {
      prefetch_[i].label_.mutable_cpu_data();
      if (untransformed_top_)
        prefetch_untransformed_[i].label_.mutable_cpu_data();
    }
  }
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    for (int i = 0; i < PREFETCH_COUNT; ++i) {
      prefetch_[i].data_.mutable_gpu_data();
      if (this->output_labels_) {
        prefetch_[i].label_.mutable_gpu_data();
     }
    }
    if (untransformed_top_)
      for (int i = 0; i < PREFETCH_COUNT; ++i) {
        prefetch_untransformed_[i].data_.mutable_gpu_data();
        if (this->output_labels_) {
          prefetch_untransformed_[i].label_.mutable_gpu_data();
        }
      }
  }
#endif
  DLOG(INFO) << "Initializing prefetch";
  this->data_transformer_->InitRand();
  StartInternalThread();
  DLOG(INFO) << "Prefetch initialized.";
}

template <typename Dtype>
void BasePrefetchingDataLayer<Dtype>::InternalThreadEntry() {
#ifndef CPU_ONLY
  cudaStream_t stream;
  cudaStream_t stream2;
  if (Caffe::mode() == Caffe::GPU) {
    CAFFE1_CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    if (untransformed_top_)
      CAFFE1_CUDA_CHECK(cudaStreamCreateWithFlags(&stream2, cudaStreamNonBlocking));
  }
#endif

  try {
    while (!must_stop()) {
      Batch<Dtype>* batch = prefetch_free_.pop();
      Batch<Dtype>* batch_untransformed = NULL;
      if (untransformed_top_)
        {
          batch_untransformed = prefetch_free_untransformed_.pop();
          load_batch_and_untransformed_batch(batch,batch_untransformed);
        }
      else
        load_batch(batch);

#ifndef CPU_ONLY
      if (Caffe::mode() == Caffe::GPU) {
        batch->data_.data().get()->async_gpu_push(stream);
        CAFFE1_CUDA_CHECK(cudaStreamSynchronize(stream));
        if (untransformed_top_)
          {
            batch_untransformed->data_.data().get()->async_gpu_push(stream2);
            CAFFE1_CUDA_CHECK(cudaStreamSynchronize(stream2));
          }
      }
#endif
      prefetch_full_.push(batch);
      if (untransformed_top_)
        prefetch_full_untransformed_.push(batch_untransformed);
    }
  } catch (boost::thread_interrupted&) {
    // Interrupted exception is expected on shutdown
  }
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    CAFFE1_CUDA_CHECK(cudaStreamDestroy(stream));
    if (untransformed_top_)
      CAFFE1_CUDA_CHECK(cudaStreamDestroy(stream2));
  }
#endif
}

template <typename Dtype>
void BasePrefetchingDataLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  Batch<Dtype>* batch = prefetch_full_.pop("Data layer prefetch queue empty");
  // Reshape to loaded data.
  top[0]->ReshapeLike(batch->data_);

  // Copy the data
  caffe_copy(batch->data_.count(), batch->data_.cpu_data(),
             top[0]->mutable_cpu_data());

  Batch<Dtype>* batch_untransformed;
  if (untransformed_top_)
    {
      batch_untransformed = prefetch_full_untransformed_.pop("Data layer prefetch queue empty");
      top[2]->ReshapeLike(batch_untransformed->data_);
      caffe_copy(batch_untransformed->data_.count(), batch_untransformed->data_.cpu_data(),
                 top[2]->mutable_cpu_data());
    }


  DLOG(INFO) << "Prefetch copied";
  if (this->output_labels_) {
    // Reshape to loaded labels.
    top[1]->ReshapeLike(batch->label_);
    // Copy the labels.
    caffe_copy(batch->label_.count(), batch->label_.cpu_data(),
        top[1]->mutable_cpu_data());
  }

  prefetch_free_.push(batch);
  if (untransformed_top_)
    prefetch_free_untransformed_.push(batch_untransformed);

}




template <typename Dtype>
BasePrefetchingSparseDataLayer<Dtype>::BasePrefetchingSparseDataLayer(
    const LayerParameter& param)
    : BaseDataLayer<Dtype>(param),
      prefetch_free_(), prefetch_full_() {
  for (int i = 0; i < PREFETCH_COUNT; ++i) {
    prefetch_free_.push(&prefetch_[i]);
  }
}

template <typename Dtype>
void BasePrefetchingSparseDataLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  BaseDataLayer<Dtype>::LayerSetUp(bottom, top);
  //DataLayerSetUp(bottom,top);

  // Before starting the prefetch thread, we make cpu_data and gpu_data
  // calls so that the prefetch thread does not accidentally make simultaneous
  // cudaMalloc calls when the main thread is running. In some GPUs this
  // seems to cause failures if we do not so.
  for (int i = 0; i < PREFETCH_COUNT; ++i) {
    //std::cerr << "batch data check\n";
    prefetch_[i].data_.mutable_cpu_data();
    /*prefetch_[i].data_.mutable_cpu_indices();
      prefetch_[i].data_.mutable_cpu_ptr();*/
    if (this->output_labels_) {
      //std::cerr << "batch label check\n";
      prefetch_[i].label_.mutable_cpu_data();
    }
  }
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    for (int i = 0; i < PREFETCH_COUNT; ++i) {
      prefetch_[i].data_.mutable_gpu_data();
      if (this->output_labels_) {
        prefetch_[i].label_.mutable_gpu_data();
      }
    }
  }
#endif
  DLOG(INFO) << "Initializing sparse prefetch";
  this->data_transformer_->InitRand();
  StartInternalThread();
  DLOG(INFO) << "Prefetch sparse initialized.";
}

template <typename Dtype>
void BasePrefetchingSparseDataLayer<Dtype>::InternalThreadEntry() {
#ifndef CPU_ONLY
  cudaStream_t stream;
  if (Caffe::mode() == Caffe::GPU) {
    CAFFE1_CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  }
#endif

  try {
    while (!must_stop()) {
      SparseBatch<Dtype>* batch = prefetch_free_.pop();
      load_batch(batch);
#ifndef CPU_ONLY
      if (Caffe::mode() == Caffe::GPU) {
        batch->data_.data().get()->async_gpu_push(stream);
        CAFFE1_CUDA_CHECK(cudaStreamSynchronize(stream));
      }
#endif
      prefetch_full_.push(batch);
    }
  } catch (boost::thread_interrupted&) {
    // Interrupted exception is expected on shutdown
  }
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    CAFFE1_CUDA_CHECK(cudaStreamDestroy(stream));
  }
#endif
}

template <typename Dtype>
void BasePrefetchingSparseDataLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  SparseBatch<Dtype>* batch = prefetch_full_.pop("Data layer prefetch queue empty");
  // Reshape to loaded data.
  //top[0]->ReshapeLike(batch->data_);
  // Copy the data
  /*caffe_copy(batch->data_.count(), batch->data_.cpu_data(),
    top[0]->mutable_cpu_data());*/
  if (SparseBlob<Dtype>* sparseBlob = dynamic_cast<SparseBlob<Dtype>*>(top[0]))
    {
      // Reshape to loaded data.
      sparseBlob->ReshapeLike(batch->data_);
      // Copy the data
      caffe_copy(batch->data_.nnz(), batch->data_.cpu_data(),
         sparseBlob->mutable_cpu_data());
      caffe_copy<int>(batch->data_.nnz(), batch->data_.cpu_indices(),
         sparseBlob->mutable_cpu_indices());
      caffe_copy<int>(batch->data_.shape()[0]+1, batch->data_.cpu_ptr(),
         sparseBlob->mutable_cpu_ptr());
    } else {
    LOG(ERROR) << "The top blob in the sparse data layer is not sparse";
    LOG(FATAL) << "fatal error";
    }
 
  DLOG(INFO) << "Prefetch sparse copied (forward)";
  //std::cerr << "\nforward has labels=" << this->output_labels_ << std::endl;
  if (this->output_labels_) {
    // Reshape to loaded labels.
    top[1]->ReshapeLike(batch->label_);
    // Copy the labels.
    caffe_copy(batch->label_.count(), batch->label_.cpu_data(),
        top[1]->mutable_cpu_data());
  }

  prefetch_free_.push(batch);
}





#ifdef CPU_ONLY
STUB_GPU_FORWARD(BasePrefetchingDataLayer, Forward);
STUB_GPU_FORWARD(BasePrefetchingSparseDataLayer, Forward);
#endif

INSTANTIATE_CLASS(BaseDataLayer);
INSTANTIATE_CLASS(BasePrefetchingDataLayer);
INSTANTIATE_CLASS(BasePrefetchingSparseDataLayer);

}  // namespace caffe
