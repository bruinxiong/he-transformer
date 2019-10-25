//*****************************************************************************
// Copyright 2018-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "aby/aby_executor.hpp"
#include "seal/kernel/subtract_seal.hpp"
#include "seal/seal_util.hpp"

namespace ngraph {
namespace aby {

ABYExecutor::ABYExecutor(std::string role, std::string mpc_protocol,
                         he::HESealExecutable& he_seal_executable,
                         std::string hostname, std::size_t port,
                         uint64_t security_level, uint32_t bit_length,
                         uint32_t num_threads, std::string mg_algo_str,
                         uint32_t reserve_num_gates,
                         const std::string& circuit_directiory)
    : m_num_threads{num_threads}, m_he_seal_executable{he_seal_executable} {
  std::map<std::string, e_role> role_map{{"server", SERVER},
                                         {"client", CLIENT}};

  auto role_it = role_map.find(ngraph::to_lower(role));
  NGRAPH_CHECK(role_it != role_map.end(), "Unknown role ", role);
  m_role = role_it->second;

  std::map<std::string, e_sharing> protocol_map{{"yao", S_YAO},
                                                {"gmw", S_BOOL}};

  auto protocol_it = protocol_map.find(ngraph::to_lower(mpc_protocol));
  NGRAPH_CHECK(role_it != role_map.end(), "Unknown role ", role);
  NGRAPH_CHECK(protocol_it != protocol_map.end(), "Unknown mpc_protocol ",
               mpc_protocol);
  m_aby_gc_protocol = protocol_it->second;

  NGRAPH_CHECK(mg_algo_str == "MT_OT", "Unknown mg_algo_str ", mg_algo_str);
  m_mt_gen_alg = MT_OT;

  NGRAPH_CHECK(security_level == 128, "Unsupported security level ",
               security_level);
  m_security_level = security_level;

  m_ABYParty =
      new ABYParty(m_role, hostname, port, get_sec_lvl(m_security_level),
                   bit_length, m_num_threads, m_mt_gen_alg, reserve_num_gates);
  // TODO: connect and base OTs?
  //  m_ABYParty_server->ConnectAndBaseOTs();
  NGRAPH_HE_LOG(1) << "Started ABYParty with role " << role;

  m_random_distribution = std::uniform_int_distribution<int64_t>{0, m_rand_max};
}

ABYExecutor::~ABYExecutor() {
  // TODO: delete ABYParty
}

std::shared_ptr<he::HETensor> ABYExecutor::generate_gc_mask(
    const ngraph::Shape& shape, bool plaintext_packing, bool complex_packing,
    const std::string& name, bool random, uint64_t default_value) {
  auto tensor = std::make_shared<he::HETensor>(
      element::i64, shape, plaintext_packing, complex_packing, false,
      m_he_seal_executable.he_seal_backend(), name);

  std::vector<uint64_t> rand_vals(tensor->get_element_count());
  if (random) {
    auto random_gen = [this]() {
      return m_random_distribution(m_random_generator);
    };
    std::generate(rand_vals.begin(), rand_vals.end(), random_gen);
  } else {
    rand_vals = std::vector<uint64_t>(rand_vals.size(), default_value);
  }
  tensor->write(rand_vals.data(), rand_vals.size() * sizeof(uint64_t));

  return tensor;
}

std::shared_ptr<he::HETensor> ABYExecutor::generate_gc_input_mask(
    const ngraph::Shape& shape, bool plaintext_packing, bool complex_packing,
    uint64_t default_value) {
  return generate_gc_mask(
      shape, plaintext_packing, complex_packing, "gc_input_mask",
      m_he_seal_executable.he_seal_backend().mask_gc_inputs(), default_value);
}

std::shared_ptr<he::HETensor> ABYExecutor::generate_gc_output_mask(
    const ngraph::Shape& shape, bool plaintext_packing, bool complex_packing,
    uint64_t default_value) {
  return generate_gc_mask(
      shape, plaintext_packing, complex_packing, "gc_output_mask",
      m_he_seal_executable.he_seal_backend().mask_gc_outputs(), default_value);
}

void ABYExecutor::mask_input_unknown_relu_ciphers_batch(
    std::vector<he::HEType>& cipher_batch) {
  NGRAPH_HE_LOG(3) << "mask_input_unknown_relu_ciphers_batch ";

  bool plaintext_packing = cipher_batch[0].plaintext_packing();
  bool complex_packing = cipher_batch[0].complex_packing();
  size_t batch_size = cipher_batch[0].batch_size();

  NGRAPH_INFO << "Generating gc input mask";

  m_gc_input_mask =
      generate_gc_input_mask(Shape{batch_size, cipher_batch.size()},
                             plaintext_packing, complex_packing);

  NGRAPH_INFO << "Generating gc output mask";

  m_gc_output_mask =
      generate_gc_output_mask(Shape{batch_size, cipher_batch.size()},
                              plaintext_packing, complex_packing);

  std::vector<double> scales(cipher_batch.size());

  for (size_t i = 0; i < cipher_batch.size(); ++i) {
    auto& he_type = cipher_batch[i];
    auto& gc_input_mask = m_gc_input_mask->data(i);
    NGRAPH_CHECK(he_type.is_ciphertext(), "HEType is not ciphertext");

    auto cipher = he_type.get_ciphertext();

    auto chain_ind =
        m_he_seal_executable.he_seal_backend().get_chain_index(*cipher);

    NGRAPH_INFO << "chain ind " << chain_ind;

    NGRAPH_INFO << "Mod switchign to lowest";

    // Swith modulus to lowest values since mask values are drawn
    // from (-q/2, q/2) for q the lowest coeff modulus
    m_he_seal_executable.he_seal_backend().rescale_to_lowest(*cipher);

    // Divide by scale so we can encode at the same scale as existing
    // ciphertext
    double scale = cipher->ciphertext().scale();
    scales[i] = scale;

    NGRAPH_INFO << "scaling input mask";

    he::HEPlaintext scaled_gc_input_mask(gc_input_mask.get_plaintext());
    for (size_t mask_idx = 0; mask_idx < scaled_gc_input_mask.size();
         ++mask_idx) {
      scaled_gc_input_mask[mask_idx] /= scale;
    }

    NGRAPH_INFO << "scalar_subtract_seal";
    scalar_subtract_seal(*cipher, scaled_gc_input_mask, cipher,
                         he_type.complex_packing(),
                         m_he_seal_executable.he_seal_backend());
  }
}

}  // namespace aby
}  // namespace ngraph