/*
 * Copyright (c) 2018-2020 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/core/CL/kernels/CLWidthConcatenate2TensorsKernel.h"

#include "arm_compute/core/AccessWindowStatic.h"
#include "arm_compute/core/CL/CLHelpers.h"
#include "arm_compute/core/CL/CLKernelLibrary.h"
#include "arm_compute/core/CL/CLValidate.h"
#include "arm_compute/core/CL/ICLTensor.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Window.h"
#include "arm_compute/core/utils/helpers/tensor_info.h"
#include "arm_compute/core/utils/misc/Cast.h"
#include "arm_compute/core/utils/misc/ShapeCalculator.h"

#include "support/StringSupport.h"

namespace arm_compute
{
namespace
{
constexpr unsigned int num_elems_processed_per_iteration = 8;

std::pair<Status, Window> validate_and_configure_window(ITensorInfo *input1, ITensorInfo *input2, ITensorInfo *output)
{
    // The window needs to be based on the output
    Window             win = calculate_max_window(*output, Steps(num_elems_processed_per_iteration));
    AccessWindowStatic input1_access(input1, 0, 0, ceil_to_multiple(input1->dimension(0), num_elems_processed_per_iteration), input1->dimension(1));
    const unsigned int input2_right_padding = ((output->dimension(0) / num_elems_processed_per_iteration) * num_elems_processed_per_iteration - input1->dimension(0) - input2->dimension(
                                                   0)) % num_elems_processed_per_iteration;
    AccessWindowStatic input2_access(input2, -(input1->dimension(0) % num_elems_processed_per_iteration),
                                     0, input2->dimension(0) + input2_right_padding, input2->dimension(1));
    AccessWindowHorizontal output_access(output, 0, num_elems_processed_per_iteration);
    bool                   window_changed = update_window_and_padding(win, input1_access, input2_access, output_access);

    Window win_collapsed = win.collapse(win, Window::DimZ);

    Status err = (window_changed) ? ARM_COMPUTE_CREATE_ERROR(ErrorCode::RUNTIME_ERROR, "Insufficient Padding!") : Status{};
    return std::make_pair(err, win_collapsed);
}
Status validate_arguments(const ITensorInfo *input1, const ITensorInfo *input2, const ITensorInfo *output)
{
    ARM_COMPUTE_RETURN_ERROR_ON_NULLPTR(input1, input2, output);
    ARM_COMPUTE_RETURN_ERROR_ON_F16_UNSUPPORTED(input1);
    ARM_COMPUTE_RETURN_ERROR_ON(input1->data_type() == DataType::UNKNOWN);
    ARM_COMPUTE_RETURN_ERROR_ON_MISMATCHING_DATA_TYPES(input1, input2, output);
    ARM_COMPUTE_RETURN_ERROR_ON(input1->dimension(0) + input2->dimension(0) > output->dimension(0));

    for(size_t i = 1; i < Coordinates::num_max_dimensions; ++i)
    {
        ARM_COMPUTE_RETURN_ERROR_ON(input1->dimension(i) != output->dimension(i));
        ARM_COMPUTE_RETURN_ERROR_ON(input2->dimension(i) != output->dimension(i));
    }
    ARM_COMPUTE_RETURN_ERROR_ON(input1->num_dimensions() > 4);

    return Status{};
}
} // namespace

Status CLWidthConcatenate2TensorsKernel::validate(const ITensorInfo *input1, const ITensorInfo *input2, const ITensorInfo *output)
{
    ARM_COMPUTE_RETURN_ON_ERROR(validate_arguments(input1, input2, output));
    ARM_COMPUTE_RETURN_ON_ERROR(validate_and_configure_window(input1->clone().get(), input2->clone().get(), output->clone().get()).first);
    return Status{};
}

void CLWidthConcatenate2TensorsKernel::configure(const CLCompileContext &compile_context, ITensorInfo *input1, ITensorInfo *input2, ITensorInfo *output)
{
    ARM_COMPUTE_ERROR_ON_NULLPTR(input1, input2, output);
    ARM_COMPUTE_ERROR_THROW_ON(validate_arguments(input1, input2, output));

    // Add build options
    CLBuildOptions build_opts;
    build_opts.add_option("-DDATA_TYPE=" + get_cl_type_from_data_type(input1->data_type()));
    build_opts.add_option("-DVEC_SIZE=" + support::cpp11::to_string(num_elems_processed_per_iteration));
    build_opts.add_option("-DDEPTH=" + support::cpp11::to_string(input1->dimension(2)));
    build_opts.add_option("-DINPUT1_WIDTH=" + support::cpp11::to_string(input1->dimension(0)));
    build_opts.add_option("-DELEMENT_SIZE=" + support::cpp11::to_string(input1->element_size()));

    // If input have different quantization info set quantization parameters needed for the re-quantization process
    const bool have_different_qinfo = helpers::tensor_info::tensors_have_different_quantization_info(output, input1, input2);
    if(is_data_type_quantized_asymmetric(input1->data_type()) && have_different_qinfo)
    {
        const UniformQuantizationInfo iq1_info = input1->quantization_info().uniform();
        const UniformQuantizationInfo iq2_info = input2->quantization_info().uniform();
        const UniformQuantizationInfo oq_info  = output->quantization_info().uniform();

        build_opts.add_option("-DOFFSET_IN1=" + float_to_string_with_full_precision(iq1_info.offset));
        build_opts.add_option("-DSCALE_IN1=" + float_to_string_with_full_precision(iq1_info.scale));
        build_opts.add_option("-DOFFSET_IN2=" + float_to_string_with_full_precision(iq2_info.offset));
        build_opts.add_option("-DSCALE_IN2=" + float_to_string_with_full_precision(iq2_info.scale));
        build_opts.add_option("-DOFFSET_OUT=" + float_to_string_with_full_precision(oq_info.offset));
        build_opts.add_option("-DSCALE_OUT=" + float_to_string_with_full_precision(oq_info.scale));
    }

    // Create kernel
    _kernel = create_kernel(compile_context, "concatenate_width_x2", build_opts.options());

    // Configure kernel window
    auto win_config = validate_and_configure_window(input1, input2, output);
    ARM_COMPUTE_ERROR_THROW_ON(std::get<0>(win_config));

    ICLKernel::configure_internal(std::get<1>(win_config));

    // Set output valid region
    output->set_valid_region(ValidRegion(Coordinates(), output->tensor_shape()));

    // Pass paddings as arguments to the kernel
    const unsigned int input1_width         = input1->dimension(0);
    const unsigned int input1_right_padding = ceil_to_multiple(input1_width, num_elems_processed_per_iteration) - input1_width;
    const unsigned int input2_left_padding  = input1_width % num_elems_processed_per_iteration;
    unsigned int       idx0                 = 3 * num_arguments_per_4D_tensor();
    _kernel.setArg<cl_uint>(idx0++, input1_right_padding);
    _kernel.setArg<cl_uint>(idx0++, input2_left_padding);

    // Set config_id for enabling LWS tuning
    _config_id = "concatenate_width_x2_";
    _config_id += lower_string(string_from_data_type(input1->data_type()));
    _config_id += "_";
    _config_id += support::cpp11::to_string(input1->dimension(0));
    _config_id += "_";
    _config_id += support::cpp11::to_string(input1->dimension(1));
    _config_id += "_";
    _config_id += support::cpp11::to_string(input2->dimension(0));
    _config_id += "_";
    _config_id += support::cpp11::to_string(input2->dimension(1));
}

void CLWidthConcatenate2TensorsKernel::run_op(ITensorPack &tensors, const Window &window, cl::CommandQueue &queue)
{
    ARM_COMPUTE_ERROR_ON_UNCONFIGURED_KERNEL(this);
    ARM_COMPUTE_ERROR_ON_INVALID_SUBWINDOW(ICLKernel::window(), window);

    Window slice = window.first_slice_window_4D();

    const auto src0 = utils::cast::polymorphic_downcast<const ICLTensor *>(tensors.get_const_tensor(TensorType::ACL_SRC_VEC));
    const auto src1 = utils::cast::polymorphic_downcast<const ICLTensor *>(tensors.get_const_tensor(TensorType::ACL_SRC_VEC + 1));
    auto       dst  = utils::cast::polymorphic_downcast<ICLTensor *>(tensors.get_tensor(TensorType::ACL_DST));

    do
    {
        unsigned int idx = 0;
        add_4D_tensor_argument(idx, src0, slice);
        add_4D_tensor_argument(idx, src1, slice);
        add_4D_tensor_argument(idx, dst, slice);
        enqueue(queue, *this, window, lws_hint());
    }
    while(window.slide_window_slice_4D(slice));
}
} // namespace arm_compute
