// Copyright 2024 The FlatFlow Authors
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

#ifndef FLATFLOW_SCHEDULER_SCHEDULER_H_
#define FLATFLOW_SCHEDULER_SCHEDULER_H_

#include <omp.h>

#include <cassert>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "flatbuffers/vector.h"

#include "flatflow/data/dataset.h"
#include "flatflow/data/internal/types.h"
#include "flatflow/scheduler/internal/algorithm/concat.h"
#include "flatflow/scheduler/internal/algorithm/partition.h"
#include "flatflow/scheduler/internal/algorithm/passive_aggressive.h"
#include "flatflow/scheduler/internal/algorithm/reshape.h"
#include "flatflow/scheduler/internal/algorithm/shuffle.h"

namespace flatflow {
namespace scheduler {

// flatflow::scheduler::Scheduler<>
//
// A common base class for all scheduler implementations.
// There are several scheduling policies on how to distribute the given data,
// and each policy has its own partial template specialization.
//
// Note that this scheduling policy is only effective for models with linear
// complexity in the size of each data sample; traditional convolutional neural
// networks (CNNs) and state space models (SSMs) in the Mamba family that
// implement linear-time sequence modeling are of this kind.
template <typename Index, typename Size, int Order, bool Heterogeneous>
  requires(flatflow::data::internal::Unsigned<Index> &&
           flatflow::data::internal::Unsigned<Size>)
class Scheduler {
 public:
  using key_type = Size;
  using mapped_type = Index;

  // Constructors and assignment operators
  //
  // In addition to the below constructor to set up scheduling,
  // a `flatflow::scheduler::Scheduler<>` supports copy and move constructors
  // and assignment operators; the default constructor, on the other hand, is
  // not available since the scheduler is initialized using `std::variant` and
  // `std::monostate` to select one of several scheduling policies at runtime
  // without dynamic dispatch overhead.
  //
  // Note that unlike `flatflow::data::Dataset<>`, the constructors of scheduler
  // are not specified as `explicit` since an implicit conversion from scheduler
  // to `std::variant` is required.
  Scheduler(const flatbuffers::Vector<key_type, mapped_type> *sizes,
            const mapped_type &data_parallel_size,
            const mapped_type &global_batch_size,
            const mapped_type &micro_batch_size, const mapped_type &seed)
      : data_parallel_size_(data_parallel_size),
        global_batch_size_(global_batch_size),
        micro_batch_size_(micro_batch_size),
        seed_(seed) {
    assert(data_parallel_size != 0);
    assert(global_batch_size != 0);
    assert(global_batch_size % data_parallel_size == 0);
    assert(micro_batch_size != 0);
    assert(global_batch_size / data_parallel_size % micro_batch_size == 0);
    assert(sizes != nullptr);
    assert(sizes->size() != 0);
    assert(sizes->size() % data_parallel_size == 0);

    // (x - 1) / y + 1 is always equal to x % y == 0 ? x / y : x / y + 1 without
    // any branch instructions.
    num_micro_batches_ =
        ((sizes->size() / data_parallel_size - 1) / micro_batch_size + 1) *
        data_parallel_size;

    // The last micro-batch size must be calculated since the total number of
    // data samples is guaranteed to be a multiple of data parallel size, but
    // may not be divisible by the micro-batch size.
    //
    // (x - 1) % y + 1 is always equal to x % y == 0 ? y : x % y without any
    // branch instructions.
    last_micro_batch_size_ =
        (sizes->size() / data_parallel_size - 1) % micro_batch_size + 1;

    // The below copy assignment is actually not copied but direct-initialized
    // by copy elision.
    dataset_ = flatflow::data::Dataset(sizes, seed);
  }

  Scheduler() = delete;

  Scheduler(const Scheduler &other) = default;

  Scheduler &operator=(const Scheduler &other) = default;

  Scheduler(Scheduler &&other) = default;

  Scheduler &operator=(Scheduler &&other) = default;

  // Scheduler::Schedule()
  //
  // Makes schedules for the next training epoch and then shuffles them.
  //
  // Note that this scheduler discards the scheduling interval; scheduling
  // for models with linear complexity on identical machines occurs at the
  // granularity of epoch.
  std::vector<std::vector<mapped_type>> Schedule() {
    auto now = omp_get_wtime();

    if (micro_batch_size_ == last_micro_batch_size_) {
      const auto items = dataset_.take(
          static_cast<std::size_t>(micro_batch_size_ * num_micro_batches_));
      const auto micro_batches = internal::algorithm::KarmarkarKarp(
          items, num_micro_batches_,
          flatflow::data::internal::OverflowSafeCast<key_type>);

      LOG(INFO) << absl::StrFormat("Partitioning into %u micro-batches took %fs", num_micro_batches_, omp_get_wtime() - now);
      now = omp_get_wtime();

      const auto indices = internal::algorithm::reshape(
          internal::algorithm::shuffle(micro_batches, epoch_ + seed_),
          data_parallel_size_, global_batch_size_);

      LOG(INFO) << absl::StrFormat("Epoch: %u inter-batch shuffling took %fs", epoch_, omp_get_wtime() - now);

      return indices;
    }

    const auto items = dataset_.take(static_cast<std::size_t>(
        micro_batch_size_ * (num_micro_batches_ - data_parallel_size_)));
    const auto micro_batches = internal::algorithm::KarmarkarKarp(
        items, num_micro_batches_ - data_parallel_size_,
        flatflow::data::internal::OverflowSafeCast<key_type>);

    const auto last_items = dataset_.take(
        static_cast<std::size_t>(last_micro_batch_size_ * data_parallel_size_));
    const auto last_micro_batches = internal::algorithm::KarmarkarKarp(
        last_items, data_parallel_size_,
        flatflow::data::internal::OverflowSafeCast<key_type>);

    LOG(INFO) << absl::StrFormat("Partitioning into %u micro-batches took %fs", num_micro_batches_, omp_get_wtime() - now);
    now = omp_get_wtime();

    auto indices = internal::algorithm::reshape(
        internal::algorithm::shuffle(micro_batches, epoch_ + seed_),
        data_parallel_size_, global_batch_size_);

    const auto last_indices = internal::algorithm::reshape(
        internal::algorithm::shuffle(last_micro_batches, epoch_ + seed_),
        data_parallel_size_, global_batch_size_);

    internal::algorithm::concat(indices, last_indices);

    LOG(INFO) << absl::StrFormat("Epoch: %u inter-batch shuffling took %fs", epoch_, omp_get_wtime() - now);

    return indices;
  }

  // Scheduler::on_batch_begin()
  //
  // A callback to be called at the beginning of a training batch.
  inline void on_batch_begin(const mapped_type &batch) const noexcept {
    dataset_.on_batch_begin(batch);
  }

  // Scheduler::on_batch_end()
  //
  // A callback to be called at the end of a training batch.
  inline void on_batch_end(
      const mapped_type &batch, [[maybe_unused]] const mapped_type &rank,
      [[maybe_unused]] const flatbuffers::Vector<double, mapped_type> *costs)
      const noexcept {
    dataset_.on_batch_end(batch);
  }

  // Scheduler::on_epoch_begin()
  //
  // A callback to be called at the beginning of an epoch.
  inline void on_epoch_begin(const mapped_type &epoch) {
    epoch_ = epoch;
    dataset_.on_epoch_begin(epoch);
  }

  // Scheduler::on_epoch_end()
  //
  // A callback to be called at the end of an epoch.
  inline void on_epoch_end(const mapped_type &epoch) {
    dataset_.on_epoch_end(epoch);
  }

  // Scheduler::on_train_begin()
  //
  // A callback to be called at the beginning of training.
  inline void on_train_begin() const noexcept { dataset_.on_train_begin(); }

  // Scheduler::on_train_end()
  //
  // A callback to be called at the end of training.
  inline void on_train_end() const noexcept { dataset_.on_train_end(); }

 protected:
  mapped_type data_parallel_size_;
  mapped_type epoch_;
  mapped_type global_batch_size_;
  mapped_type last_micro_batch_size_;
  mapped_type micro_batch_size_;
  mapped_type num_micro_batches_;
  mapped_type seed_;
  flatflow::data::Dataset<mapped_type, key_type> dataset_;
};

}  // namespace scheduler
}  // namespace flatflow

#endif  // FLATFLOW_SCHEDULER_SCHEDULER_H_
