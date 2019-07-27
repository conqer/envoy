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

using Upstream::ConnectionRequestPolicy;

ConnPoolImpl::ConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
                           Upstream::ResourcePriority priority,
                           const Network::ConnectionSocket::OptionsSharedPtr& options)
    : ConnPoolImplBase(std::move(host), std::move(priority)), dispatcher_(dispatcher),
      socket_options_(options) {
  ENVOY_LOG(debug, "#########################################");
        std::cout << "CReating http2 conn pool" << std::endl;
      }

void ConnPoolImpl::applyToEachClient(std::list<ActiveClientPtr>& client_list,
                              const std::function<void(const ActiveClientPtr&)>& fn) {
  for_each(client_list.begin(), client_list.end(), fn);
}

ConnPoolImpl::~ConnPoolImpl() {
  applyToEachClient(ready_clients_,
                    [](const ActiveClientPtr& client) { client->client_->close(); });

  applyToEachClient(busy_clients_,
                    [](const ActiveClientPtr& client) { client->client_->close(); });

  applyToEachClient(overflow_clients_,
                    [](const ActiveClientPtr& client) { client->client_->close(); });

  applyToEachClient(drain_clients_,
                    [](const ActiveClientPtr& client) { client->client_->close(); });

  // Make sure all clients are destroyed before we are destroyed.
  dispatcher_.clearDeferredDeleteList();
}

void ConnPoolImpl::ConnPoolImpl::drainConnections() {
  applyToEachClient(ready_clients_,
                    [](const ActiveClientPtr& client) { client->client_->close(); });

  applyToEachClient(drain_clients_, [](const ActiveClientPtr& client) {
    if (client->client_->numActiveRequests() == 0) {
      client->client_->close();
    }
  });

  applyToEachClient(overflow_clients_, [this](const ActiveClientPtr& client) {
    client->state_ = ConnectionRequestPolicy::State::DRAIN;
    client->moveBetweenLists(overflow_clients_, drain_clients_);
  });

  applyToEachClient(busy_clients_, [this](const ActiveClientPtr& client) {
    client->state_ = ConnectionRequestPolicy::State::DRAIN;
    client->moveBetweenLists(busy_clients_, drain_clients_);
  });

  checkForDrained();
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

Upstream::ResourceManager& ConnPoolImpl::ActiveClient::resourceManager() const {
  return parent_.host()->cluster().resourceManager(parent_.resourcePriority());
}

bool ConnPoolImpl::hasActiveConnections() const {
  return !pending_requests_.empty() || !busy_clients_.empty() || !overflow_clients_.empty() ||
         !drain_clients_.empty();
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
  for (auto it = drain_clients_.begin(); it != drain_clients_.end();) {
    auto splice_it = it++;
    auto &client = *splice_it;
    if (client->client_ && client->client_->numActiveRequests() == 0) {
      ENVOY_CONN_LOG(debug, "adding to close list", *client->client_);
      to_close_clients_.splice(to_close_clients_.cend(), drain_clients_, splice_it);
    }
  }

  while (!to_close_clients_.empty()) {
    auto& client = to_close_clients_.front();
    if (client->client_->numActiveRequests() == 0) {
      ENVOY_CONN_LOG(debug, "closing from drained list", *client->client_);
      client->client_->close();
    }
  }

  if (!drained_callbacks_.empty() && pending_requests_.empty() && busy_clients_.empty() &&
      overflow_clients_.empty()) {
    ENVOY_LOG(debug, "draining ready clients");

    while (!ready_clients_.empty()) {
      ready_clients_.front()->client_->close();
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

void ConnPoolImpl::attachRequestToClient(ConnPoolImpl::ActiveClient& client, StreamDecoder& response_decoder,
                                         ConnectionPool::Callbacks& callbacks) {
  ENVOY_LOG(debug, "attaching request to connection");
	client.total_streams_++;
  host_->stats().rq_total_.inc();
  host_->stats().rq_active_.inc();
  host_->cluster().stats().upstream_rq_total_.inc();
  host_->cluster().stats().upstream_rq_active_.inc();
  host_->cluster().resourceManager(priority_).requests().inc();
  callbacks.onPoolReady(client.client_->newStream(response_decoder),
                        client.real_host_description_);
}

void ConnPoolImpl::createNewConnection() {
  ENVOY_LOG(debug, "creating a new connection");
  ActiveClientPtr client(new ActiveClient(*this));
  client->state_ = ConnectionRequestPolicy::State::INIT;
  ENVOY_CONN_LOG(debug, "Moving new connection to connecting_clients list", *client->client_);
  client->moveIntoList(std::move(client), connecting_clients_);
}

ConnectionPool::Cancellable* ConnPoolImpl::newStream(Http::StreamDecoder& response_decoder,
                                                     ConnectionPool::Callbacks& callbacks) {
  ENVOY_LOG(debug, "new stream");
  std::list<ActiveClientPtr> *client_list = nullptr;

  if (!busy_clients_.empty()) {
    client_list = &busy_clients_;
  } else if (!ready_clients_.empty()) {
    client_list = &ready_clients_;
  }

  if (client_list) {
    auto &client = client_list->front();

    ENVOY_CONN_LOG(debug, "using existing connection", *client->client_);
    attachRequestToClient(*client, response_decoder, callbacks);

    const auto state = host_->cluster().connectionPolicy().onNewStream(*client);
    switch (state) {
      case ConnectionRequestPolicy::State::ACTIVE:
        ENVOY_CONN_LOG(debug, "moving to active list after attaching request", *client->client_);
        client->moveBetweenLists(*client_list, busy_clients_);
        break;
      case ConnectionRequestPolicy::State::OVERFLOW:
        ENVOY_CONN_LOG(debug, "moving to overflow list after attaching request", *client->client_);
        client->moveBetweenLists(*client_list, overflow_clients_);
        break;
      case ConnectionRequestPolicy::State::DRAIN:
        ENVOY_CONN_LOG(debug, "moving to drain list after attaching request", *client->client_);
        client->moveBetweenLists(*client_list, drain_clients_);
        break;
      default:
        ASSERT(false);
    }

    client->state_ = state;
/*
    if (ready_clients_.empty()) {
      ENVOY_LOG(debug, "creating new connection as no exiting ready or busy clients");
      createNewConnection();
    }
*/
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
    ENVOY_LOG(debug, "no ready or busy clients available for new stream");
    createNewConnection();
  }

  ENVOY_LOG(debug, "queueing request as no connection available");
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

    ActiveClientPtr removed;
    bool check_for_drained = true;

    if ((!client.connect_timer_) && (client.state_ != ConnectionRequestPolicy::State::READY)) {
      // if not in ready list
      switch(client.state_){
        case ConnectionRequestPolicy::State::OVERFLOW: {
          removed = client.removeFromList(overflow_clients_);
          break;
        }
        case ConnectionRequestPolicy::State::DRAIN: {
          ENVOY_CONN_LOG(debug, "client removed from drain list", *client.client_);
          removed = client.removeFromList(to_close_clients_);
          break;
        }
        default:
          ENVOY_CONN_LOG(debug, "client removed from busy list", *client.client_);
          removed = client.removeFromList(busy_clients_);
          break;
      }
    } else {
      // in ready list
      host_->cluster().stats().upstream_cx_connect_fail_.inc();
      host_->stats().cx_connect_fail_.inc();

      removed = client.removeFromList(ready_clients_);
      check_for_drained = false;

      // Raw connect failures should never happen under normal circumstances. If we have an upstream
      // that is behaving badly, requests can get stuck here in the pending state. If we see a
      // connect failure, we purge all pending requests so that calling code can determine what to
      // do with the request.
      purgePendingRequests(client.real_host_description_,
                           client.client_->connectionFailureReason());
    }

    dispatcher_.deferredDelete(std::move(removed));

    // If we have pending requests and we just lost a connection we should make a new one.
    if (pending_requests_.size() > (ready_clients_.size() + busy_clients_.size())) {
      createNewConnection();
    }

    if (check_for_drained) {
      checkForDrained();
    }
    /*
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
    */
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
/*
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
*/
void ConnPoolImpl::onConnectTimeout(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "connect timeout", *client.client_);
  host_->cluster().stats().upstream_cx_connect_timeout_.inc();
  if (client.state_ ==  ConnectionRequestPolicy::State::ACTIVE) {
    client.moveBetweenLists(busy_clients_, drain_clients_);
  }
  if (client.state_ ==  ConnectionRequestPolicy::State::INIT) {
    client.moveBetweenLists(connecting_clients_, drain_clients_);
  }

  client.state_ = ConnectionRequestPolicy::State::DRAIN;

  client.connect_timer_->disableTimer();
  client.connect_timer_.reset();

  checkForDrained();
}

void ConnPoolImpl::onGoAway(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "remote goaway", *client.client_);
  host_->cluster().stats().upstream_cx_close_notify_.inc();

  switch (client.state_) {
  case ConnectionRequestPolicy::State::OVERFLOW: {
    client.moveBetweenLists(overflow_clients_, drain_clients_);
    break;
  }
  case ConnectionRequestPolicy::State::ACTIVE: {
    client.moveBetweenLists(busy_clients_, drain_clients_);
    break;
  }
  default:
    ASSERT(client.state_ == ConnectionRequestPolicy::State::DRAIN);
    break;
  }

  client.state_ = ConnectionRequestPolicy::State::DRAIN;

  /*
  if (&client == primary_client_.get()) {
    movePrimaryClientToDraining();
  }
  */
}

void ConnPoolImpl::onStreamDestroy(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "destroying stream: {} remaining", *client.client_,
                 client.client_->numActiveRequests());
  host_->stats().rq_active_.dec();
  host_->cluster().stats().upstream_rq_active_.dec();
  host_->cluster().resourceManager(priority_).requests().dec();
	client.total_streams_--;

  bool check_drained = false;
  const auto state = host_->cluster().connectionPolicy().onStreamReset(client, client.state_);
  switch (state) {
    case ConnectionRequestPolicy::State::ACTIVE: {
      if (client.state_ == ConnectionRequestPolicy::State::OVERFLOW) {
        client.moveBetweenLists(overflow_clients_, busy_clients_);
      }
      if (client.state_ == ConnectionRequestPolicy::State::DRAIN) {
        client.moveBetweenLists(drain_clients_, busy_clients_);
      }

      break;
    }
    case ConnectionRequestPolicy::State::DRAIN: {
      if (client.state_ == ConnectionRequestPolicy::State::OVERFLOW) {
        ENVOY_CONN_LOG(debug, "moving to drain list", *client.client_);
        check_drained = true;
        client.moveBetweenLists(overflow_clients_, drain_clients_);
      }

      if (client.state_ == ConnectionRequestPolicy::State::ACTIVE) {
        ENVOY_CONN_LOG(debug, "moving to drain list", *client.client_);
        check_drained = true;
        client.moveBetweenLists(overflow_clients_, drain_clients_);
      }

      if (client.state_ == ConnectionRequestPolicy::State::DRAIN) {
        ENVOY_CONN_LOG(debug, "moving to drain list", *client.client_);
        check_drained = true;
      }
      break;
    }
    default:
      ASSERT(false);
      break;
  }

  if (check_drained) {
    checkForDrained();
  }

      // if policy.
      //    if (client.state_ == DRAIN) {
      //      if (client.numOfActiveRequests()  == 0) {
      //       checkForDrained();
      //       return;
      //      }
      //    }
      //
      //    if (client.state_ == OVERFLOW) {
      //      if (--client.numOfActiveRequests()  <  max) {
      //        move_to_ready();
      //        return;
      //      }
      //    }
      //
      // if (!client.closed_with_active_rq_) {
      //  checkForDrained();
      //  }
      //
      //
      // }
      //
      /*
      if (&client == draining_client_.get() && client.client_->numActiveRequests() == 0) {
        // Close out the draining client if we no long have active requests.
        client.client_->close();
      }

      // If we are destroying this stream because of a disconnect, do not check for drain here. We
      will
      // wait until the connection has been fully drained of streams and then check in the
      connection
      // event callback.
      if (!client.closed_with_active_rq_) {
        checkForDrained();
      }
      */
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
    if (client.state_ == ConnectionRequestPolicy::State::INIT) {
      // There is nothing to service or delayed processing is requested, so just move the connection
      // into the ready list.
      ENVOY_CONN_LOG(debug, "moving to ready", *client.client_);
      client.moveBetweenLists(connecting_clients_, ready_clients_);
      client.state_ = ConnectionRequestPolicy::State::READY;
    }
  } else {
    if (client.state_ == ConnectionRequestPolicy::State::INIT) {
      client.moveBetweenLists(connecting_clients_, busy_clients_);
      client.state_ = ConnectionRequestPolicy::State::ACTIVE;

      // There is work to do immediately so bind a request to the client and move it to the busy list.
      // Pending requests are pushed onto the front, so pull from the back.
      ENVOY_CONN_LOG(debug, "attaching to next request", *client.client_);
      attachRequestToClient(client, pending_requests_.back()->decoder_,
                            pending_requests_.back()->callbacks_);
      pending_requests_.pop_back();

      const auto state = host_->cluster().connectionPolicy().onNewStream(client);
      switch (state) {
        case ConnectionRequestPolicy::State::OVERFLOW:
          ENVOY_CONN_LOG(debug, "moving to overflow list after attaching request", *client.client_);
          client.moveBetweenLists(busy_clients_, overflow_clients_);
          break;
        case ConnectionRequestPolicy::State::DRAIN:
          ENVOY_CONN_LOG(debug, "moving to drain list after attaching request", *client.client_);
          client.moveBetweenLists(busy_clients_, drain_clients_);
          break;
        default:
          ASSERT(false);
      }

      client.state_ = state;

    }
  }

  if (ready_clients_.empty() && busy_clients_.empty() && connecting_clients_.empty()) {
    ENVOY_LOG(debug, "creating new connection as no exiting ready or busy clients");
    createNewConnection();
  }

  ENVOY_LOG(debug, "handled onUpsreamReady");
  //checkForDrained();
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
