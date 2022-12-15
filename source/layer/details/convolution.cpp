//
// Created by fss on 22-11-13.
//

#include "convolution.hpp"
#include "runtime/runtime_ir.hpp"
#include "layer/abstract/layer_factory.hpp"
#include "data/fast_copy.hpp"
#include "convolution_3x3.hpp"
#include <glog/logging.h>

namespace kuiper_infer {

ConvolutionLayer::ConvolutionLayer(uint32_t output_channel, uint32_t in_channel, uint32_t kernel_h,
                                   uint32_t kernel_w, uint32_t padding_h, uint32_t padding_w, uint32_t stride_h,
                                   uint32_t stride_w, uint32_t groups, bool use_bias)
    : ParamLayer("Convolution"), padding_h_(padding_h), padding_w_(padding_w), stride_h_(stride_h),
      stride_w_(stride_w), groups_(groups), use_bias_(use_bias) {

  if (groups != 1) {
    in_channel /= groups;
  }

  for (uint32_t i = 0; i < output_channel; ++i) {
    std::shared_ptr<Tensor<float>> weight = std::make_shared<Tensor<float>>(in_channel, kernel_h, kernel_w);
    this->weights_.push_back(weight);
    if (use_bias_) {
      std::shared_ptr<Tensor<float>> bias = std::make_shared<Tensor<float>>(1, 1, 1);
      this->bias_.push_back(bias);
    }
  }
}

InferStatus ConvolutionLayer::Forward(const std::vector<std::shared_ptr<Tensor<float>>> &inputs,
                                      std::vector<std::shared_ptr<Tensor<float>>> &outputs) {
  if (inputs.empty() || inputs.size() != outputs.size()) {
    LOG(ERROR) << "The input feature map of convolution layer is empty";
    return InferStatus::kInferFailedInputEmpty;
  }

  if (weights_.empty()) {
    LOG(ERROR) << "Weight parameters is empty";
    return InferStatus::kInferFailedWeightParameterError;
  }

  if (this->use_bias_ && this->bias_.size() != this->weights_.size()) {
    LOG(ERROR) << "The size of the weight and bias is not adapting";
    return InferStatus::kInferFailedBiasParameterError;
  }

  const uint32_t batch_size = inputs.size();

  if (!stride_h_ || !stride_w_) {
    LOG(ERROR) << "The stride parameter is set incorrectly. It must always be greater than 0";
    return InferStatus::kInferFailedStrideParameterError;
  }

//#pragma omp parallel for num_threads(batch_size)
  for (uint32_t i = 0; i < batch_size; ++i) {
    std::shared_ptr<Tensor<float>> output_tensor = outputs.at(i);

    const std::shared_ptr<Tensor<float>> &input = inputs.at(i);

    std::shared_ptr<Tensor<float>> input_;
    if (padding_h_ > 0 || padding_w_ > 0) {
      input_ = input->Clone();
      input_->Padding({padding_h_, padding_h_, padding_w_, padding_w_}, 0);
    } else {
      input_ = input;
    }

    const uint32_t input_w = input_->cols();
    const uint32_t input_h = input_->rows();
    const uint32_t input_c = input_->channels();
    const uint32_t kernel_count = this->weights_.size();

    uint32_t kernel_h = this->weights_.at(0)->rows();
    uint32_t kernel_w = this->weights_.at(0)->cols();

    uint32_t output_h = uint32_t(std::floor((input_h - kernel_h) / stride_h_ + 1));
    uint32_t output_w = uint32_t(std::floor((input_w - kernel_w) / stride_w_ + 1));
    CHECK(output_h > 0 && output_w > 0) << "The size of the output feature map is less than zero";

    if (groups_ != 1) {
      CHECK(kernel_count % groups_ == 0);
      CHECK(input_c % groups_ == 0);
    }

    for (uint32_t k = 0; k < kernel_count; ++k) {
      const std::shared_ptr<Tensor<float>> &kernel = this->weights_.at(k);
      CHECK(kernel->rows() == kernel_h);
      CHECK(kernel->cols() == kernel_w);
      CHECK(kernel->channels() == input_c / groups_);
    }

    if (stride_h_ == 1 && stride_w_ == 1 && kernel_w == 3 && kernel_h == 3 && groups_ == 1) {
      input_->Padding({0, 4 - input_h % 4, 0, 4 - input_w % 4}, 0.f);
      const uint32_t input_h_padding = (4 - input_h % 4) + input_h;
      const uint32_t input_w_padding = (4 - input_w % 4) + input_w;

      const uint32_t output_h_padding = uint32_t(std::floor(input_h_padding - kernel_h + 1));
      const uint32_t output_w_padding = uint32_t(std::floor(input_w_padding - kernel_h + 1));
      const uint32_t out_channels = this->weights_.size();

      std::shared_ptr<Tensor<float>>
          output_channels = std::make_shared<Tensor<float>>(out_channels, output_h_padding, output_w_padding);
      std::shared_ptr<Tensor<float>> output_channels_ = outputs.at(i);

      for (uint32_t oc = 0; oc < out_channels; ++oc) {
        std::shared_ptr<Tensor<float>> kernel = this->weights_.at(oc);
        arma::fmat &output_channel = output_channels->at(oc);
        arma::fmat &output_channel_clipping = output_channels_->at(oc);

        for (uint32_t ic = 0; ic < input_c; ++ic) {
          const arma::fmat &input_channel = input_->at(ic);
          const arma::fmat &kernel_channel = kernel->at(ic);
          CHECK(kernel_channel.n_cols == 3 && kernel_channel.n_rows == 3);

          const arma::fmat &kernel_channel_g = WinogradTransformG(kernel_channel);

          const uint32_t h_tiles = input_channel.n_rows;
          const uint32_t w_tiles = input_channel.n_cols;
          CHECK(h_tiles % 4 == 0 && w_tiles % 4 == 0);

          for (uint32_t h_tile = 0; h_tile < h_tiles; h_tile += 2) {
            for (uint32_t w_tile = 0; w_tile < w_tiles; w_tile += 2) {
              if (h_tile + 4 <= h_tiles && w_tile + 4 <= w_tiles) {

                arma::fmat tile_mat(4, 4);
                for (uint32_t j = 0; j < 4; ++j) {
                  const float *col_ptr = input_channel.colptr(j + w_tile) + h_tile;
                  memcpy(tile_mat.memptr() + j * 4, col_ptr, 4 * sizeof(float));
                }
                const arma::fmat &output_tile = Winograd(kernel_channel_g, tile_mat);
                if (w_tile + 2 <= output_w_padding && h_tile + 2 <= output_h_padding) {
                  float *output_channel_ptr = output_channel.colptr(w_tile) + h_tile;
                  *(output_channel_ptr) += *(output_tile.memptr());
                  *(output_channel_ptr + 1) += *(output_tile.memptr() + 1);

                  output_channel_ptr = output_channel.colptr(w_tile + 1) + h_tile;
                  *(output_channel_ptr) += *(output_tile.memptr() + 2);
                  *(output_channel_ptr + 1) += *(output_tile.memptr() + 3);
                }
              }
            }
          }
        }
        output_channel_clipping = output_channel.submat(0, 0, output_h - 1, output_w - 1);
      }
    } else {
      uint32_t row_len = kernel_w * kernel_h;
      uint32_t col_len = ((input_w - kernel_w + 1) / stride_w_) * ((input_h - kernel_w + 1) / stride_h_);
      if (!col_len) {
        col_len = 1;
      }

      uint32_t input_c_group = input_c / groups_;
      uint32_t kernel_count_group = kernel_count / groups_;

      for (uint32_t g = 0; g < groups_; ++g) {
        std::vector<arma::fmat> kernel_matrix_arr(kernel_count_group);
        arma::fmat kernel_matrix_c(1, row_len * input_c_group);

        for (uint32_t k = 0; k < kernel_count_group; ++k) {
          const std::shared_ptr<Tensor<float>> &kernel = this->weights_.at(k + g * kernel_count_group);
          for (uint32_t ic = 0; ic < input_c_group; ++ic) {
            memcpy(kernel_matrix_c.memptr() + row_len * ic,
                   kernel->at(ic).memptr(), row_len * sizeof(float));
          }
          kernel_matrix_arr.at(k) = kernel_matrix_c;
        }

        arma::fmat input_matrix(input_c_group * row_len, col_len);
        arma::fmat input_matrix_c(row_len, col_len);

        for (uint32_t ic = 0; ic < input_c_group; ++ic) {
          float *input_matrix_c_ptr = input_matrix_c.memptr();
          const arma::fmat &input_channel = input_->at(ic + g * input_c_group);

          uint32_t offset_index = 0;
          for (uint32_t c = 0; c < input_w - kernel_w + 1; c += stride_w_) {
            for (uint32_t r = 0; r < input_h - kernel_h + 1; r += stride_h_) {
              for (uint32_t kw = 0; kw < kernel_w; ++kw) {
                const float *region_ptr = input_channel.colptr(c + kw) + r;
                memcpy(input_matrix_c_ptr + offset_index * kernel_h, region_ptr, kernel_h * sizeof(float));
                offset_index += 1;
              }
            }
          }
          input_matrix.submat(ic * row_len, 0, ((ic + 1) * row_len) - 1, col_len - 1) = input_matrix_c;
        }

        CHECK(output_tensor != nullptr);
        CHECK(output_tensor->channels() == kernel_count &&
            output_tensor->rows() == output_h && output_tensor->cols() == output_w);

        const uint32_t max_threads = kernel_count_group < 32 ? kernel_count_group : 32;
//#pragma omp parallel for num_threads(max_threads)
        for (uint32_t k = 0; k < kernel_count_group; ++k) {
          arma::fmat output = (kernel_matrix_arr.at(k)) * input_matrix;
          CHECK(output.size() == output_h * output_w);

          output.reshape(output_h, output_w);
          std::shared_ptr<Tensor<float>> bias;
          if (!this->bias_.empty() && this->use_bias_) {
            bias = this->bias_.at(k);
          }

          if (bias != nullptr) {
            float bias_value = bias->index(0);
            output += bias_value;
          }
          output_tensor->at(k + g * kernel_count_group) = std::move(output);
        }
      }
    }
  }
  return InferStatus::kInferSuccess;
}

ParseParameterAttrStatus ConvolutionLayer::GetInstance(const std::shared_ptr<RuntimeOperator> &op,
                                                       std::shared_ptr<Layer> &conv_layer) {
  CHECK(op != nullptr);
  const std::map<std::string, RuntimeParameter *> &params = op->params;

  if (params.find("in_channels") == params.end()) {
    LOG(ERROR) << "Can not find the in channel parameter";
    return ParseParameterAttrStatus::kParameterMissingInChannel;
  }

  const auto &in_channel = dynamic_cast<RuntimeParameterInt *>(params.at("in_channels"));
  if (!in_channel) {
    LOG(ERROR) << "Can not find the in channel parameter";
    return ParseParameterAttrStatus::kParameterMissingInChannel;
  }

  if (params.find("out_channels") == params.end()) {
    LOG(ERROR) << "Can not find the out channel parameter";
    return ParseParameterAttrStatus::kParameterMissingOutChannel;
  }

  const auto &out_channel = dynamic_cast<RuntimeParameterInt *>(params.at("out_channels"));
  if (!out_channel) {
    LOG(ERROR) << "Can not find the out channel parameter";
    return ParseParameterAttrStatus::kParameterMissingOutChannel;
  }

  if (params.find("padding") == params.end()) {
    LOG(ERROR) << "Can not find the padding parameter";
    return ParseParameterAttrStatus::kParameterMissingPadding;
  }

  const auto &padding = dynamic_cast<RuntimeParameterIntArray *>(params.at("padding"));
  if (!padding) {
    LOG(ERROR) << "Can not find the padding parameter";
    return ParseParameterAttrStatus::kParameterMissingPadding;
  }

  if (params.find("bias") == params.end()) {
    LOG(ERROR) << "Can not find the bias parameter";
    return ParseParameterAttrStatus::kParameterMissingUseBias;
  }
  const auto &use_bias = dynamic_cast<RuntimeParameterBool *>(params.at("bias"));
  if (!use_bias) {
    LOG(ERROR) << "Can not find the bias parameter";
    return ParseParameterAttrStatus::kParameterMissingUseBias;
  }

  if (params.find("stride") == params.end()) {
    LOG(ERROR) << "Can not find the stride parameter";
    return ParseParameterAttrStatus::kParameterMissingStride;
  }
  const auto &stride = dynamic_cast<RuntimeParameterIntArray *>(params.at("stride"));
  if (!stride) {
    LOG(ERROR) << "Can not find the stride parameter";
    return ParseParameterAttrStatus::kParameterMissingStride;
  }

  if (params.find("kernel_size") == params.end()) {
    LOG(ERROR) << "Can not find the kernel parameter";
    return ParseParameterAttrStatus::kParameterMissingKernel;
  }
  const auto &kernel = dynamic_cast<RuntimeParameterIntArray *>(params.at("kernel_size"));
  if (!kernel) {
    LOG(ERROR) << "Can not find the kernel parameter";
    return ParseParameterAttrStatus::kParameterMissingKernel;
  }

  const auto &groups = dynamic_cast<RuntimeParameterInt *>(params.at("groups"));
  if (!groups) {
    LOG(ERROR) << "Can not find the groups parameter";
    return ParseParameterAttrStatus::kParameterMissingGroups;
  }

  const uint32_t dims = 2;
  const std::vector<int> &kernels = kernel->value;
  const std::vector<int> &paddings = padding->value;
  const std::vector<int> &strides = stride->value;
  CHECK(kernels.size() == dims);
  CHECK(strides.size() == dims);
  CHECK(paddings.size() == dims);

  // kernel的方向是倒置的
  conv_layer = std::make_shared<ConvolutionLayer>(out_channel->value, in_channel->value,
                                                  kernels.at(0), kernels.at(1), paddings.at(0),
                                                  paddings.at(1), strides.at(0), strides.at(1),
                                                  groups->value, use_bias->value);

  // load weights
  const std::map<std::string, std::shared_ptr<RuntimeAttribute>> &attrs = op->attribute;
  if (use_bias->value) {
    if (attrs.find("bias") == attrs.end()) {
      LOG(ERROR) << "Can not find the bias attribute";
      return ParseParameterAttrStatus::kAttrMissingBias;
    }
    const auto &bias = attrs.at("bias");
    const std::vector<int> &bias_shape = bias->shape;
    if (bias_shape.empty() || bias_shape.at(0) != out_channel->value) {
      LOG(ERROR) << "Bias shape is wrong";
      return ParseParameterAttrStatus::kAttrMissingBias;
    }

    const std::vector<float> &bias_values = bias->get<float>();
    conv_layer->set_bias(bias_values);
  }

  if (attrs.find("weight") == attrs.end()) {
    LOG(ERROR) << "Can not find the weight attribute";
    return ParseParameterAttrStatus::kAttrMissingWeight;
  }

  const auto &weight = attrs.at("weight");
  const std::vector<int> &weight_shape = weight->shape;
  if (weight_shape.empty()) {
    LOG(ERROR) << "Weight shape is empty";
    return ParseParameterAttrStatus::kAttrMissingWeight;
  }

  const std::vector<float> &weight_values = weight->get<float>();
  conv_layer->set_weights(weight_values);
  return ParseParameterAttrStatus::kParameterAttrParseSuccess;
}

LayerRegistererWrapper kConvGetInstance("nn.Conv2d", ConvolutionLayer::GetInstance);

}