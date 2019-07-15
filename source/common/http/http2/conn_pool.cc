#include "common/http/http2/conn_pool.h"

#include <cstdint>
#include <memory>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/upstream/upstream.h"

#include "common/http/http2/codec_impl.h"
#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

namespace Envoy {
namespace Http {
namespace Http2 {

ConnPoolImpl::ConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
                           Upstream::ResourcePriority priority,
                           const Network::ConnectionSocket::OptionsSharedPtr& options)
    : ConnPoolImplBase(std::move(host), std::move(priority)), dispatcher_(dispatcher),
      socket_options_(options) {}

ConnPoolImpl::~ConnPoolImpl() {
  while (!ready_clients_.empty()) {
    ready_clients_.front()->codec_client_->close();
  }

  while (!busy_clients_.empty()) {
    busy_clients_.front()->codec_client_->close();
  }

  while (!overflow_clients_.empty()) {
    overflow_clients_.front()->codec_client_->close();
  }

  while (!drain_clients_.empty()) {
    drain_clients_.front()->codec_client_->close();
  }

  /*
  if (draining_client_) {
    draining_client_->client_->close();
  }

  */

  // Make sure all clients are destroyed before we are destroyed.
  dispatcher_.clearDeferredDeleteList();
}

void ConnPoolImpl::ConnPoolImpl::drainConnections() {
  while (!ready_clients_.empty()) {
    ready_clients_.front()->codec_client_->close();
  }

  while (!drain_clients_.empty()) {
    drain_clients_.front()->codec_client_->close();
  }

  for (const auto& client : overflow_clients_) {
    client->remaining_requests_ = 1;
  }

  for (const auto& client : busy_clients_) {
    client->remaining_requests_ = 1;
  }

  /*
  if (primary_client_ != nullptr) {
    movePrimaryClientToDraining();
  }
  */
}

void ConnPoolImpl::addDrainedCallback(DrainedCb cb) {
  drained_callbacks_.push_back(cb);
  checkForDrained();
}

bool ConnPoolImpl::hasActiveConnections() const {
  return !pending_requests_.empty() || !busy_clients_.empty() || !overflow_clients_.empty();
/*
  if (primary_client_ && primary_client_->client_->numActiveRequests() > 0) {
    return true;
  }

  if (draining_client_ && draining_client_->client_->numActiveRequests() > 0) {
    return true;
  }

  return !pending_requests_.empty();
  */
}

void ConnPoolImpl::checkForDrained() {
  if (!drained_callbacks_.empty() && pending_requests_.empty() && busy_clients_.empty()) {
    while (!drain_clients_.empty()) {
      ready_clients_.front()->codec_client_->close();
    }

    while (!overflow_clients_.empty()) {
      ready_clients_.front()->codec_client_->close();
    }

    while (!ready_clients_.empty()) {
      ready_clients_.front()->codec_client_->close();
    }

    for (const DrainedCb& cb : drained_callbacks_) {
      cb();
    }
  }

/*
  if (drained_callbacks_.empty()) {
    return;
  }

  bool drained = true;
  if (primary_client_) {
    if (primary_client_->client_->numActiveRequests() == 0) {
      primary_client_->client_->close();
      ASSERT(!primary_client_);
    } else {
      drained = false;
    }
  }

  ASSERT(!draining_client_ || (draining_client_->client_->numActiveRequests() > 0));
  if (draining_client_ && draining_client_->client_->numActiveRequests() > 0) {
    drained = false;
  }

  if (drained) {
    ENVOY_LOG(debug, "invoking drained callbacks");
    for (const DrainedCb& cb : drained_callbacks_) {
      cb();
    }
  }
  */
}

/*
void ConnPoolImpl::newClientStream(Http::StreamDecoder& response_decoder,
                                   ConnectionPool::Callbacks& callbacks) {
  if (!host_->cluster().resourceManager(priority_).requests().canCreate()) {
    ENVOY_LOG(debug, "max requests overflow");
      ENVOY_LOG(info, "max requests overflow");
      std::cout << this << ": max requests overflow" << std::endl;
    callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
                            nullptr);
    host_->cluster().stats().upstream_rq_pending_overflow_.inc();
  } else {
    ENVOY_CONN_LOG(debug, "creating stream", *primary_client_->client_);
    primary_client_->total_streams_++;
    std::cout << "total streams for client:  " << primary_client_.get() << " : " << primary_client_->total_streams_ << std::endl;
    host_->stats().rq_total_.inc();
    host_->stats().rq_active_.inc();
    host_->cluster().stats().upstream_rq_total_.inc();
    host_->cluster().stats().upstream_rq_active_.inc();
    host_->cluster().resourceManager(priority_).requests().inc();
    callbacks.onPoolReady(primary_client_->client_->newStream(response_decoder),
                          primary_client_->real_host_description_);
  }
}
ConnectionPool::Cancellable* ConnPoolImpl::newStream(Http::StreamDecoder& response_decoder,
                                                     ConnectionPool::Callbacks& callbacks) {
  ASSERT(drained_callbacks_.empty());

  // First see if we need to handle max streams rollover.
  uint64_t max_streams = host_->cluster().maxRequestsPerConnection();
  std::cout << "max requests per connection: " << max_streams << std::endl;
  if (max_streams == 0) {
    max_streams = maxTotalStreams();
  }

  if (primary_client_)
  {
    std::cout << "primary_client_->total_streams_: " << primary_client_->total_streams_ << ", max_streams: " << max_streams << std::endl;

  }

  if (primary_client_ && primary_client_->total_streams_ >= max_streams) {
    movePrimaryClientToDraining();
  }

  if (!primary_client_) {
    primary_client_ = std::make_unique<ActiveClient>(*this);
    std::cout << std::this_thread::get_id() << ": " << this << ": Created new primary client: " << primary_client_.get() << std::endl;
  }

  // If the primary client is not connected yet, queue up the request.
  if (!primary_client_->upstream_ready_) {
    // If we're not allowed to enqueue more requests, fail fast.
    if (!host_->cluster().resourceManager(priority_).pendingRequests().canCreate()) {
      ENVOY_LOG(debug, "max pending requests overflow");
      callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
                              nullptr);
      host_->cluster().stats().upstream_rq_pending_overflow_.inc();
      return nullptr;
    }

    std::cout << std::this_thread::get_id() << ": " << this << ":  queueing request" << std::endl;
    return newPendingRequest(response_decoder, callbacks);
  }

  // We already have an active client that's connected to upstream, so attempt to establish a
  // new stream.
  newClientStream(response_decoder, callbacks);
  return nullptr;
}
*/

void ConnPoolImpl::attachRequestToClient(ActiveClient& client, StreamDecoder& response_decoder,
                                         ConnectionPool::Callbacks& callbacks) {
  host_->stats().rq_total_.inc();
  host_->stats().rq_active_.inc();
  host_->cluster().stats().upstream_rq_total_.inc();
  host_->cluster().stats().upstream_rq_active_.inc();
  host_->cluster().resourceManager(priority_).requests().inc();
  callbacks.onPoolReady(client_->client_->newStream(response_decoder),
                        primary_client_->real_host_description_);
}

void ConnPoolImpl::createNewConnection() {
  ENVOY_LOG(debug, "creating a new connection");
  ActiveClientPtr client(new ActiveClient(*this));
  client->moveIntoList(std::move(client), busy_clients_);
}

ConnectionPool::Cancellable* ConnPoolImpl::newStream(Http::StreamDecoder& response_decoder,
                                                     ConnectionPool::Callbacks& callbacks) {
  if (!ready_list_.empty()) {
    auto client = ready_list_.front();
    attachRequestToClient(*client);
    const auto state = host_->cluster().connectionPolicy().onNewStream(*client);
    switch (state) {
      case ConnectionPolicy::State::READY:
        client->moveBetweenLists(ready_clients_, busy_clients_);
        break;
      case ConnectionPolicy::State::OVERFLOW:
        client->moveBetweenLists(ready_clients_, overflow_clients_);
        break;
      case ConnectionPolicy::State::DRAIN:
        client->moveBetweenLists(ready_clients_, drain_clients_);
        break;
      default:
        DCHECK(false);
    }

    client->state_ = state;

    ready_list_.pop_front();
    return nullptr;
  }

  if (!host_->cluster().resourceManager(priority_).pendingRequests().canCreate()) {
    ENVOY_LOG(debug, "max pending requests overflow");
    callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
                            nullptr);
    host_->cluster().stats().upstream_rq_pending_overflow_.inc();
    return nullptr;
  }

  // If we have no connections at all, make one no matter what so we don't starve.
  if ((ready_clients_.empty() && busy_clients_.empty())) {
    createNewConnection();
  }

  std::cout << std::this_thread::get_id() << ": " << this << ":  queueing request" << std::endl;
  return newPendingRequest(response_decoder, callbacks);
}

void ConnPoolImpl::onConnectionEvent(ActiveClient& client, Network::ConnectionEvent event) {
    std::cout << this << " : Got event" << std::endl;
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    ENVOY_CONN_LOG(debug, "client disconnected", *client.client_);

    std::cout << this << " : Got close event" << std::endl;
    Envoy::Upstream::reportUpstreamCxDestroy(host_, event);
    if (client.closed_with_active_rq_) {
      Envoy::Upstream::reportUpstreamCxDestroyActiveRequest(host_, event);
    }

    if (client.connect_timer_) {
      host_->cluster().stats().upstream_cx_connect_fail_.inc();
      host_->stats().cx_connect_fail_.inc();

      // Raw connect failures should never happen under normal circumstances. If we have an upstream
      // that is behaving badly, requests can get stuck here in the pending state. If we see a
      // connect failure, we purge all pending requests so that calling code can determine what to
      // do with the request.
      // NOTE: We move the existing pending requests to a temporary list. This is done so that
      //       if retry logic submits a new request to the pool, we don't fail it inline.
      purgePendingRequests(client.real_host_description_,
                           client.client_->connectionFailureReason());
    }

    if (&client == primary_client_.get()) {
      ENVOY_CONN_LOG(debug, "destroying primary client", *client.client_);
      dispatcher_.deferredDelete(std::move(primary_client_));
    } else {
      ENVOY_CONN_LOG(debug, "destroying draining client", *client.client_);
      dispatcher_.deferredDelete(std::move(draining_client_));
    }

    if (client.closed_with_active_rq_) {
      checkForDrained();
    }
  }

  if (event == Network::ConnectionEvent::Connected) {
    std::cout << this << " : Got connected event" << std::endl;
    conn_connect_ms_->complete();

    client.upstream_ready_ = true;
    onUpstreamReady(client);
  }

  if (client.connect_timer_) {
    client.connect_timer_->disableTimer();
    client.connect_timer_.reset();
  }
}

void ConnPoolImpl::movePrimaryClientToDraining() {
  ENVOY_CONN_LOG(debug, "moving primary to draining", *primary_client_->client_);
  if (draining_client_) {
    // This should pretty much never happen, but is possible if we start draining and then get
    // a goaway for example. In this case just kill the current draining connection. It's not
    // worth keeping a list.
    draining_client_->client_->close();
  }

  ASSERT(!draining_client_);
  if (primary_client_->client_->numActiveRequests() == 0) {
    // If we are making a new connection and the primary does not have any active requests just
    // close it now.
    primary_client_->client_->close();
  } else {
    draining_client_ = std::move(primary_client_);
  }

  ASSERT(!primary_client_);
}

void ConnPoolImpl::onConnectTimeout(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "connect timeout", *client.client_);
  host_->cluster().stats().upstream_cx_connect_timeout_.inc();
  client.client_->close();
}

void ConnPoolImpl::onGoAway(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "remote goaway", *client.client_);
  host_->cluster().stats().upstream_cx_close_notify_.inc();
  if (&client == primary_client_.get()) {
    movePrimaryClientToDraining();
  }
}

void ConnPoolImpl::onStreamDestroy(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "destroying stream: {} remaining", *client.client_,
                 client.client_->numActiveRequests());
  host_->stats().rq_active_.dec();
  host_->cluster().stats().upstream_rq_active_.dec();
  host_->cluster().resourceManager(priority_).requests().dec();
  if (&client == draining_client_.get() && client.client_->numActiveRequests() == 0) {
    // Close out the draining client if we no long have active requests.
    client.client_->close();
  }

  // If we are destroying this stream because of a disconnect, do not check for drain here. We will
  // wait until the connection has been fully drained of streams and then check in the connection
  // event callback.
  if (!client.closed_with_active_rq_) {
    checkForDrained();
  }
}

void ConnPoolImpl::onStreamReset(ActiveClient& client, Http::StreamResetReason reason) {
  if (reason == StreamResetReason::ConnectionTermination ||
      reason == StreamResetReason::ConnectionFailure) {
    host_->cluster().stats().upstream_rq_pending_failure_eject_.inc();
    client.closed_with_active_rq_ = true;
  } else if (reason == StreamResetReason::LocalReset) {
    host_->cluster().stats().upstream_rq_tx_reset_.inc();
  } else if (reason == StreamResetReason::RemoteReset) {
    host_->cluster().stats().upstream_rq_rx_reset_.inc();
  }

  //  state=  policy.onStreamReset(clien)
  //  switch(state):
  //  case overflow:
  //    
}

/*
void ConnPoolImpl::onUpstreamReady() {
  // Establishes new codec streams for each pending request.
  std::cout << "Pending requests size: " << pending_requests_.size() << std::endl;
  while (!pending_requests_.empty()) {
    std::cout << std::this_thread::get_id() << ": " << this <<  ": Processing Pending request: " << pending_requests_.back().get() << std::endl;
    newClientStream(pending_requests_.back()->decoder_, pending_requests_.back()->callbacks_);
    pending_requests_.pop_back();
  }
}
 */
 
void ConnPoolImpl::onUpstreamReady(ActiveClient& client) {
  if (pending_requests_.empty()) {
    // There is nothing to service or delayed processing is requested, so just move the connection
    // into the ready list.
    ENVOY_CONN_LOG(debug, "moving to ready", *client.client_);
    client.moveBetweenLists(busy_clients_, ready_clients_);
  } else {
    // There is work to do immediately so bind a request to the client and move it to the busy list.
    // Pending requests are pushed onto the front, so pull from the back.
    ENVOY_CONN_LOG(debug, "attaching to next request", *client.client_);
    attachRequestToClient(client, pending_requests_.back()->decoder_,
                          pending_requests_.back()->callbacks_);
    pending_requests_.pop_back();
  }

  checkForDrained();
}

ConnPoolImpl::ActiveClient::ActiveClient(ConnPoolImpl& parent)
    : parent_(parent),
      connect_timer_(parent_.dispatcher_.createTimer([this]() -> void { onConnectTimeout(); })) {
  parent_.conn_connect_ms_ = std::make_unique<Stats::Timespan>(
      parent_.host_->cluster().stats().upstream_cx_connect_ms_, parent_.dispatcher_.timeSource());
  Upstream::Host::CreateConnectionData data =
      parent_.host_->createConnection(parent_.dispatcher_, parent_.socket_options_, nullptr);
  real_host_description_ = data.host_description_;
  client_ = parent_.createCodecClient(data);
  client_->addConnectionCallbacks(*this);
  client_->setCodecClientCallbacks(*this);
  client_->setCodecConnectionCallbacks(*this);
  connect_timer_->enableTimer(parent_.host_->cluster().connectTimeout());

  parent_.host_->stats().cx_total_.inc();
  parent_.host_->stats().cx_active_.inc();
  parent_.host_->cluster().stats().upstream_cx_total_.inc();
  parent_.host_->cluster().stats().upstream_cx_active_.inc();
  parent_.host_->cluster().stats().upstream_cx_http2_total_.inc();
  conn_length_ = std::make_unique<Stats::Timespan>(
      parent_.host_->cluster().stats().upstream_cx_length_ms_, parent_.dispatcher_.timeSource());

  client_->setConnectionStats({parent_.host_->cluster().stats().upstream_cx_rx_bytes_total_,
                               parent_.host_->cluster().stats().upstream_cx_rx_bytes_buffered_,
                               parent_.host_->cluster().stats().upstream_cx_tx_bytes_total_,
                               parent_.host_->cluster().stats().upstream_cx_tx_bytes_buffered_,
                               &parent_.host_->cluster().stats().bind_errors_, nullptr});
}

ConnPoolImpl::ActiveClient::~ActiveClient() {
  parent_.host_->stats().cx_active_.dec();
  parent_.host_->cluster().stats().upstream_cx_active_.dec();
  conn_length_->complete();
}

CodecClientPtr ProdConnPoolImpl::createCodecClient(Upstream::Host::CreateConnectionData& data) {
  CodecClientPtr codec{new CodecClientProd(CodecClient::Type::HTTP2, std::move(data.connection_),
                                           data.host_description_, dispatcher_)};
  return codec;
}

uint32_t ProdConnPoolImpl::maxTotalStreams() { return MAX_STREAMS; }

} // namespace Http2
} // namespace Http
} // namespace Envoy
