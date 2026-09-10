#include <thread>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

#include "ensemble_fabric/transport.hpp"

using namespace ensemble_fabric;

EF_TEST(a_framed_message_round_trips_over_loopback_tcp) {
  std::uint16_t port = 0;
  Endpoint bind;
  bind.host = "127.0.0.1";
  bind.port = 0;
  Outcome<Socket> listener = listen_on(bind, port);
  EF_REQUIRE_OK(listener);
  if (!listener.ok()) {
    return;
  }
  EF_CHECK(port != 0u);

  std::thread server([&listener]() {
    Outcome<Socket> accepted = accept_one(listener.value());
    if (!accepted.ok()) {
      return;
    }
    FrameChannel channel(std::move(accepted).value());
    Outcome<Frame> frame = channel.receive();
    if (!frame.ok()) {
      return;
    }
    Frame reply;
    reply.type = MessageType::status_response;
    reply.correlation = frame.value().correlation;
    reply.payload = frame.value().payload;
    (void)channel.send(reply);
  });

  Endpoint target;
  target.host = "127.0.0.1";
  target.port = port;
  Outcome<Socket> client = connect_to(target);
  EF_REQUIRE_OK(client);
  if (client.ok()) {
    FrameChannel channel(std::move(client).value());
    Frame request;
    request.type = MessageType::heartbeat;
    request.correlation = RequestId(77);
    request.payload = {std::byte{0x01}, std::byte{0x02}};
    EF_REQUIRE_OK(channel.send(request));
    Outcome<Frame> reply = channel.receive();
    EF_REQUIRE_OK(reply);
    if (reply.ok()) {
      EF_CHECK(reply.value().type == MessageType::status_response);
      EF_CHECK_EQ(reply.value().correlation.value(), 77u);
      EF_CHECK_EQ(reply.value().payload.size(), 2u);
    }
    channel.close();
  }
  listener.value().close();
  server.join();
}

EF_TEST(concurrent_writers_produce_intact_frames) {
  std::uint16_t port = 0;
  Endpoint bind;
  bind.host = "127.0.0.1";
  bind.port = 0;
  Outcome<Socket> listener = listen_on(bind, port);
  EF_REQUIRE_OK(listener);
  if (!listener.ok()) {
    return;
  }

  constexpr int kFrames = 64;
  std::thread server([&listener]() {
    Outcome<Socket> accepted = accept_one(listener.value());
    if (!accepted.ok()) {
      return;
    }
    FrameChannel channel(std::move(accepted).value());
    for (int index = 0; index < kFrames; ++index) {
      Outcome<Frame> frame = channel.receive();
      if (!frame.ok()) {
        return;
      }
    }
  });

  Endpoint target;
  target.host = "127.0.0.1";
  target.port = port;
  Outcome<Socket> client = connect_to(target);
  EF_REQUIRE_OK(client);
  if (client.ok()) {
    auto channel = std::make_shared<FrameChannel>(std::move(client).value());
    std::vector<std::thread> senders;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
      senders.emplace_back([channel, thread_index]() {
        for (int index = 0; index < kFrames / 4; ++index) {
          Frame frame;
          frame.type = MessageType::heartbeat;
          frame.correlation = RequestId(static_cast<std::uint64_t>(thread_index * 100 + index + 1));
          frame.payload.assign(static_cast<std::size_t>(thread_index) + 1u, std::byte{0x7A});
          (void)channel->send(frame);
        }
      });
    }
    for (std::thread& sender : senders) {
      sender.join();
    }
    channel->close();
  }
  listener.value().close();
  server.join();
}

EF_TEST(a_closed_channel_reports_a_typed_failure) {
  std::uint16_t port = 0;
  Endpoint bind;
  bind.host = "127.0.0.1";
  bind.port = 0;
  Outcome<Socket> listener = listen_on(bind, port);
  EF_REQUIRE_OK(listener);
  if (!listener.ok()) {
    return;
  }
  std::thread server([&listener]() {
    Outcome<Socket> accepted = accept_one(listener.value());
    if (accepted.ok()) {
      accepted.value().close();
    }
  });
  Endpoint target;
  target.host = "127.0.0.1";
  target.port = port;
  Outcome<Socket> client = connect_to(target);
  EF_REQUIRE_OK(client);
  if (client.ok()) {
    FrameChannel channel(std::move(client).value());
    Outcome<Frame> frame = channel.receive();
    EF_CHECK(!frame.ok());
    EF_CHECK(frame.code() == EnsembleError::transport_error);
  }
  listener.value().close();
  server.join();
}

EF_TEST(connecting_to_a_closed_port_fails_cleanly) {
  Endpoint target;
  target.host = "127.0.0.1";
  target.port = 1;
  Outcome<Socket> client = connect_to(target);
  EF_CHECK(!client.ok());
  EF_CHECK(client.code() == EnsembleError::transport_error);
}

EF_TEST_MAIN
