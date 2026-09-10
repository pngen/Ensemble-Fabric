#include "ensemble_fabric/client.hpp"

#include <utility>

#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {

EnsembleClient::~EnsembleClient() = default;
EnsembleClient::EnsembleClient(EnsembleClient&&) noexcept = default;
EnsembleClient& EnsembleClient::operator=(EnsembleClient&&) noexcept = default;

Outcome<EnsembleClient> EnsembleClient::connect(const Endpoint& endpoint, Limits limits) {
  Outcome<Socket> socket = connect_to(endpoint);
  if (!socket.ok()) {
    return fail<EnsembleClient>(socket.code(), socket.error().message);
  }
  EnsembleClient client;
  client.limits_ = limits;
  client.channel_ = std::make_unique<FrameChannel>(std::move(socket).value(), limits);

  HelloMessage hello;
  hello.worker = WorkerId(1);
  hello.worker_boot = generate_worker_boot_id();
  hello.coordinator_epoch = CoordinatorEpoch(0);
  hello.profile.protocol_version = protocol_version;
  hello.label = "controller";
  Encoder payload(limits, 256);
  encode(hello, payload);

  Frame frame;
  frame.type = MessageType::hello;
  frame.correlation = client.next_correlation();
  frame.sequence = Sequence(1);
  frame.worker_boot = hello.worker_boot;
  frame.payload = payload.take();
  const Status sent = client.channel_->send(frame);
  if (!sent.ok()) {
    return fail<EnsembleClient>(sent.code(), sent.error().message);
  }

  Outcome<Frame> response = client.channel_->receive();
  if (!response.ok()) {
    return fail<EnsembleClient>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::hello_ack) {
    return fail<EnsembleClient>(EnsembleError::protocol_error,
                                "expected a handshake acknowledgement, received " +
                                    std::string(to_string(response.value().type)));
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits);
  Outcome<CoordinatorAck> ack = decode_coordinator_ack(decoder);
  if (!ack.ok()) {
    return fail<EnsembleClient>(ack.code(), ack.error().message);
  }
  const Status exhausted = decoder.require_exhausted("handshake acknowledgement");
  if (!exhausted.ok()) {
    return fail<EnsembleClient>(exhausted.code(), exhausted.error().message);
  }
  if (!ack.value().accepted) {
    return fail<EnsembleClient>(static_cast<EnsembleError>(ack.value().status_code),
                                "the coordinator rejected the handshake: " + ack.value().detail);
  }
  client.coordinator_epoch_ = ack.value().coordinator_epoch;
  client.coordinator_boot_ = ack.value().coordinator_boot;
  return client;
}

RequestId EnsembleClient::next_correlation() noexcept {
  const auto allocated = sequences_.allocate();
  return RequestId(allocated.has_value() ? allocated.value().value() : 1u);
}

Outcome<Frame> EnsembleClient::request(MessageType type, std::vector<std::byte> payload) {
  if (!connected()) {
    return fail<Frame>(EnsembleError::transport_error, "the client is not connected");
  }
  const RequestId correlation = next_correlation();
  Frame frame;
  frame.type = type;
  frame.correlation = correlation;
  frame.epoch = coordinator_epoch_;
  frame.worker_boot = coordinator_boot_;
  frame.sequence = Sequence(correlation.value());
  frame.payload = std::move(payload);
  const Status sent = channel_->send(frame);
  if (!sent.ok()) {
    return fail<Frame>(sent.code(), sent.error().message);
  }
  for (;;) {
    Outcome<Frame> response = channel_->receive();
    if (!response.ok()) {
      return response;
    }
    if (response.value().correlation == correlation) {
      return response;
    }
    if (response.value().type == MessageType::error_response) {
      Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                                 response.value().payload.size()),
                      limits_);
      Outcome<ErrorResponseMessage> error = decode_error_response(decoder);
      if (error.ok() && error.value().correlation == correlation) {
        return fail<Frame>(error.value().code, error.value().detail);
      }
    }
    // Frames for other requests are not expected in a single-threaded control
    // client; a mismatch is a protocol error rather than something to skip.
    return fail<Frame>(EnsembleError::protocol_error,
                       "received a response for an unrelated correlation identity");
  }
}

Outcome<DefineEnsembleAck> EnsembleClient::define_ensemble(const EnsembleSpec& spec) {
  DefineEnsembleMessage message;
  message.encoded_spec = encode_spec(spec, limits_);
  Encoder payload(limits_, message.encoded_spec.size() + 64u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::define_ensemble, payload.take());
  if (!response.ok()) {
    return fail<DefineEnsembleAck>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::define_ensemble_ack) {
    return fail<DefineEnsembleAck>(EnsembleError::protocol_error,
                                   "unexpected response to a definition request");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<DefineEnsembleAck> ack = decode_define_ensemble_ack(decoder);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = decoder.require_exhausted("definition acknowledgement");
  if (!exhausted.ok()) {
    return fail<DefineEnsembleAck>(exhausted.code(), exhausted.error().message);
  }
  return ack;
}

Outcome<OpenExecutionAck> EnsembleClient::open_execution(EnsembleId ensemble,
                                                         EnsembleGeneration generation) {
  OpenExecutionMessage message;
  message.ensemble = ensemble;
  message.ensemble_generation = generation;
  Encoder payload(limits_, 64u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::open_execution, payload.take());
  if (!response.ok()) {
    return fail<OpenExecutionAck>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::open_execution_ack) {
    return fail<OpenExecutionAck>(EnsembleError::protocol_error,
                                  "unexpected response to an execution request");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<OpenExecutionAck> ack = decode_open_execution_ack(decoder);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = decoder.require_exhausted("execution acknowledgement");
  if (!exhausted.ok()) {
    return fail<OpenExecutionAck>(exhausted.code(), exhausted.error().message);
  }
  return ack;
}

Outcome<FinalizeResponseMessage> EnsembleClient::finalize(EnsembleId ensemble,
                                                          EnsembleGeneration generation) {
  FinalizeRequestMessage message;
  message.ensemble = ensemble;
  message.ensemble_generation = generation;
  Encoder payload(limits_, 64u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::finalize_request, payload.take());
  if (!response.ok()) {
    return fail<FinalizeResponseMessage>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::finalize_response) {
    return fail<FinalizeResponseMessage>(EnsembleError::protocol_error,
                                         "unexpected response to a finalize request");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<FinalizeResponseMessage> ack = decode_finalize_response(decoder);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = decoder.require_exhausted("finalize response");
  if (!exhausted.ok()) {
    return fail<FinalizeResponseMessage>(exhausted.code(), exhausted.error().message);
  }
  return ack;
}

Outcome<ResultResponseMessage> EnsembleClient::query_result(EnsembleId ensemble,
                                                            EnsembleGeneration generation) {
  QueryResultMessage message;
  message.ensemble = ensemble;
  message.ensemble_generation = generation;
  Encoder payload(limits_, 64u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::query_result, payload.take());
  if (!response.ok()) {
    return fail<ResultResponseMessage>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::result_response) {
    return fail<ResultResponseMessage>(EnsembleError::protocol_error,
                                       "unexpected response to a result query");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<ResultResponseMessage> ack = decode_result_response(decoder);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = decoder.require_exhausted("result response");
  if (!exhausted.ok()) {
    return fail<ResultResponseMessage>(exhausted.code(), exhausted.error().message);
  }
  return ack;
}

Outcome<InspectResponseMessage> EnsembleClient::inspect(EnsembleId ensemble) {
  InspectRequestMessage message;
  message.ensemble = ensemble;
  Encoder payload(limits_, 64u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::inspect_request, payload.take());
  if (!response.ok()) {
    return fail<InspectResponseMessage>(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::inspect_response) {
    return fail<InspectResponseMessage>(EnsembleError::protocol_error,
                                        "unexpected response to an inspection request");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<InspectResponseMessage> ack = decode_inspect_response(decoder);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = decoder.require_exhausted("inspection response");
  if (!exhausted.ok()) {
    return fail<InspectResponseMessage>(exhausted.code(), exhausted.error().message);
  }
  return ack;
}

Status EnsembleClient::cancel_ensemble(EnsembleId ensemble, EnsembleGeneration generation,
                                       std::string reason) {
  CancelEnsembleMessage message;
  message.ensemble = ensemble;
  message.ensemble_generation = generation;
  message.reason = detail::sanitize_text(reason, limits_.max_metadata_bytes);
  Encoder payload(limits_, 128u);
  encode(message, payload);
  Outcome<Frame> response = request(MessageType::cancel_ensemble, payload.take());
  if (!response.ok()) {
    return fail(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::status_response) {
    return fail(EnsembleError::protocol_error, "unexpected response to a cancellation request");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  limits_);
  Outcome<StatusResponseMessage> ack = decode_status_response(decoder);
  if (!ack.ok()) {
    return fail(ack.code(), ack.error().message);
  }
  const Status exhausted = decoder.require_exhausted("status response");
  if (!exhausted.ok()) {
    return exhausted;
  }
  if (ack.value().code != EnsembleError::ok) {
    return fail(ack.value().code, ack.value().detail);
  }
  return ok_status();
}

void EnsembleClient::close() noexcept {
  if (channel_ != nullptr) {
    channel_->close();
    channel_.reset();
  }
}

}  // namespace ensemble_fabric
