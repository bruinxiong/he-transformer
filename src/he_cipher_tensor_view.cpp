/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <memory>
#include <string>

#include "he_backend.hpp"
#include "he_cipher_tensor_view.hpp"
#include "ngraph/descriptor/layout/dense_tensor_view_layout.hpp"

using namespace ngraph;
using namespace std;

runtime::he::HECipherTensorView::HECipherTensorView(const element::Type& element_type,
                                                    const Shape& shape,
                                                    shared_ptr<HEBackend> he_backend,
                                                    const string& name)
    : runtime::he::HETensorView(element_type, shape, he_backend)
{
    // get_tensor_view_layout()->get_size() is the number of elements
    m_num_elements = m_descriptor->get_tensor_view_layout()->get_size();
    if (m_num_elements > 0)
    {
        for (size_t i = 0; i < m_num_elements; ++i)
        {
            m_cipher_texts.push_back(make_shared<seal::Ciphertext>());
        }
    }
}

runtime::he::HECipherTensorView::~HECipherTensorView()
{
}

void runtime::he::HECipherTensorView::check_io_bounds(const void* source,
                                                      size_t tensor_offset,
                                                      size_t n) const
{
    const element::Type& type = get_element_type();
    size_t type_byte_size = type.size();

    // Memory must be byte-aligned to type_byte_size
    // tensor_offset and n are all in bytes
    if (tensor_offset % type_byte_size != 0 || n % type_byte_size != 0)
    {
        throw ngraph_error("tensor_offset and n must be divisable by type_byte_size.");
    }
    // Check out-of-range
    if ((tensor_offset + n) / type_byte_size > m_num_elements)
    {
        throw out_of_range("I/O access past end of tensor");
    }
}

void runtime::he::HECipherTensorView::write(const void* source, size_t tensor_offset, size_t n)
{
    check_io_bounds(source, tensor_offset, n);
    const element::Type& type = get_element_type();
    size_t type_byte_size = type.size();
    size_t dst_start_index = tensor_offset / type_byte_size;
    size_t num_elements_to_write = n / type_byte_size;
    for (size_t i = 0; i < num_elements_to_write; ++i)
    {
        const void* src_with_offset = (void*)((char*)source + i * type.size());
        size_t dst_index = dst_start_index + i;
        seal::Plaintext p;
        m_he_backend->encode(p, src_with_offset, type);
        m_he_backend->encrypt(*(m_cipher_texts[dst_index]), p);
    }
}

void runtime::he::HECipherTensorView::read(void* target, size_t tensor_offset, size_t n) const
{
    check_io_bounds(target, tensor_offset, n);
    const element::Type& type = get_element_type();
    size_t type_byte_size = type.size();
    size_t src_start_index = tensor_offset / type_byte_size;
    size_t num_elements_to_read = n / type_byte_size;
    for (size_t i = 0; i < num_elements_to_read; ++i)
    {
        void* dst_with_offset = (void*)((char*)target + i * type.size());
        size_t src_index = src_start_index + i;
        seal::Plaintext p;
        m_he_backend->decrypt(p, *(m_cipher_texts[src_index]));
        m_he_backend->decode(dst_with_offset, p, type);
    }
}

size_t runtime::he::HECipherTensorView::get_size() const
{
    return get_tensor_view_layout()->get_size();
}

const element::Type& runtime::he::HECipherTensorView::get_element_type() const
{
    return get_tensor_view_layout()->get_element_type();
}