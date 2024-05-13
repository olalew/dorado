#pragma once

#include "correct/types.h"
#include "hts_io/FastxRandomReader.h"
#include "read_pipeline/MessageSink.h"
#include "read_pipeline/messages.h"
#include "utils/AsyncQueue.h"
#include "utils/stats.h"
#include "utils/types.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dorado {

class CorrectionNode : public MessageSink {
public:
    CorrectionNode(const std::string& fastq,
                   int threads,
                   const std::string& device,
                   int infer_threads,
                   int bach_size,
                   const std::string& model_path);
    ~CorrectionNode() { stop_input_processing(); }
    std::string get_name() const override { return "CorrectionNode"; }
    stats::NamedStats sample_stats() const override;
    void terminate(const FlushOptions&) override;
    void restart() override { start_input_processing(&CorrectionNode::input_thread_fn, this); }

private:
    const std::string m_fastq;
    void input_thread_fn();
    const int m_window_size = 4096;
    int m_batch_size;
    std::string m_model_path;

    void infer_fn(const std::string& device, int mtx_idx);
    void decode_fn();

    void concat_features_and_send(const std::vector<std::string>& seqs,
                                  const std::string& read_name);

    utils::AsyncQueue<correction::WindowFeatures> m_features_queue;
    utils::AsyncQueue<correction::WindowFeatures> m_inferred_features_queue;

    std::vector<std::thread> m_infer_threads;
    std::vector<std::thread> m_decode_threads;

    std::atomic<int> num_reads;
    std::atomic<int> num_early_reads;

    std::unordered_map<std::string, std::vector<std::string>> m_features_by_id;
    std::unordered_map<std::string, int> m_pending_features_by_id;
    std::mutex m_features_mutex;

    std::atomic<int> m_num_active_feature_threads{0};
    std::atomic<int> m_num_active_infer_threads{0};

    std::array<std::mutex, 32> m_gpu_mutexes;

    // Class to pre-allocate memory and generate tensors from it.
    template <typename T>
    class MemoryManager {
    public:
        MemoryManager(int batch_size) {
            const size_t num_tensors = 8 * 8;  // devices * threads per device;
            size_t tensor_size = WS * NR * batch_size;
            m_bases_ptr = std::make_unique<T[]>(tensor_size * num_tensors);

            for (size_t i = 0; i < num_tensors; i++) {
                m_bases_locations.push(&m_bases_ptr.get()[i * tensor_size]);
            }
        };

        ~MemoryManager() = default;

        T* get_next_ptr() {
            std::lock_guard<std::mutex> lock(m_bases_mtx);
            if (m_bases_locations.size() == 0) {
                throw std::runtime_error("No more pointers left!");
            }
            auto next_ptr = m_bases_locations.front();
            m_bases_locations.pop();
            return next_ptr;
        }

        void return_ptr(T* ptr) {
            std::lock_guard<std::mutex> lock(m_bases_mtx);
            m_bases_locations.push(ptr);
        }

    private:
        static constexpr int WS = 5120;
        static constexpr int NR = 31;

        std::unique_ptr<T[]> m_bases_ptr;
        std::queue<T*> m_bases_locations;
        std::mutex m_bases_mtx;
    };

    MemoryManager<int> m_bases_manager;
    MemoryManager<float> m_quals_manager;
};

}  // namespace dorado
