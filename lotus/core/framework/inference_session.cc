#include "core/framework/inference_session.h"

#include <mutex>

#include "core/common/logging.h"
#include "core/framework/op_kernel.h"
#include "core/graph/graph.h"
#include "core/graph/model.h"
#include "core/lib/threadpool.h"
#include "core/framework/executor.h"
#include "core/framework/session_state.h"
#include "core/platform/notification.h"
#include "core/framework/kernel_def_builder.h"

namespace Lotus {

class InferenceSession::Impl {
 public:
  Impl(const SessionOptions& session_options)
      : env_(Env::Default()),
        thread_pool_(env_, "Compute", session_options.num_threads) {
    auto& provider_mgr = ExecutionProviderMgr::Instance();
    for (auto& info : session_options.ep_infors)
    {
        auto provider = provider_mgr.GetProvider(info.Name(), info);
        if (provider == nullptr)
        {
            LOG(WARNING) << "Execution Provider with name: "
                << info.Name() << "Not found.";
            continue;
        }
        execution_providers_.emplace_back(std::move(provider));
    }
  }

  // TODO add the methods of the parent class

  Common::Status Load(const std::string& model_uri) {
    std::lock_guard<std::mutex> l(session_mutex_);
    std::shared_ptr<Model> tmp_model_ptr;
    Common::Status st = Model::Load(model_uri, &tmp_model_ptr);
    if (st.IsOK()) {
      is_model_loaded_ = true;
      model_ = tmp_model_ptr;
      session_state_.Init(model_->MainGraph());
    }

    return st;
  }

  Common::Status Initialize() {
    std::lock_guard<std::mutex> l(session_mutex_);
    if (!is_model_loaded_) {
      return Common::Status(Common::LOTUS, Common::FAIL, "Model was not loaded.");
    }

    Common::Status st = TransformGraph();
    if (!st.IsOK()) {
      return st;
    }

    st = ConstructKernels();
    if (!st.IsOK()) {
      return st;
    }

    // TODO add other per session initialization stuff here

    is_inited_ = true;
    return Status::OK();
  }

  int GetCurrentNumRuns() {
    return current_num_runs_.load();
  }

  Common::Status Run(const std::vector<MLValue>& feeds, std::vector<MLValue>* p_fetches) {
    RunOptions run_options;
    return Run(run_options, feeds, p_fetches);
  }

  Common::Status Run(const RunOptions& run_options,
                     const std::vector<MLValue>& feeds,
                     std::vector<MLValue>* p_fetches) {
    {
      std::lock_guard<std::mutex> l(session_mutex_);
      if (!is_inited_) {
        // return Common::Status(Common::LOTUS, Common::FAIL, "Session not initialized."); // TODO commenting for now until we've a real graph
      }
    }

    // TODO add instrumentation to measure the time taken for this Run
    
    LOG(INFO) << "Running with tag: " << run_options.run_tag << std::endl;
    ++current_num_runs_;
    
    // TODO should we add this exec to the list of executors? i guess its not needed now?

    struct RunStatus {
      std::unique_ptr<Executor> p_exec;
      Common::Status status; // used to collect the status from the executor
      Notification executor_done;
    };
    
    RunStatus run_status;
    if (run_options.enable_sequential_execution) {
      run_status.p_exec = std::move(Executor::NewSequentialExecutor(session_state_));
    } else {
      run_status.p_exec = std::move(Executor::NewParallelExecutor(session_state_));
    }
    
    thread_pool_.Schedule([&run_options, &feeds, p_fetches, &run_status]() {
        Common::Status local_status = run_status.p_exec->Execute(run_options, feeds, p_fetches);        
	run_status.status = local_status;
        run_status.executor_done.Notify();
      });

    // this is a blocking Run, hence wait to be notified by the above closure when the executor is done
    Common::Status waitStatus = WaitForNotification(&run_status.executor_done, run_options.timeout_in_ms);
    Common::Status retval;

    if (!waitStatus.IsOK()) {
      // TODO should we cancel the thread in the pool that corresponds to this executor?      
      retval = waitStatus;
    } else {
      retval = run_status.status;
    }

    --current_num_runs_;
    return retval;
  }
  
 private:
  Common::Status TransformGraph() {
    for (auto& ep: execution_providers_) {
      bool is_modified;
      ep->GetTransformer().Apply(*session_state_.p_graph_, is_modified);
    }
    return Status::OK();
  }

  Common::Status ConstructKernels() {
    Graph* graph = session_state_.p_graph_;
    for (auto node_it = graph->Nodes_begin(); node_it!=graph->Nodes_end(); ++node_it) {
      const std::string& opId = (*node_it)->OpType();
      AllocatorInfo allocator_info("CPUAllocator", Lotus::AllocatorType::ArenaAllocator);
      OpKernelInfo op_kernel_info {*(*node_it), allocator_info};
      std::unique_ptr<OpKernel> op_kernel_ptr = CreateOpKernel(opId, &op_kernel_info);
      if (!op_kernel_ptr) {
        LOG(ERROR) << "Couldn't create kernel for opId: " << opId << std::endl;
        return Common::Status(Common::LOTUS,
                              Common::FAIL,
                              "Failed to initialize session because kernel creation failed");
      }
      session_state_.AddKernel((*node_it)->Index(), std::move(op_kernel_ptr));
    }

    return Status::OK();
  }

  Common::Status WaitForNotification(Notification* p_executor_done, int64 timeout_in_ms) {
    if (timeout_in_ms > 0) {
      LOTUS_NOT_IMPLEMENTED; // TODO
    } else {
      p_executor_done->WaitForNotification();
    }
    return Status::OK();    
  }
  
  // The model served by this inference session instance.
  std::shared_ptr<Model> model_;
  
  // The list of execution providers in preference order.
  std::vector<ExecutionProviderPtr> execution_providers_;

  // A set of executors that can run in parallel.
  std::vector<std::unique_ptr<Executor>> executors_; // TODO do we need this vector?

  // State for each op in the model. Shared by all executors.
  SessionState session_state_;

  // Environment for this session
  Env* env_; // statically allocated pointer, no need to manage its lifetime.

  // Threadpool for this session
  thread::ThreadPool thread_pool_;

  // Number of concurrently running executors
  std::atomic<int> current_num_runs_;

  std::mutex session_mutex_; // to ensure exclusive invocation of Load and Initialize
  bool is_model_loaded_ = false; // GUARDED_BY(session_mutex_)
  bool is_inited_ = false; // GUARDED_BY(session_mutex_)
};

//
// InferenceSession
//
InferenceSession::InferenceSession(const SessionOptions& session_options):
    impl_(new Impl(session_options)) {  
}

InferenceSession::~InferenceSession() = default;

Common::Status InferenceSession::Load(const std::string& model_uri) {
  return impl_->Load(model_uri);
}

Common::Status InferenceSession::Initialize() {
  return impl_->Initialize();
}

Common::Status InferenceSession::Run(const std::vector<MLValue>& feeds, std::vector<MLValue>* p_fetches) {
  return impl_->Run(feeds, p_fetches);
}

Common::Status InferenceSession::Run(const RunOptions& run_options,
                                     const std::vector<MLValue>& feeds,
                                     std::vector<MLValue>* p_fetches) {
  return impl_->Run(run_options, feeds, p_fetches);
}

Common::Status InferenceSession::SetProviderPreference(const std::vector<IExecutionProvider>& providers) {
  UNUSED_PARAMETER(providers);
  // TODO
  return Common::Status::OK();
}

int InferenceSession::GetCurrentNumRuns() {
  return impl_->GetCurrentNumRuns();
}

}
