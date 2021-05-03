// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "blimp/common/create_blimp_message.h"
#include "blimp/common/proto/blimp_message.pb.h"
#include "blimp/common/proto/protocol_control.pb.h"
#include "blimp/net/blimp_connection.h"
#include "blimp/net/tcp_client_transport.h"
#include "blimp/net/tcp_engine_transport.h"
#include "blimp/net/test_common.h"
#include "net/base/address_list.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::SaveArg;

namespace blimp {

namespace {

// Integration test for TCPEngineTransport and TCPClientTransport.
class TCPTransportTest : public testing::Test {
 protected:
  TCPTransportTest() {
    net::IPEndPoint local_address;
    ParseAddress("127.0.0.1", 0, &local_address);
    engine_.reset(new TCPEngineTransport(local_address, nullptr));
  }

  net::AddressList GetLocalAddressList() const {
    net::IPEndPoint local_address;
    engine_->GetLocalAddressForTesting(&local_address);
    return net::AddressList(local_address);
  }

  void ParseAddress(const std::string& ip_str,
                    uint16_t port,
                    net::IPEndPoint* address) {
    net::IPAddressNumber ip_number;
    bool rv = net::ParseIPLiteralToNumber(ip_str, &ip_number);
    if (!rv)
      return;
    *address = net::IPEndPoint(ip_number, port);
  }

  base::MessageLoopForIO message_loop_;
  scoped_ptr<TCPEngineTransport> engine_;
};

TEST_F(TCPTransportTest, Connect) {
  net::TestCompletionCallback accept_callback;
  engine_->Connect(accept_callback.callback());

  net::TestCompletionCallback connect_callback;
  TCPClientTransport client(GetLocalAddressList(), nullptr);
  client.Connect(connect_callback.callback());

  EXPECT_EQ(net::OK, connect_callback.WaitForResult());
  EXPECT_EQ(net::OK, accept_callback.WaitForResult());
  EXPECT_TRUE(engine_->TakeConnection() != nullptr);
}

TEST_F(TCPTransportTest, TwoClientConnections) {
  net::TestCompletionCallback accept_callback1;
  engine_->Connect(accept_callback1.callback());

  net::TestCompletionCallback connect_callback1;
  TCPClientTransport client1(GetLocalAddressList(), nullptr);
  client1.Connect(connect_callback1.callback());

  net::TestCompletionCallback connect_callback2;
  TCPClientTransport client2(GetLocalAddressList(), nullptr);
  client2.Connect(connect_callback2.callback());

  EXPECT_EQ(net::OK, connect_callback1.WaitForResult());
  EXPECT_EQ(net::OK, accept_callback1.WaitForResult());
  EXPECT_TRUE(engine_->TakeConnection() != nullptr);

  net::TestCompletionCallback accept_callback2;
  engine_->Connect(accept_callback2.callback());
  EXPECT_EQ(net::OK, connect_callback2.WaitForResult());
  EXPECT_EQ(net::OK, accept_callback2.WaitForResult());
  EXPECT_TRUE(engine_->TakeConnection() != nullptr);
}

TEST_F(TCPTransportTest, ExchangeMessages) {
  // Start the Engine transport and connect a client to it.
  net::TestCompletionCallback accept_callback;
  engine_->Connect(accept_callback.callback());
  net::TestCompletionCallback client_connect_callback;
  TCPClientTransport client(GetLocalAddressList(), nullptr /* NetLog */);
  client.Connect(client_connect_callback.callback());
  EXPECT_EQ(net::OK, client_connect_callback.WaitForResult());
  EXPECT_EQ(net::OK, accept_callback.WaitForResult());

  // Expect the engine to get two messages from the client, and the client to
  // get one from the engine.
  MockBlimpMessageProcessor engine_incoming_processor;
  MockBlimpMessageProcessor client_incoming_processor;
  net::CompletionCallback engine_process_message_cb;
  scoped_ptr<BlimpMessage> client_message1 =
      CreateStartConnectionMessage("", 0);
  scoped_ptr<BlimpMessage> client_message2 = CreateCheckpointAckMessage(5);
  scoped_ptr<BlimpMessage> engine_message = CreateCheckpointAckMessage(10);
  EXPECT_CALL(engine_incoming_processor,
              MockableProcessMessage(EqualsProto(*client_message1), _))
      .WillOnce(SaveArg<1>(&engine_process_message_cb));
  EXPECT_CALL(engine_incoming_processor,
              MockableProcessMessage(EqualsProto(*client_message2), _))
      .Times(1);
  EXPECT_CALL(client_incoming_processor,
              MockableProcessMessage(EqualsProto(*engine_message), _))
      .Times(1);

  // Attach the ends of the connection to our mock message-processors.
  scoped_ptr<BlimpConnection> engine_connnection = engine_->TakeConnection();
  scoped_ptr<BlimpConnection> client_connnection = client.TakeConnection();
  engine_connnection->SetIncomingMessageProcessor(&engine_incoming_processor);
  client_connnection->SetIncomingMessageProcessor(&client_incoming_processor);

  // Client sends the first message.
  net::TestCompletionCallback client_send_callback1;
  client_connnection->GetOutgoingMessageProcessor()->ProcessMessage(
      std::move(client_message1), client_send_callback1.callback());
  EXPECT_EQ(net::OK, client_send_callback1.WaitForResult());

  // Engine finishes processing the client message.
  EXPECT_FALSE(engine_process_message_cb.is_null());
  engine_process_message_cb.Run(net::OK);

  // Engine sends one message.
  net::TestCompletionCallback engine_send_callback;
  engine_connnection->GetOutgoingMessageProcessor()->ProcessMessage(
      std::move(engine_message), engine_send_callback.callback());
  EXPECT_EQ(net::OK, engine_send_callback.WaitForResult());

  // Client sends the second message.
  net::TestCompletionCallback client_send_callback2;
  client_connnection->GetOutgoingMessageProcessor()->ProcessMessage(
      std::move(client_message2), client_send_callback2.callback());
  EXPECT_EQ(net::OK, client_send_callback2.WaitForResult());
}

}  // namespace

}  // namespace blimp
