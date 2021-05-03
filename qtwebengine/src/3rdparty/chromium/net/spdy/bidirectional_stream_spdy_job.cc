// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/bidirectional_stream_spdy_job.h"

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/http/bidirectional_stream_request_info.h"
#include "net/spdy/spdy_buffer.h"
#include "net/spdy/spdy_header_block.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_stream.h"

namespace net {

namespace {

// Time to wait in millisecond to notify |delegate_| of data received.
// Handing small chunks of data to the caller creates measurable overhead.
// So buffer data in short time-spans and send a single read notification.
const int kBufferTimeMs = 1;

}  // namespace

BidirectionalStreamSpdyJob::BidirectionalStreamSpdyJob(
    const base::WeakPtr<SpdySession>& spdy_session)
    : spdy_session_(spdy_session),
      request_info_(nullptr),
      delegate_(nullptr),
      negotiated_protocol_(kProtoUnknown),
      more_read_data_pending_(false),
      read_buffer_len_(0),
      stream_closed_(false),
      closed_stream_status_(ERR_FAILED),
      closed_stream_received_bytes_(0),
      closed_stream_sent_bytes_(0),
      weak_factory_(this) {}

BidirectionalStreamSpdyJob::~BidirectionalStreamSpdyJob() {
  if (stream_) {
    stream_->DetachDelegate();
    DCHECK(!stream_);
  }
}

void BidirectionalStreamSpdyJob::Start(
    const BidirectionalStreamRequestInfo* request_info,
    const BoundNetLog& net_log,
    BidirectionalStreamJob::Delegate* delegate,
    scoped_ptr<base::Timer> timer) {
  DCHECK(!stream_);
  DCHECK(timer);

  delegate_ = delegate;
  timer_ = std::move(timer);

  if (!spdy_session_) {
    delegate_->OnFailed(ERR_CONNECTION_CLOSED);
    return;
  }

  request_info_ = request_info;

  int rv = stream_request_.StartRequest(
      SPDY_BIDIRECTIONAL_STREAM, spdy_session_, request_info_->url,
      request_info_->priority, net_log,
      base::Bind(&BidirectionalStreamSpdyJob::OnStreamInitialized,
                 weak_factory_.GetWeakPtr()));
  if (rv != ERR_IO_PENDING)
    OnStreamInitialized(rv);
}

int BidirectionalStreamSpdyJob::ReadData(IOBuffer* buf, int buf_len) {
  if (stream_)
    DCHECK(!stream_->IsIdle());

  DCHECK(buf);
  DCHECK(buf_len);
  DCHECK(!timer_->IsRunning()) << "There should be only one ReadData in flight";

  // If there is data buffered, complete the IO immediately.
  if (!read_data_queue_.IsEmpty()) {
    return read_data_queue_.Dequeue(buf->data(), buf_len);
  } else if (stream_closed_) {
    return closed_stream_status_;
  }
  // Read will complete asynchronously and Delegate::OnReadCompleted will be
  // called upon completion.
  read_buffer_ = buf;
  read_buffer_len_ = buf_len;
  return ERR_IO_PENDING;
}

void BidirectionalStreamSpdyJob::SendData(IOBuffer* data,
                                          int length,
                                          bool end_stream) {
  DCHECK(!stream_closed_);
  DCHECK(stream_);

  stream_->SendData(data, length,
                    end_stream ? NO_MORE_DATA_TO_SEND : MORE_DATA_TO_SEND);
}

void BidirectionalStreamSpdyJob::Cancel() {
  if (!stream_)
    return;
  // Cancels the stream and detaches the delegate so it doesn't get called back.
  stream_->DetachDelegate();
  DCHECK(!stream_);
}

NextProto BidirectionalStreamSpdyJob::GetProtocol() const {
  return negotiated_protocol_;
}

int64_t BidirectionalStreamSpdyJob::GetTotalReceivedBytes() const {
  if (stream_closed_)
    return closed_stream_received_bytes_;

  if (!stream_)
    return 0;

  return stream_->raw_received_bytes();
}

int64_t BidirectionalStreamSpdyJob::GetTotalSentBytes() const {
  if (stream_closed_)
    return closed_stream_sent_bytes_;

  if (!stream_)
    return 0;

  return stream_->raw_sent_bytes();
}

void BidirectionalStreamSpdyJob::OnRequestHeadersSent() {
  DCHECK(stream_);

  negotiated_protocol_ = stream_->GetProtocol();
  delegate_->OnHeadersSent();
}

SpdyResponseHeadersStatus BidirectionalStreamSpdyJob::OnResponseHeadersUpdated(
    const SpdyHeaderBlock& response_headers) {
  DCHECK(stream_);

  delegate_->OnHeadersReceived(response_headers);
  return RESPONSE_HEADERS_ARE_COMPLETE;
}

void BidirectionalStreamSpdyJob::OnDataReceived(scoped_ptr<SpdyBuffer> buffer) {
  DCHECK(stream_);
  DCHECK(!stream_closed_);

  // If |buffer| is null, BidirectionalStreamSpdyJob::OnClose will be invoked by
  // SpdyStream to indicate the end of stream.
  if (!buffer)
    return;

  // When buffer is consumed, SpdyStream::OnReadBufferConsumed will adjust
  // recv window size accordingly.
  read_data_queue_.Enqueue(std::move(buffer));
  if (read_buffer_) {
    // Handing small chunks of data to the caller creates measurable overhead.
    // So buffer data in short time-spans and send a single read notification.
    ScheduleBufferedRead();
  }
}

void BidirectionalStreamSpdyJob::OnDataSent() {
  DCHECK(stream_);
  DCHECK(!stream_closed_);

  delegate_->OnDataSent();
}

void BidirectionalStreamSpdyJob::OnTrailers(const SpdyHeaderBlock& trailers) {
  DCHECK(stream_);
  DCHECK(!stream_closed_);

  delegate_->OnTrailersReceived(trailers);
}

void BidirectionalStreamSpdyJob::OnClose(int status) {
  DCHECK(stream_);

  stream_closed_ = true;
  closed_stream_status_ = status;
  closed_stream_received_bytes_ = stream_->raw_received_bytes();
  closed_stream_sent_bytes_ = stream_->raw_sent_bytes();
  stream_.reset();

  if (status != OK) {
    delegate_->OnFailed(status);
    return;
  }
  // Complete any remaining read, as all data has been buffered.
  // If user has not called ReadData (i.e |read_buffer_| is nullptr), this will
  // do nothing.
  timer_->Stop();
  DoBufferedRead();
}

void BidirectionalStreamSpdyJob::SendRequestHeaders() {
  scoped_ptr<SpdyHeaderBlock> headers(new SpdyHeaderBlock);
  HttpRequestInfo http_request_info;
  http_request_info.url = request_info_->url;
  http_request_info.method = request_info_->method;
  http_request_info.extra_headers = request_info_->extra_headers;

  CreateSpdyHeadersFromHttpRequest(
      http_request_info, http_request_info.extra_headers,
      stream_->GetProtocolVersion(), true, headers.get());
  stream_->SendRequestHeaders(std::move(headers),
                              request_info_->end_stream_on_headers
                                  ? NO_MORE_DATA_TO_SEND
                                  : MORE_DATA_TO_SEND);
}

void BidirectionalStreamSpdyJob::OnStreamInitialized(int rv) {
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv == OK) {
    stream_ = stream_request_.ReleaseStream();
    stream_->SetDelegate(this);
    SendRequestHeaders();
    return;
  }
  delegate_->OnFailed(rv);
}

void BidirectionalStreamSpdyJob::ScheduleBufferedRead() {
  // If there is already a scheduled DoBufferedRead, don't issue
  // another one. Mark that we have received more data and return.
  if (timer_->IsRunning()) {
    more_read_data_pending_ = true;
    return;
  }

  more_read_data_pending_ = false;
  timer_->Start(FROM_HERE, base::TimeDelta::FromMilliseconds(kBufferTimeMs),
                base::Bind(&BidirectionalStreamSpdyJob::DoBufferedRead,
                           weak_factory_.GetWeakPtr()));
}

void BidirectionalStreamSpdyJob::DoBufferedRead() {
  DCHECK(!timer_->IsRunning());
  // Check to see that the stream has not errored out.
  DCHECK(stream_ || stream_closed_);
  DCHECK(!stream_closed_ || closed_stream_status_ == OK);

  // When |more_read_data_pending_| is true, it means that more data has arrived
  // since started waiting. Wait a little longer and continue to buffer.
  if (more_read_data_pending_ && ShouldWaitForMoreBufferedData()) {
    ScheduleBufferedRead();
    return;
  }

  int rv = 0;
  if (read_buffer_) {
    rv = ReadData(read_buffer_.get(), read_buffer_len_);
    DCHECK_NE(ERR_IO_PENDING, rv);
    read_buffer_ = nullptr;
    read_buffer_len_ = 0;
    delegate_->OnDataRead(rv);
  }
}

bool BidirectionalStreamSpdyJob::ShouldWaitForMoreBufferedData() const {
  if (stream_closed_)
    return false;
  DCHECK_GT(read_buffer_len_, 0);
  return read_data_queue_.GetTotalSize() <
         static_cast<size_t>(read_buffer_len_);
}

}  // namespace net
