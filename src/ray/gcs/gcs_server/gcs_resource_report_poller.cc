#include "ray/gcs/gcs_server/gcs_resource_report_poller.h"

namespace ray {
namespace gcs {

GcsResourceReportPoller::GcsResourceReportPoller(
    uint64_t max_concurrent_pulls,
    std::shared_ptr<GcsResourceManager> gcs_resource_manager,
    std::shared_ptr<rpc::NodeManagerClientPool> raylet_client_pool)
    :
      ticker_(polling_service_),
      max_concurrent_pulls_(max_concurrent_pulls),
      inflight_pulls_(0),
      gcs_resource_manager_(gcs_resource_manager),
      raylet_client_pool_(raylet_client_pool),
      poll_period_ms_(RayConfig::instance().gcs_resource_report_poll_period_ms()) {}

GcsResourceReportPoller::~GcsResourceReportPoller() { Stop(); }

void GcsResourceReportPoller::Start() {
  polling_thread_.reset(new std::thread{[this]() {
    SetThreadName("resource_report_poller");
    boost::asio::io_service::work work(polling_service_);

    polling_service_.run();
    RAY_LOG(DEBUG) << "GCSResourceReportPoller has stopped. This should only happen if "
                      "the cluster has stopped";
  }});
  ticker_.RunFnPeriodically([this]{ Tick();}, 100);
}

void GcsResourceReportPoller::Stop() {
  polling_service_.stop();
  if (polling_thread_->joinable()) {
    polling_thread_->join();
  }
}

void GcsResourceReportPoller::HandleNodeAdded(
    std::shared_ptr<rpc::GcsNodeInfo> node_info) {
  absl::MutexLock guard(&mutex_);
  const auto node_id = NodeID::FromBinary(node_info->node_id());

  RAY_CHECK(!nodes_.count(node_id)) << "Node with id: " << node_id << " was added twice!";

  auto state = std::make_shared<PullState>();
  state->node_id = node_id;

  state->address.set_raylet_id(node_info->node_id());
  state->address.set_ip_address(node_info->node_manager_address());
  state->address.set_port(node_info->node_manager_port());

  state->last_pull_time = -1;
  state->next_pull_time = absl::GetCurrentTimeNanos();

  nodes_.emplace(node_id, state);
  to_pull_queue_.push_front(state);

  polling_service_.post([this]() { TryPullResourceReport(); });
}

void GcsResourceReportPoller::HandleNodeRemoved(
    std::shared_ptr<rpc::GcsNodeInfo> node_info) {
  NodeID node_id = NodeID::FromBinary(node_info->node_id());

  {
    absl::MutexLock guard(&mutex_);
    nodes_.erase(node_id);
  }
}

void GcsResourceReportPoller::Tick() {
  TryPullResourceReport();
}

void GcsResourceReportPoller::TryPullResourceReport() {
  absl::MutexLock guard(&mutex_);

  int64_t cur_time = absl::GetCurrentTimeNanos();

  while (inflight_pulls_ < max_concurrent_pulls_ && !to_pull_queue_.empty()) {
    auto &to_pull = to_pull_queue_.front();
    if (cur_time > to_pull->next_pull_time) {
      break;
    }

    to_pull_queue_.pop_front();

    if (!nodes_.count(to_pull->node_id)) {
      RAY_LOG(DEBUG)
          << "Update finished, but node was already removed from the cluster. Ignoring.";
      continue;
    }

    PullResourceReport(to_pull);
  }
}

void GcsResourceReportPoller::PullResourceReport(
    const std::shared_ptr<PullState> &state) {
  inflight_pulls_++;
  auto raylet_client = raylet_client_pool_->GetOrConnectByAddress(state->address);
  raylet_client->RequestResourceReport(
      [this, state](const Status &status, const rpc::RequestResourceReportReply &reply) {
        if (status.ok()) {
          // TODO (Alex): This callback is always posted onto the main thread. Since most
          // of the work is in the callback we should move this callback's execution to
          // the polling thread. We will need to implement locking once we switch threads.
          gcs_resource_manager_->UpdateFromResourceReport(reply.resources());
          polling_service_.post([&] { NodeResourceReportReceived(state); });
        } else {
          RAY_LOG(INFO) << "Couldn't get resource request from raylet " << state->node_id
                        << ": " << status.ToString();
        }
      });
}

void GcsResourceReportPoller::NodeResourceReportReceived(
    const std::shared_ptr<PullState> &state) {
  absl::MutexLock guard(&mutex_);
  inflight_pulls_--;
  if (!nodes_.count(state->node_id)) {
    RAY_LOG(DEBUG)
        << "Update finished, but node was already removed from the cluster. Ignoring.";
    return;
  }

  state->next_pull_time = absl::GetCurrentTimeNanos() + poll_period_ms_;
  to_pull_queue_.push_back(state);
}

}  // namespace gcs
}  // namespace ray
